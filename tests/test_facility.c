/*
 * test_facility.c - 시설 프로세스를 실제로 띄워 백 타임, 이벤트 알림, 수리 확인, 정리를 본다.
 * 이 프로그램이 호스트 역할을 한다 (fac_mgr 사용, SIGUSR1 핸들러).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "fac_mgr.h"
#include "fixture.h"
#include "kvfile.h"
#include "repair_check.h"
#include "t.h"
#include "timeutil.h"

static char root[PATH_MAX];
static fac_mgr_t mgr;
static volatile sig_atomic_t usr1_count;
static char last_equip_note[FAC_COUNT][FAC_DETAIL_MAX];
static int equip_notes[FAC_COUNT];

static void on_usr1(int sig)
{
    (void)sig;
    usr1_count++;
}

static void note_equip(int fac, const fac_msg_t *msg)
{
    if (msg->type != FMSG_EQUIP)
        return;
    equip_notes[fac]++;
    snprintf(last_equip_note[fac], FAC_DETAIL_MAX, "%s", msg->detail);
}

/*
 * fac 에서 type 메시지가 올 때까지 읽는다 (다른 종류는 넘긴다). 왔으면 1.
 */
static int wait_msg_from(fac_mgr_t *m, int fac, int type, int timeout_ms, fac_msg_t *out)
{
    int64_t end = now_ms() + timeout_ms;

    for (;;) {
        fac_msg_t msg;
        fac_read_t r = fac_mgr_read(m, fac, &msg);
        struct pollfd pfd;
        int64_t left;

        if (r == FAC_GOT) {
            note_equip(fac, &msg);
            if (msg.type == type) {
                if (out != NULL)
                    *out = msg;
                return 1;
            }
            continue;
        }
        if (r == FAC_EOF)
            return 0;
        left = end - now_ms();
        if (left <= 0)
            return 0;
        pfd.fd = fac_mgr_fd(m, fac);
        pfd.events = POLLIN;
        poll(&pfd, 1, (int)(left > 50 ? 50 : left));
    }
}

static int wait_msg(int fac, int type, int timeout_ms, fac_msg_t *out)
{
    return wait_msg_from(&mgr, fac, type, timeout_ms, out);
}

static void drain(int fac)
{
    fac_msg_t tmp;

    while (fac_mgr_read(&mgr, fac, &tmp) == FAC_GOT)
        note_equip(fac, &tmp);
}

/* 쌓인 보고를 버리고 그 뒤에 새로 만든 STATUS 하나를 받는다 */
static int fresh_status(int fac, fac_msg_t *out)
{
    drain(fac);
    sleep_ms(FAC_TICK_MS + 50);
    drain(fac);
    return wait_msg(fac, FMSG_STATUS, 1500, out);
}

/* 수리 확인을 요청하고 답을 받는다 */
static fac_msg_t verify(int fac)
{
    fac_msg_t v;

    memset(&v, 0, sizeof v);
    v.verify_ok = -1;
    if (fac_mgr_request_verify(&mgr, fac) == 0)
        (void)wait_msg(fac, FMSG_VERIFY, 2000, &v);
    return v;
}

/* /proc/<pid>/stat 의 상태 글자. 프로세스가 없으면 0 */
static char proc_state(pid_t pid)
{
    char path[64], buf[512], *p;
    int fd;
    ssize_t n;

    if (pid <= 0)
        return 0;
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    p = strrchr(buf, ')');
    return (p != NULL && p[1] == ' ') ? p[2] : 0;
}

static int gone(pid_t pid)
{
    char s = proc_state(pid);
    return s == 0 || s == 'Z';
}

static int wait_gone(pid_t pid, int timeout_ms)
{
    int64_t end = now_ms() + timeout_ms;

    while (!gone(pid)) {
        if (now_ms() > end)
            return 0;
        sleep_ms(10);
    }
    return 1;
}

static void proc_comm(pid_t pid, char *out, size_t n)
{
    char path[64];
    int fd;
    ssize_t len;

    out[0] = '\0';
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    len = read(fd, out, n - 1);
    close(fd);
    if (len > 0 && out[len - 1] == '\n')
        len--;
    out[len > 0 ? len : 0] = '\0';
}

static const char *P(const char *rel)
{
    char *p = malloc(PATH_MAX);

    snprintf(p, PATH_MAX, "%s/%s", root, rel);
    return p;
}

/* ---------- 테스트 ---------- */
static void test_startup(void)
{
    fac_msg_t s;
    char comm[32], val[32];
    int i;

    SECTION("시작: 주기 보고와 설비 프로세스");
    for (i = 0; i < FAC_COUNT; i++) {
        CHECK(wait_msg(i, FMSG_STATUS, 2000, &s));
        CHECK(s.facility == i && s.pid == (int)mgr.fac[i].pid);
        CHECK(s.arm_state == ARM_IDLE && s.event == EV_NONE && s.fault == 0);
        CHECK(s.equip_pid > 0 && s.equip_state == EQ_RUNNING);
        CHECK(proc_state(s.equip_pid) != 0 && proc_state(s.equip_pid) != 'T');
        sleep_ms(30);                                   /* exec 직후 이름을 바꿀 틈 */
        proc_comm(s.equip_pid, comm, sizeof comm);
        CHECK(strcmp(comm, equip_key(i)) == 0);
        proc_comm(s.pid, comm, sizeof comm);
        CHECK(strncmp(comm, "fac-", 4) == 0);
        CHECK(equip_notes[i] >= 1);                     /* 설비 시작 알림 */
    }
    /* equip 실행 파일이 있으면 execv 로 뜨고, 없으면 fork 한 채로 돈다 */
    {
        const char *bin = getenv("PLANT_EQUIP_BIN");
        char link[64], exe[PATH_MAX];
        ssize_t n;

        snprintf(link, sizeof link, "/proc/%d/exe", s.equip_pid);
        n = readlink(link, exe, sizeof exe - 1);
        exe[n > 0 ? n : 0] = '\0';
        if (bin != NULL && access(bin, X_OK) == 0)
            CHECK(strcmp(exe, bin) == 0);
        else
            CHECK(strstr(exe, "test_facility") != NULL);
    }
    CHECK(wait_msg(FAC_COOLING, FMSG_STATUS, 2000, &s));
    CHECK(s.metrics[0] > 25.f && s.metrics[0] < 40.f);  /* 냉각수 온도 */
    CHECK(metric_level(FAC_COOLING, 2, s.metrics[2]) == 0);

    /* 설비 상태 파일의 pid=, state= 줄 (다른 줄은 그대로) */
    CHECK(kv_get(P("cooling/pump_status.txt"), "pid", val, sizeof val) == 0 && atoi(val) == s.equip_pid);
    CHECK(kv_get(P("cooling/pump_status.txt"), "state", val, sizeof val) == 0 && strcmp(val, "RUNNING") == 0);
    CHECK(kv_get(P("power/breaker_status.txt"), "breaker_id", val, sizeof val) == 0 && strcmp(val, "BK-7") == 0);
}

static void test_backtime_event(void)
{
    fac_msg_t s, e;
    int64_t t0;
    int before = usr1_count;

    SECTION("백 타임이 끝나면 FMSG_EVENT + SIGUSR1");
    CHECK(fac_mgr_arm(&mgr, FAC_COOLING, EV_G01, 500, 0) == -1);   /* 남의 이벤트는 받지 않는다 */
    t0 = now_ms();
    CHECK(fac_mgr_arm(&mgr, FAC_COOLING, EV_C01, 1500, 0) == 0);
    CHECK(fresh_status(FAC_COOLING, &s));
    CHECK(s.arm_state == ARM_ARMED && s.event == EV_C01 && s.fault == 0);
    CHECK(s.backtime_total_ms == 1500 && s.backtime_left_ms > 0 && s.backtime_left_ms < 1500);
    CHECK(wait_msg(FAC_COOLING, FMSG_EVENT, 2000, &e));
    CHECK(now_ms() - t0 >= 1450 && now_ms() - t0 < 2400);
    CHECK(e.event == EV_C01 && e.arm_state == ARM_FIRED && e.fault == 1);
    CHECK(e.equip_state == EQ_RUNNING);                 /* C01 은 파일 고장뿐 */
    sleep_ms(20);
    CHECK(usr1_count > before);

    /* 한 번 터진 뒤에는 다시 터지지 않는다 */
    CHECK(!wait_msg(FAC_COOLING, FMSG_EVENT, 600, NULL));
}

static void test_verify_file_event(void)
{
    fac_msg_t v, s;
    int before = usr1_count;

    SECTION("SIGUSR2 로 수리 확인 (파일 고장)");
    CHECK(fixture_break(root, EV_C01) == 0);            /* 호스트가 넣는 파일 고장 */
    CHECK(fresh_status(FAC_COOLING, &s));
    CHECK(metric_level(FAC_COOLING, 2, s.metrics[2]) > 0);   /* 유량이 떨어져 보인다 */

    v = verify(FAC_COOLING);
    CHECK(v.verify_ok == 0 && v.verify_failed == CHK_CFG_RANGE && v.detail[0] != '\0');
    CHECK(v.event == EV_C01 && v.fault == 1 && v.arm_state == ARM_FIRED && v.repairs == 0);
    sleep_ms(20);
    CHECK(usr1_count > before);

    CHECK(fixture_fix(root, EV_C01) == 0);
    v = verify(FAC_COOLING);
    CHECK(v.verify_ok == 1 && v.verify_failed == CHK_NONE && v.event == EV_C01);
    CHECK(v.fault == 0 && v.arm_state == ARM_IDLE && v.repairs == 1);

    CHECK(fresh_status(FAC_COOLING, &s));
    CHECK(s.fault == 0 && s.event == EV_NONE && s.repairs == 1);
    sleep_ms(1500);
    CHECK(fresh_status(FAC_COOLING, &s));
    CHECK(metric_level(FAC_COOLING, 2, s.metrics[2]) == 0);  /* 수치도 돌아온다 */

    v = verify(FAC_COOLING);                            /* 이벤트가 없을 때의 확인 요청 */
    CHECK(v.verify_ok == 0 && v.verify_failed == CHK_NO_EVENT && v.repairs == 1);
}

static void test_pause_resume(void)
{
    fac_msg_t s;

    SECTION("백 타임 멈춤과 재개");
    CHECK(fac_mgr_arm(&mgr, FAC_POWER, EV_P01, 700, 0) == 0);
    sleep_ms(250);
    fac_mgr_pause_others(&mgr, FAC_COOLING);
    CHECK(!wait_msg(FAC_POWER, FMSG_EVENT, 1200, NULL));     /* 멈춘 동안에는 터지지 않는다 */
    CHECK(fresh_status(FAC_POWER, &s));
    CHECK(s.arm_state == ARM_PAUSED && s.backtime_left_ms > 100 && s.backtime_left_ms < 500);
    fac_mgr_resume_others(&mgr, FAC_COOLING);
    CHECK(wait_msg(FAC_POWER, FMSG_EVENT, 1000, &s));        /* 남은 시간부터 이어서 */
    CHECK(s.event == EV_P01);

    CHECK(fac_mgr_disarm(&mgr, FAC_POWER) == 0);             /* 이벤트를 걷어 낸다 */
    CHECK(fresh_status(FAC_POWER, &s));
    CHECK(s.arm_state == ARM_IDLE && s.fault == 0 && s.event == EV_NONE);
}

static void test_proc_stop(void)
{
    fac_msg_t e, v, s;
    char val[32];
    pid_t pid;

    SECTION("프로세스 고장: 정지 -> SIGCONT 로 재개");
    CHECK(fac_mgr_arm(&mgr, FAC_COOLING, EV_C02, 100, PF_STOP) == 0);
    CHECK(wait_msg(FAC_COOLING, FMSG_EVENT, 2000, &e));
    pid = e.equip_pid;
    CHECK(e.event == EV_C02 && e.equip_state == EQ_STOPPED);
    CHECK(proc_state(pid) == 'T');                           /* 실제로 멈춰 있다 */
    CHECK(strstr(last_equip_note[FAC_COOLING], "정지") != NULL);
    CHECK(kv_get(P("cooling/pump_status.txt"), "state", val, sizeof val) == 0 && strcmp(val, "STOPPED") == 0);
    CHECK(fresh_status(FAC_COOLING, &s));
    sleep_ms(1200);
    CHECK(fresh_status(FAC_COOLING, &s));
    CHECK(s.metrics[2] < 20.f);                              /* 펌프가 서서 유량이 없다 */

    v = verify(FAC_COOLING);
    CHECK(v.verify_ok == 0 && v.verify_failed == CHK_EQUIP);

    /* resume: 호스트가 설비에 직접 SIGCONT 를 보내고 곧바로 확인을 요청해도 된다 */
    CHECK(fac_mgr_equip_signal(&mgr, FAC_COOLING, SIGCONT) == 0);
    v = verify(FAC_COOLING);
    CHECK(v.verify_ok == 1 && v.equip_state == EQ_RUNNING && v.equip_pid == (int)pid);
    CHECK(proc_state(pid) != 'T' && proc_state(pid) != 0);
    CHECK(v.repairs == 2);
}

static void test_proc_kill(void)
{
    fac_msg_t e, v;
    pid_t old;

    SECTION("프로세스 고장: 종료 -> 재실행");
    CHECK(fac_mgr_arm(&mgr, FAC_GENERATOR, EV_G02, 100, PF_KILL) == 0);
    CHECK(wait_msg(FAC_GENERATOR, FMSG_EVENT, 2000, &e));
    old = e.equip_pid;
    CHECK(e.event == EV_G02 && e.equip_state == EQ_DEAD);
    CHECK(wait_gone(old, 500));
    CHECK(fac_mgr_equip_signal(&mgr, FAC_GENERATOR, SIGCONT) == -1 && errno == ESRCH);

    v = verify(FAC_GENERATOR);
    CHECK(v.verify_ok == 0 && v.verify_failed == CHK_EQUIP);

    /* restart: 제어 메시지를 쓰고 곧바로 확인을 요청해도 순서가 지켜진다 */
    CHECK(fac_mgr_equip_restart(&mgr, FAC_GENERATOR) == 0);
    v = verify(FAC_GENERATOR);
    CHECK(v.verify_ok == 1 && v.equip_state == EQ_RUNNING);
    CHECK(v.equip_pid > 0 && v.equip_pid != (int)old && !gone(v.equip_pid));
}

static void test_final_event(void)
{
    fac_msg_t e, v, s;

    SECTION("G03: 권한을 고쳐야 설비가 뜬다");
    CHECK(fac_mgr_arm(&mgr, FAC_GENERATOR, EV_G03, 0, 0) == 0);   /* 백 타임 0 이면 바로 */
    CHECK(wait_msg(FAC_GENERATOR, FMSG_EVENT, 2000, &e));
    CHECK(e.event == EV_G03 && e.equip_state == EQ_DEAD);
    CHECK(fixture_break(root, EV_G03) == 0);

    CHECK(fac_mgr_equip_restart(&mgr, FAC_GENERATOR) == 0);       /* 권한이 막혀 있어 거부된다 */
    CHECK(fresh_status(FAC_GENERATOR, &s));
    CHECK(s.equip_state == EQ_DEAD);
    CHECK(strstr(last_equip_note[FAC_GENERATOR], "generator_config.cfg") != NULL);
    CHECK(s.metrics[1] < 50.f);                                   /* 출력 없음 */
    v = verify(FAC_GENERATOR);
    CHECK(v.verify_ok == 0 && v.verify_failed == CHK_MODE);

    CHECK(fixture_fix(root, EV_G03) == 0);
    v = verify(FAC_GENERATOR);
    CHECK(v.verify_ok == 0 && v.verify_failed == CHK_EQUIP);      /* 권한만 고쳐서는 부족하다 */
    CHECK(fac_mgr_equip_restart(&mgr, FAC_GENERATOR) == 0);
    v = verify(FAC_GENERATOR);
    CHECK(v.verify_ok == 1 && v.equip_state == EQ_RUNNING);
}

static void test_sensor_events(void)
{
    fac_msg_t e, v, s;
    struct stat st;
    char val[32];

    SECTION("센서 링크 이벤트와 센서 파일 갱신");
    CHECK(fac_mgr_arm(&mgr, FAC_GENERATOR, EV_G01, 0, 0) == 0);
    CHECK(wait_msg(FAC_GENERATOR, FMSG_EVENT, 2000, &e));
    CHECK(fixture_break(root, EV_G01) == 0);
    CHECK(fresh_status(FAC_GENERATOR, &s));
    CHECK(isnan(s.metrics[0]) && !isnan(s.metrics[1]));           /* 온도만 수신 불가 */
    v = verify(FAC_GENERATOR);
    CHECK(v.verify_ok == 0 && v.verify_failed == CHK_LINK);
    CHECK(fixture_fix(root, EV_G01) == 0);
    v = verify(FAC_GENERATOR);
    CHECK(v.verify_ok == 1);
    CHECK(fresh_status(FAC_GENERATOR, &s));
    CHECK(!isnan(s.metrics[0]));

    CHECK(fac_mgr_arm(&mgr, FAC_COOLING, EV_C03, 0, 0) == 0);
    CHECK(wait_msg(FAC_COOLING, FMSG_EVENT, 2000, &e));
    CHECK(fixture_break(root, EV_C03) == 0);
    CHECK(fresh_status(FAC_COOLING, &s));
    CHECK(fabsf(s.metrics[0] - 18.4f) < 0.01f);                   /* 옛 파일의 멈춘 값 */
    v = verify(FAC_COOLING);
    CHECK(v.verify_ok == 0 && v.verify_failed == CHK_LINK);
    CHECK(fixture_fix(root, EV_C03) == 0);
    v = verify(FAC_COOLING);
    CHECK(v.verify_ok == 1);                                      /* 링크가 맞고 데이터가 갱신 중 */

    /* 시설이 사용 중인 센서 파일에 값을 계속 쓴다. 다른 줄과 옛 센서 파일은 그대로 */
    CHECK(stat(P("cooling/cooling_sensor_b.dat"), &st) == 0 && time(NULL) - st.st_mtime <= 3);
    CHECK(kv_get(P("cooling/cooling_sensor_b.dat"), "sensor_id", val, sizeof val) == 0 && strcmp(val, "CS-B") == 0);
    CHECK(kv_get(P("cooling/cooling_sensor_b.dat"), "value", val, sizeof val) == 0 && atof(val) > 25.0);
    CHECK(stat(P("cooling/cooling_sensor_a.dat"), &st) == 0 && time(NULL) - st.st_mtime > 86400);
}

static void test_link_level(void)
{
    fac_msg_t s;

    SECTION("연계 고장 레벨");
    CHECK(fac_mgr_link(&mgr, FAC_GENERATOR, 10) == 0);
    sleep_ms(1500);
    CHECK(fresh_status(FAC_GENERATOR, &s));
    CHECK(s.link_level == 10 && metric_level(FAC_GENERATOR, 0, s.metrics[0]) == 2);
    CHECK(fac_mgr_link(&mgr, FAC_GENERATOR, 0) == 0);
    sleep_ms(1500);
    CHECK(fresh_status(FAC_GENERATOR, &s));
    CHECK(s.link_level == 0 && metric_level(FAC_GENERATOR, 0, s.metrics[0]) == 0);
}

static void test_shutdown(void)
{
    pid_t fac[FAC_COUNT], eq[FAC_COUNT];
    fac_msg_t s;
    char val[32];
    int i;

    SECTION("종료: 좀비도 고아도 남지 않는다");
    CHECK(fac_mgr_equip_signal(&mgr, FAC_POWER, SIGSTOP) == 0);   /* 멈춘 설비가 있어도 정리된다 */
    for (i = 0; i < FAC_COUNT; i++) {
        CHECK(fresh_status(i, &s));
        fac[i] = (pid_t)s.pid;
        eq[i] = (pid_t)s.equip_pid;
    }
    CHECK(proc_state(eq[FAC_POWER]) == 'T');
    fac_mgr_shutdown(&mgr);
    for (i = 0; i < FAC_COUNT; i++) {
        CHECK(wait_gone(fac[i], 500));
        CHECK(wait_gone(eq[i], 500));
        CHECK(fac_mgr_fd(&mgr, i) == -1);
    }
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    CHECK(kv_get(P("power/breaker_status.txt"), "state", val, sizeof val) == 0 && strcmp(val, "DEAD") == 0);
    fac_mgr_shutdown(&mgr);                                       /* 두 번 불러도 된다 */
}

/* 호스트가 정리 없이 죽어도 시설과 설비(멈춘 것 포함)가 따라 죽는다 */
static void test_host_death(void)
{
    int pfd[2];
    pid_t host, pids[FAC_COUNT * 2];
    ssize_t n;
    int i, status;

    SECTION("호스트가 갑자기 죽을 때");
    CHECK(pipe(pfd) == 0);
    host = fork();
    if (host == 0) {
        fac_mgr_t m;
        fac_msg_t s;

        close(pfd[0]);
        if (fac_mgr_init(&m, root) != 0 || fac_mgr_spawn_all(&m) != 0)
            _exit(1);
        if (fac_mgr_arm(&m, FAC_COOLING, EV_C02, 0, PF_STOP) != 0 ||
            !wait_msg_from(&m, FAC_COOLING, FMSG_EVENT, 2000, NULL))
            _exit(2);
        for (i = 0; i < FAC_COUNT; i++) {
            if (!wait_msg_from(&m, i, FMSG_STATUS, 2000, &s))
                _exit(3);
            pids[i * 2] = (pid_t)s.pid;
            pids[i * 2 + 1] = (pid_t)s.equip_pid;
        }
        if (write(pfd[1], pids, sizeof pids) != (ssize_t)sizeof pids)
            _exit(4);
        _exit(0);                                       /* fac_mgr_shutdown 없이 끝낸다 */
    }
    close(pfd[1]);
    n = read(pfd[0], pids, sizeof pids);
    close(pfd[0]);
    CHECK(waitpid(host, &status, 0) == host && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(n == (ssize_t)sizeof pids);
    if (n == (ssize_t)sizeof pids)
        for (i = 0; i < FAC_COUNT * 2; i++)
            CHECK(pids[i] > 0 && wait_gone(pids[i], 2000));
}

int main(void)
{
    struct sigaction sa;

    snprintf(root, sizeof root, "/tmp/plant_test_fac_%d", (int)getpid());
    if (fixture_create(root) != 0) {
        perror("fixture_create");
        return 1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    if (fac_mgr_init(&mgr, root) != 0 || fac_mgr_spawn_all(&mgr) != 0) {
        perror("fac_mgr");
        return 1;
    }
    test_startup();
    test_backtime_event();
    test_verify_file_event();
    test_pause_resume();
    test_proc_stop();
    test_proc_kill();
    test_final_event();
    test_sensor_events();
    test_link_level();
    test_shutdown();
    test_host_death();

    fixture_remove(root);
    return t_done();
}
