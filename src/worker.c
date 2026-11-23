#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <errno.h>
#include <ftw.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "fac_mgr.h"
#include "repair_check.h"
#include <time.h>
#include <sys/file.h>
#include "kvfile.h"
#include "timeutil.h"

#define ROOT "plant"
#define COOL ROOT "/cooling/"
#define POWER ROOT "/power/"
#define GEN ROOT "/generator/"
#define GEN_CFG GEN "generator_config.cfg"
#define GEN_LINK GEN "temperature_sensor_link"
#define TEMP_A "temp_sensor_a.dat"
#define TEMP_B "temp_sensor_b.dat"
#define FLOW_CFG COOL "coolant_config.cfg"
#define FLOW_LEVEL COOL "coolant_level.txt"
#define POWER_CFG POWER "power_config.cfg"
#define SENSOR_LINK COOL "cooling_sensor_link"
#define SENSOR_A "cooling_sensor_a.dat"
#define SENSOR_B "cooling_sensor_b.dat"

#define BACKTIME_MS 10000
#define REPAIR_MS 40000

static const int events[] = {
    EV_C01, EV_C02, EV_C03, EV_P01, EV_P02, EV_P03,
    EV_G01, EV_G02, EV_G03, EV_NONE
};

typedef struct {
    fac_mgr_t mgr;
    int step, active, location, pending, breaker_wait;
    int pump_wait, turbine_wait, recovering, solo, variant;
    int64_t reported_at[FAC_COUNT];
    int64_t deadline;
} worker_t;

static volatile sig_atomic_t quitting;
static struct termios saved_tty;
static int editing_tty;

static int begin_input(void)
{
    struct termios mode;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return 0;
    if (tcgetattr(STDIN_FILENO, &saved_tty) < 0)
        return -1;

    mode = saved_tty;
    mode.c_lflag &= ~(ICANON | ECHO | ECHONL | IEXTEN);
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &mode) < 0)
        return -1;

    editing_tty = 1;
    return 0;
}

static void clear_input_line(void)
{
    if (editing_tty)
        fputs("\r\033[2K", stdout);
}

static void draw_input(const char *line, size_t used, int location)
{
    const char *prompt = location == FAC_GENERATOR ? "발전실 직원> " : "발전소 직원> ";
    if (editing_tty) {
        struct winsize size;
        size_t start = 0;

        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 &&
            size.ws_col > 14 && used > (size_t)(size.ws_col - 14)) {
            start = used - (size_t)(size.ws_col - 14);
            while (start < used && ((unsigned char)line[start] & 0xc0) == 0x80)
                start++;
        }

        clear_input_line();
        fputs(prompt, stdout);
        fwrite(line + start, 1, used - start, stdout);
    } else {
        fputs(prompt, stdout);
    }
    fflush(stdout);
}

static void on_signal(int sig)
{
    /* SIGUSR1은 poll을 깨우고, 실제 보고는 파이프에서 읽는다. */
    if (sig != FAC_SIG_NOTIFY)
        quitting = 1;
}

static int install_signals(void)
{
    struct sigaction sa = {0};
    const int signals[] = {FAC_SIG_NOTIFY, SIGINT, SIGTERM, SIGHUP};

    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
        if (sigaction(signals[i], &sa, NULL) < 0)
            return -1;
    return 0;
}

static int set_number(const char *path, const char *key, int number)
{
    char value[16];

    snprintf(value, sizeof value, "%d", number);
    if (kv_set(path, key, value) < 0) {
        perror(path);
        return -1;
    }
    return 0;
}

static int set_pair(const char *path1, const char *key1, int a,
                    const char *path2, const char *key2, int b)
{
    if (set_number(path1, key1, a) < 0)
        return -1;
    return set_number(path2, key2, b);
}

static int set_flow(int value)
{
    return set_number(FLOW_CFG, "target_flow", value);
}

static int set_load(int a, int b)
{
    if (set_pair(POWER_CFG, "circuit_a_load", a, POWER_CFG, "circuit_b_load", b) < 0 ||
        set_pair(POWER "load_status.txt", "circuit_a", a, POWER "load_status.txt", "circuit_b", b) < 0 ||
        kv_set(POWER "circuit_status.txt", "circuit_a", a > 80 ? "overload" : "normal") < 0)
        return -1;
    return kv_set(POWER "circuit_status.txt", "circuit_b", b > 80 ? "overload" : "normal");
}

static int set_split(int generator, int cooling)
{
    return set_pair(POWER_CFG, "supply_generator", generator,
                    POWER_CFG, "supply_cooling", cooling);
}

static int relink_sensor(const char *path, const char *target)
{
    struct stat st;
    char temp[PATH_MAX];

    if (lstat(path, &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            errno = EEXIST;
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    snprintf(temp, sizeof temp, "%s.tmp.%ld", path, (long)getpid());
    if (symlink(target, temp) < 0)
        return -1;
    if (rename(temp, path) < 0) {
        int saved = errno;
        unlink(temp);
        errno = saved;
        return -1;
    }
    return 0;
}

static int set_generator_mode(mode_t mode)
{
    struct stat st;
    if (chmod(GEN_CFG, mode) < 0 || stat(GEN_CFG, &st) < 0)
        return -1;
    mode_t actual = st.st_mode & 07777;
    if (actual == mode)
        return 0;
    fprintf(stderr, "%s 권한 변경 실패: 요청 %04o, 실제 %04o. "
            "Linux 권한을 지원하는 파일시스템(예: WSL 홈)에 프로젝트를 복사해 실행하세요.\n",
            GEN_CFG, (unsigned)mode, (unsigned)actual);
    errno = ENOTSUP;
    return -1;
}

static int inject_fault(int event)
{
    switch (event) {
    case EV_G01: return relink_sensor(GEN_LINK, TEMP_A);
    case EV_G02: return 0;
    case EV_G03: return set_generator_mode(0000);
    case EV_C01:
        return set_flow(40);
    case EV_C02:
        return 0; /* 시설이 펌프 프로세스에 고장을 적용한다. */
    case EV_C03:
        return relink_sensor(SENSOR_LINK, SENSOR_A);
    case EV_P01:
        return set_load(118, 12);
    case EV_P02:
        if (kv_set(POWER "circuit_status.txt", "circuit_a", "fault") < 0)
            return -1;
        return set_number(POWER_CFG, "fault_isolated", 0);
    case EV_P03:
        return set_split(75, 25);
    default:
        errno = EINVAL;
        return -1;
    }
}

static int arm_next(worker_t *w, int delay)
{
    int event = events[w->step];

    if (event == EV_NONE)
        return 0;
    return fac_mgr_arm(&w->mgr, event_facility(event), event, delay, w->variant);
}

static void send_verify(worker_t *w)
{
    if (fac_mgr_request_verify(&w->mgr, event_facility(w->active)) < 0) {
        perror("수리 확인 요청");
        return;
    }
    w->pending = 1;
}

/* Existing UI normal bands also drive console recovery decisions. */
#define GEN_LIMIT_KW 2184
#define COOL_LIMIT_KW 3276

static int power_normal(const fac_msg_t *m)
{
    return metric_level(FAC_POWER, 1, m->metrics[1]) == 0 &&
           m->metrics[1] <= 5460;
}

static int branches_normal(const fac_msg_t *m)
{
    return m->delivered_kw[0] >= 1880 && m->delivered_kw[0] <= GEN_LIMIT_KW &&
           m->delivered_kw[1] >= 2820 && m->delivered_kw[1] <= COOL_LIMIT_KW;
}

static const char *recovery_reason(worker_t *w)
{
    int fac = event_facility(w->active);
    if (fac < 0 || fac >= FAC_COUNT) return "시설 상태";
    const fac_msg_t *m = &w->mgr.fac[fac].last;
    switch (w->active) {
    case EV_C01:
    case EV_C02:
        if (metric_level(fac, 2, m->metrics[2]) ||
            (w->active == EV_C01 && metric_level(fac, 3, m->metrics[3])))
            return "냉각수 유량·효율";
        break;
    case EV_P02:
    case EV_P03:
        if (!power_normal(m))
            return "배전실 총공급 전력";
        if (w->active == EV_P03 && !branches_normal(m))
            return "분기별 공급 전력";
        break;
    case EV_G03:
        for (int i = FAC_COOLING; i <= FAC_POWER; ++i) {
            const fac_msg_t *peer = &w->mgr.fac[i].last;
            if (!w->mgr.fac[i].has_last || now_ms() - w->reported_at[i] > 2000 ||
                peer->repairs < 3 || peer->fault || peer->equip_state != EQ_RUNNING)
                return facility_name(i);
            for (int j = 0; j < METRIC_COUNT; ++j)
                if (metric_level(i, j, peer->metrics[j]))
                    return facility_name(i);
            if (i == FAC_POWER && (!power_normal(peer) || !branches_normal(peer)))
                return facility_name(i);
        }
        /* fall through */
    case EV_G02:
        if (metric_level(fac, 1, m->metrics[1]))
            return "발전기 출력";
        break;
    }
    return NULL;
}

static void request_verify(worker_t *w)
{
    repair_result_t r;
    int fac = event_facility(w->active);
    if (fac < 0 || fac >= FAC_COUNT) return;
    repair_verify(ROOT, w->active, w->mgr.fac[fac].last.equip_state, &r);
    if (!r.ok && r.failed == CHK_FRESH) {
        if (!w->recovering) puts("회복 확인 중: 현용 센서의 다음 갱신 대기");
        w->recovering = 1;
        return;
    }
    if (!r.ok) {
        w->recovering = 0;
        printf("추가 조작 필요: %s\n", r.detail);
        return;
    }
    /* A cached reading can predate the configuration write. Wait for the
       next periodic measurement before accepting either new setpoint. */
    if (!w->recovering && (w->active == EV_C01 || w->active == EV_P03)) {
        puts("회복 확인 중: 새 설정 적용 후 계측 보고 대기");
        w->recovering = 1;
        return;
    }
    if (w->active == EV_P02) {
        char state[32];
        if (kv_get(POWER "circuit_status.txt", "circuit_a", state, sizeof state) ||
            strcmp(state, "isolated")) {
            puts("추가 조작 필요: CR-A1 격리");
            w->recovering = 0;
            return;
        }
    }
    const char *why = recovery_reason(w);
    if (why) {
        if (!w->recovering)
            printf("회복 확인 중: %s\n", why);
        w->recovering = 1;
        return;
    }
    w->recovering = 0;
    send_verify(w);
}

static void recover_pump(worker_t *w, int state)
{
    printf("C02 펌프 상태: %s\n", equip_state_key(state));
    switch (state) {
    case EQ_STOPPED:
        if (fac_mgr_equip_signal(&w->mgr, FAC_COOLING, SIGCONT) < 0) {
            perror("C02 펌프 재개");
            return;
        }
        break;
    case EQ_DEAD:
        if (fac_mgr_equip_restart(&w->mgr, FAC_COOLING) < 0) {
            perror("C02 펌프 재실행");
            return;
        }
        break;
    case EQ_RUNNING:
        request_verify(w);
        return;
    default:
        puts("C02 상태 조회 실패: 알 수 없는 펌프 상태입니다.");
        return;
    }
    w->pump_wait = 1;
    puts("C02 복구 요청 완료. 시설의 RUNNING 보고를 기다립니다.");
}

/* -1: 오류, 0: 프롬프트 유지, 1: 프롬프트 다시 표시 */
static int handle_report(worker_t *w, int fac, const fac_msg_t *msg)
{
    int event = msg->event;

    w->reported_at[fac] = now_ms();
    if (w->recovering && msg->type == FMSG_STATUS && fac == event_facility(w->active) && event == w->active)
        request_verify(w);

    if (msg->type == FMSG_EVENT && w->active == EV_NONE &&
        event != EV_NONE && event == events[w->step] &&
        fac == event_facility(event)) {
        if (inject_fault(event) < 0) {
            fprintf(stderr, "%s 고장 적용 실패\n", event_code(event));
            return -1;
        }
        if ((event == EV_C01 && fac_mgr_link(&w->mgr, FAC_GENERATOR, 6) < 0) ||
            (event == EV_P01 && fac_mgr_link(&w->mgr, FAC_COOLING, 6) < 0))
            return -1;
        fac_mgr_pause_others(&w->mgr, fac);
        w->active = event;
        w->deadline = now_ms() + REPAIR_MS;

        clear_input_line();
        printf("\n%s 발생: %s (제한 %d초)\n",
               event_code(event), event_title(event), REPAIR_MS / 1000);
        if (event == EV_G02 || event == EV_G03)
            printf("터빈 상태: %s\n", equip_state_key(msg->equip_state));
        puts("현장 조사: 해당 시설로 이동해 help, status, cat 명령을 사용하세요.");
        return 1;
    }

    if (w->active == EV_C02 && event == EV_C02 && fac == FAC_COOLING &&
        (msg->type == FMSG_STATUS || msg->type == FMSG_EQUIP) &&
        w->pump_wait && msg->equip_state == EQ_RUNNING) {
        clear_input_line();
        w->pump_wait = 0;
        request_verify(w);
        return 1;
    }

    if (w->turbine_wait && fac == FAC_GENERATOR && event == w->active &&
        (msg->type == FMSG_STATUS || msg->type == FMSG_EQUIP)) {
        if (msg->equip_state == EQ_RUNNING) {
            clear_input_line();
            puts("시설 보고: 터빈 RUNNING");
            w->turbine_wait = 0;
            request_verify(w);
            return 1;
        }
        if (msg->type == FMSG_EQUIP && msg->equip_state == EQ_DEAD) {
            clear_input_line();
            puts(msg->detail);
            w->turbine_wait = 0;
            return 1;
        }
    }

    /* P02는 차단기가 가동 중이라는 보고를 받은 뒤 판정을 요청한다. */
    if (w->active == EV_P02 && fac == FAC_POWER && event == EV_P02 &&
        w->breaker_wait &&
        (msg->type == FMSG_STATUS || msg->type == FMSG_EQUIP) &&
        msg->equip_state == EQ_RUNNING) {
        w->breaker_wait = 0;
        request_verify(w);
    }

    if (msg->type == FMSG_VERIFY && w->pending && event == w->active &&
        fac == event_facility(event)) {
        w->pending = 0;
        clear_input_line();

        if (!msg->verify_ok) {
            printf("\n%s 수리 미완료: %s\n", event_code(event), msg->detail);
            return 1;
        }

        printf("\n%s 수리 완료\n", event_code(event));
        if ((event == EV_C01 && fac_mgr_link(&w->mgr, FAC_GENERATOR, 0) < 0) ||
            (event == EV_P01 && fac_mgr_link(&w->mgr, FAC_COOLING, 0) < 0))
            return -1;
        w->active = EV_NONE;
        w->step++;
        fac_mgr_resume_others(&w->mgr, fac);

        if (arm_next(w, BACKTIME_MS) < 0) {
            if (events[w->step] == EV_C02) {
                perror("C02 예약 실패 (debug next로 재시도)");
                return 1;
            }
            perror("다음 이벤트 예약");
            return -1;
        }
        if (events[w->step] == EV_NONE)
            puts("냉각실·배전실·발전실 이벤트를 모두 수리했습니다.");
        return 1;
    }
    return 0;
}

static void show_file(const char *path)
{
    FILE *f = fopen(path, "r");
    int ch, last = '\n';

    if (!f) {
        perror(path);
        return;
    }
    if (flock(fileno(f), LOCK_SH) < 0) {
        perror(path);
        fclose(f);
        return;
    }
    while ((ch = fgetc(f)) != EOF) {
        putchar(ch);
        last = ch;
    }
    if (last != '\n')
        putchar('\n');
    if (ferror(f))
        perror(path);
    fclose(f);
}

static void cat_file(int location, const char *name)
{
    const char *path = NULL;

    static const char *const files[] = {
        GEN_CFG, GEN "generator_label.txt", GEN "generator_status.txt",
        GEN "turbine_status.txt", GEN TEMP_A, GEN TEMP_B, GEN_LINK
    };
    if (location == FAC_GENERATOR) {
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
            if (!strcmp(name, strrchr(files[i], '/') + 1))
                path = files[i];
    } else if (location == FAC_COOLING) {
        if (!strcmp(name, "pump_status.txt")) path = COOL "pump_status.txt";
        else if (!strcmp(name, SENSOR_A)) path = COOL SENSOR_A;
        else if (!strcmp(name, SENSOR_B)) path = COOL SENSOR_B;
        else if (!strcmp(name, "pump_label.txt"))
            path = COOL "pump_label.txt";
        else if (!strcmp(name, "coolant_config.cfg"))
            path = FLOW_CFG;
        else if (!strcmp(name, "coolant_level.txt"))
            path = FLOW_LEVEL;
    } else if (location == FAC_POWER) {
        if (!strcmp(name, "circuit_status.txt")) path = POWER "circuit_status.txt";
        else if (!strcmp(name, "load_status.txt")) path = POWER "load_status.txt";
        else if (!strcmp(name, "power_config.cfg"))
            path = POWER_CFG;
        else if (!strcmp(name, "breaker_status.txt"))
            path = POWER "breaker_status.txt";
        else if (!strcmp(name, "circuit_label.txt"))
            path = POWER "circuit_label.txt";
    }

    if (path && strstr(name, ".dat")) {
        struct stat st;
        if (!stat(path, &st))
            printf("mtime=%lld (%lld초 전)\n", (long long)st.st_mtime,
                   (long long)(time(NULL) - st.st_mtime));
    }
    if (path && !strcmp(path, GEN_LINK)) {
        char target[128];
        ssize_t n = readlink(path, target, sizeof target - 1);
        path = NULL;
        if (n >= 0) {
            target[n] = 0;
            if (!strcmp(target, TEMP_A)) path = GEN TEMP_A;
            if (!strcmp(target, TEMP_B)) path = GEN TEMP_B;
        }
    }
    if (path)
        show_file(path);
    else
        puts("현재 위치에서 열람할 수 없는 파일입니다.");
}

static void show_sensor_link(const char *path)
{
    char target[256];
    ssize_t n = readlink(path, target, sizeof target - 1);

    if (n < 0) {
        perror(path);
        return;
    }
    target[n] = '\0';
    printf("%s -> %s\n", strrchr(path, '/') + 1, target);
}

static int number(const char *str, int max, int *out)
{
    char *end;

    errno = 0;
    long value = strtol(str, &end, 10);
    if (errno || end == str || *end || value < 0 || value > max)
        return 0;

    *out = (int)value;
    return 1;
}

static void repair(worker_t *w, char *v[], int n)
{
    int a, b, expected;

    if (n == 2 && !strcmp(v[1], "generator_config")) {
        if (w->location != FAC_GENERATOR || w->active != EV_G03 ||
            w->pending || w->turbine_wait)
            puts("발전실에서 G03 수리 중에 사용하세요.");
        else if (set_generator_mode(0640) < 0)
            perror(GEN_CFG);
        else {
            puts("권한 복구 완료, 터빈 재실행 필요");
        }
        return;
    }
    if (n < 3)
        goto usage;

    if (!strcmp(v[1], "coolant_flow"))
        expected = EV_C01;
    else if (!strcmp(v[1], "temperature_sensor"))
        expected = EV_G01;
    else if (!strcmp(v[1], "cooling_sensor"))
        expected = EV_C03;
    else if (!strcmp(v[1], "circuit_load"))
        expected = EV_P01;
    else if (!strcmp(v[1], "fault_isolated"))
        expected = EV_P02;
    else if (!strcmp(v[1], "power_split"))
        expected = EV_P03;
    else
        goto usage;

    if (w->location != event_facility(expected) ||
        (expected != EV_C01 && expected != w->active)) {
        puts("해당 시설로 이동해서 이벤트가 진행 중일 때 수리하세요.");
        return;
    }
    if (w->pending || w->breaker_wait) {
        puts("이전 수리의 확인을 기다리는 중입니다.");
        return;
    }

    switch (expected) {
    case EV_G01:
        if (n != 3 || (strcmp(v[2], TEMP_A) && strcmp(v[2], TEMP_B)))
            goto usage;
        if (relink_sensor(GEN_LINK, v[2]) < 0) {
            perror(GEN_LINK);
            return;
        }
        show_sensor_link(GEN_LINK);
        break;
    case EV_C01:
        if (n != 3 || !number(v[2], 100, &a))
            goto usage;
        if (set_flow(a) < 0)
            return;
        printf("설정한 목표 유량: %d\n", a);
        if (w->active == EV_C01)
            request_verify(w);
        return;

    case EV_C03:
        if (n != 3 || (strcmp(v[2], "A") && strcmp(v[2], "B")))
            goto usage;
        if (relink_sensor(SENSOR_LINK, !strcmp(v[2], "A") ? SENSOR_A : SENSOR_B) < 0) {
            perror(SENSOR_LINK);
            return;
        }
        printf("연결한 센서: %s\n", v[2]);
        break;

    case EV_P01:
        if (n != 4 || !number(v[2], 130, &a) || !number(v[3], 130, &b))
            goto usage;
        if (set_load(a, b) < 0)
            return;
        printf("설정한 회로 부하 A/B: %d/%d\n", a, b);
        break;

    case EV_P02:
        if ((n != 3 && n != 4) || !number(v[n - 1], 1, &a))
            goto usage;
        if (n == 4 && strcmp(v[2], "CR-A1")) {
            puts("추가 조작 필요: 지정한 회로가 고장 회로와 다릅니다.");
            return;
        }
        if (set_number(POWER_CFG, "fault_isolated", a) < 0 ||
            kv_set(POWER "circuit_status.txt", "circuit_a", a ? "isolated" : "fault") < 0)
            return;
        puts(a ? "고장 회로를 격리했습니다. 차단기를 조작하세요."
               : "고장 회로 격리를 해제했습니다.");
        return;

    case EV_P03:
        if (n != 4 || !number(v[2], 100, &a) || !number(v[3], 100, &b))
            goto usage;
        if (set_split(a, b) < 0)
            return;
        printf("설정한 발전실/냉각실 배분: %d/%d\n", a, b);
        break;

    default:
        return;
    }

    request_verify(w);
    return;

usage:
    puts("repair temperature_sensor <temp_sensor_a.dat|temp_sensor_b.dat> | repair generator_config");
    puts("사용법: repair coolant_flow <0~100> | cooling_sensor <A|B>");
    puts("        repair circuit_load <A> <B> | fault_isolated [회로 ID] <0|1>");
    puts("        repair power_split <발전실> <냉각실>");
}

static void operate_breaker(worker_t *w, int restart)
{
    double isolated;

    if (w->location != FAC_POWER || w->active != EV_P02) {
        puts("배전실에서 P02가 진행 중일 때만 조작할 수 있습니다.");
        return;
    }
    if (w->pending || w->breaker_wait) {
        puts("차단기 상태를 확인 중입니다.");
        return;
    }
    if (kv_get_double(POWER_CFG, "fault_isolated", &isolated) < 0 ||
        isolated != 1.0) {
        puts("먼저 고장 회로를 격리하세요.");
        return;
    }

    int r = restart ? fac_mgr_equip_restart(&w->mgr, FAC_POWER)
                    : fac_mgr_equip_signal(&w->mgr, FAC_POWER, SIGCONT);

    if (r < 0) {
        perror("차단기 조작");
        return;
    }

    w->breaker_wait = 1;
    puts(restart ? "차단기 재실행을 요청했습니다."
                 : "차단기 재개를 요청했습니다.");
}

static void show_manual(const char *topic)
{
    static const struct { const char *topic, *path; } catalog[] = {
        {"index", "common/error_code_index.txt"}, {"registry", "common/equipment_registry.csv"},
        {"history", "common/maintenance_history.log"}, {"G01", "generator/sensor_link_map.csv"},
        {"G02", "generator/turbine_restart_guide.txt"}, {"G03", "generator/generator_config_manual.txt"},
        {"C01", "cooling/coolant_flow_spec.csv"}, {"C02", "cooling/pump_process_manual.txt"},
        {"C03", "cooling/cooling_sensor_history.log"}, {"P01", "power/power_distribution_map.csv"},
        {"P02", "power/breaker_reset_manual.txt"}, {"P03", "power/load_balance_manual.txt"}
    };
    for (size_t i = 0; i < sizeof catalog / sizeof catalog[0]; i++) {
        if (!strcmp(topic, catalog[i].topic)) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "archive/%s", catalog[i].path);
            show_file(path);
            if (!strcmp(topic, "G03")) show_file("archive/restricted/generator_restart_guide.txt");
            return;
        }
    }
    puts("manual index|registry|history|이벤트코드");
}

static int run_command(worker_t *w, char *line)
{
    char *v[5];
    int n = 0;

    for (char *s = strtok(line, " \t\r"); s; s = strtok(NULL, " \t\r")) {
        if (n == 5) {
            puts("명령어 인자가 너무 많습니다.");
            return 0;
        }
        v[n++] = s;
    }

    if (n == 2 && !strcmp(v[0], "test") &&
        (!strcmp(v[1], "on") || !strcmp(v[1], "off"))) {
        w->solo = !strcmp(v[1], "on");
        puts(w->solo ? "테스트 도구 켜짐" : "테스트 도구 꺼짐");
        return 0;
    }
    if (n == 2 && !strcmp(v[0], "manual")) {
        if (w->solo) show_manual(v[1]);
        else puts("관제사 자료는 직원 콘솔에서 열람할 수 없습니다.");
        return 0;
    }
    if (n == 1 && !strcmp(v[0], "status")) {
        int fac = w->location;
        if (fac < 0 || fac >= FAC_COUNT || !w->mgr.fac[fac].has_last) {
            puts("시설로 이동한 뒤 상태 보고를 기다리세요.");
            return 0;
        }
        const fac_msg_t *m = &w->mgr.fac[fac].last;
        for (int i = 0; i < METRIC_COUNT; ++i)
            printf("%s %.1f %s\n", metric_label(fac,i), m->metrics[i], metric_unit(fac,i));
        if (fac == FAC_POWER)
            printf("GN-2291-K %.1f / %d kW, PUMP-B %.1f / %d kW\n",
                   m->delivered_kw[0], GEN_LIMIT_KW, m->delivered_kw[1], COOL_LIMIT_KW);
        return 0;
    }
    if (n == 3 && w->solo && !strcmp(v[0], "debug") && !strcmp(v[1], "next") &&
        (!strcmp(v[2], "stop") || !strcmp(v[2], "kill")) && w->active == EV_NONE &&
        (events[w->step] == EV_C02 || events[w->step] == EV_G02)) {
        w->variant = !strcmp(v[2], "stop") ? PF_STOP : PF_KILL;
        if (arm_next(w, 0) < 0) perror("이벤트 예약");
        w->variant = 0;
        return 0;
    }
    if (n == 1 && !strcmp(v[0], "quit"))
        return 1;

    if (n == 2 && !strcmp(v[0], "go") && !strcmp(v[1], "cooling")) {
        w->location = FAC_COOLING;
        puts("냉각실로 이동했습니다.");
    } else if (n == 2 && !strcmp(v[0], "go") && !strcmp(v[1], "power")) {
        w->location = FAC_POWER;
        puts("배전실로 이동했습니다.");
    } else if (n == 2 && !strcmp(v[0], "go") && !strcmp(v[1], "generator")) {
        w->location = FAC_GENERATOR;
        puts("발전실로 이동했습니다.");
    } else if (w->location == FAC_GENERATOR && n == 2 && !strcmp(v[0], "stat") &&
               !strcmp(v[1], "temperature_sensor_link")) {
        show_sensor_link(GEN_LINK);
    } else if (w->location == FAC_GENERATOR && n == 2 && !strcmp(v[0], "stat") &&
               !strcmp(v[1], "generator_config.cfg")) {
        struct stat st;
        if (stat(GEN_CFG, &st) < 0)
            perror(GEN_CFG);
        else
            printf("generator_config.cfg mode=%04o\n", (unsigned)(st.st_mode & 07777));
    } else if (n == 2 && !strcmp(v[0], "cat")) {
        cat_file(w->location, v[1]);
    } else if (n == 2 && !strcmp(v[0], "stat") &&
               !strcmp(v[1], "cooling_sensor_link")) {
        if (w->location == FAC_COOLING)
            show_sensor_link(SENSOR_LINK);
        else
            puts("먼저 go cooling을 입력하세요.");
    } else if (n == 2 && !strcmp(v[1], "pump") &&
               (!strcmp(v[0], "repair") || !strcmp(v[0], "resume") ||
                !strcmp(v[0], "restart"))) {
        const fac_handle_t *cool = &w->mgr.fac[FAC_COOLING];
        if (w->active != EV_C02 || w->location != FAC_COOLING)
            puts("냉각실에서 C02 진행 중에 수리하세요.");
        else if (w->pump_wait)
            puts("C02 펌프의 RUNNING 보고를 기다립니다.");
        else if (w->pending)
            puts("C02 수리 검증 결과를 기다립니다.");
        else if (!cool->has_last || cool->last.event != EV_C02)
            puts("C02 펌프 상태 조회 실패: 시설 보고를 기다리세요.");
        else if ((!strcmp(v[0], "resume") && cool->last.equip_state != EQ_STOPPED) ||
                 (!strcmp(v[0], "restart") && cool->last.equip_state != EQ_DEAD))
            printf("현재 펌프 상태 %s에서는 해당 명령을 사용할 수 없습니다.\n",
                   equip_state_key(cool->last.equip_state));
        else
            recover_pump(w, cool->last.equip_state);
    } else if (n == 2 && !strcmp(v[1], "turbine") &&
               (!strcmp(v[0], "resume") || !strcmp(v[0], "restart"))) {
        const fac_handle_t *gen = &w->mgr.fac[FAC_GENERATOR];
        int restart = !strcmp(v[0], "restart");
        if (w->location != FAC_GENERATOR || (w->active != EV_G02 && w->active != EV_G03))
            puts("발전실에서 G02 또는 G03 진행 중에 조작하세요.");
        else if (w->pending || w->turbine_wait)
            puts("시설 응답을 기다리는 중입니다.");
        else if (!gen->has_last || gen->last.event != w->active ||
                 gen->last.equip_state != (restart ? EQ_DEAD : EQ_STOPPED))
            puts("터빈 상태를 확인하세요. STOPPED: resume turbine / DEAD: restart turbine");
        else if ((restart ? fac_mgr_equip_restart(&w->mgr, FAC_GENERATOR)
                          : fac_mgr_equip_signal(&w->mgr, FAC_GENERATOR, SIGCONT)) < 0)
            perror("터빈 조작");
        else {
            w->turbine_wait = 1;
            puts("터빈의 RUNNING 보고를 기다립니다.");
        }
    } else if (n && !strcmp(v[0], "repair")) {
        repair(w, v, n);
    } else if (n == 2 && !strcmp(v[1], "breaker") &&
               (!strcmp(v[0], "resume") || !strcmp(v[0], "restart"))) {
        operate_breaker(w, !strcmp(v[0], "restart"));
    } else if (n == 2 && !strcmp(v[0], "debug") && !strcmp(v[1], "status")) {
        if (w->active != EV_NONE)
            printf("%s 진행 중%s\n", event_code(w->active),
                   (w->pending || w->breaker_wait) ? " / 응답 대기 중" : "");
        else if (events[w->step] != EV_NONE)
            printf("%s 발생 대기 중\n", event_code(events[w->step]));
        else
            puts("테스트 이벤트 모두 수리 완료");
    } else if (w->solo && n == 2 && !strcmp(v[0], "debug") &&
               (!strcmp(v[1], "next") ||
                (events[w->step] == EV_C01 && !strcmp(v[1], "c01")) ||
                (events[w->step] == EV_C03 && !strcmp(v[1], "c03")))) {
        if (w->active != EV_NONE || events[w->step] == EV_NONE)
            puts("지금 발생시킬 대기 이벤트가 없습니다.");
        else if (arm_next(w, 0) < 0)
            perror("이벤트 예약");
    } else {
        puts("go cooling|power|generator / status / cat <파일> / quit");
        if (w->location == FAC_GENERATOR)
            puts("generator_label.txt generator_config.cfg generator_status.txt turbine_status.txt temp_sensor_a.dat temp_sensor_b.dat temperature_sensor_link\nstat temperature_sensor_link|generator_config.cfg / repair temperature_sensor FILE / repair generator_config / resume|restart turbine");
        if (w->location == FAC_COOLING)
            puts("pump_label.txt pump_status.txt coolant_config.cfg coolant_level.txt cooling_sensor_a.dat cooling_sensor_b.dat\nstat cooling_sensor_link / repair cooling_sensor A|B / repair coolant_flow N / repair|resume|restart pump");
        if (w->location == FAC_POWER)
            puts("circuit_label.txt circuit_status.txt load_status.txt breaker_status.txt power_config.cfg\nrepair circuit_load A B / repair fault_isolated 0|1 / resume|restart breaker / repair power_split G C");
        puts("개발 테스트 도구: test on / test off");
        if (w->solo) puts("테스트 전용: manual index|registry|history|이벤트코드 / debug next [stop|kill]");
    }
    return 0;
}

/* Probe the same mount as the G03 file without changing the seed config. */
static int supports_modes(void)
{
    char probe[] = GEN ".mode-XXXXXX";
    int fd = mkstemp(probe);
    if (fd < 0) return -1;
    const mode_t modes[] = {0000, 0640};
    int supported = 1;
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        struct stat st;
        if (fchmod(fd, modes[i]) < 0 || fstat(fd, &st) < 0) {
            supported = -1;
            break;
        }
        if ((st.st_mode & 07777) != modes[i]) {
            supported = 0;
            break;
        }
    }
    close(fd);
    if (unlink(probe) < 0) return -1;
    return supported;
}

static int remove_session_file(const char *path, const struct stat *st, int type,
                               struct FTW *info)
{
    (void)st; (void)type; (void)info;
    return remove(path);
}

static int prepare_session(char *session)
{
    int supported = supports_modes();
    if (supported != 0) return supported < 0 ? -1 : 0;
    char archive[PATH_MAX], link[PATH_MAX];
    if (!realpath("archive", archive)) return -1;
    strcpy(session, "/tmp/plant-worker-XXXXXX");
    if (!mkdtemp(session)) {
        session[0] = '\0';
        return -1;
    }
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execlp("cp", "cp", "-RP", "--no-preserve=mode,ownership", "--", ROOT, session,
               (char *)NULL);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EIO;
        return -1;
    }
    snprintf(link, sizeof link, "%s/archive", session);
    if (symlink(archive, link) < 0 || chdir(session) < 0) return -1;
    if (supports_modes() != 1) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    worker_t w = {.active = EV_NONE, .location = -1};
    char session[PATH_MAX] = "";
    char line[128];
    size_t used = 0;
    int overflow = 0, escape = 0, prompt = 1, result = EXIT_SUCCESS;

    w.solo = argc == 2 && !strcmp(argv[1], "--solo-test");
    if (argc != 1 && !w.solo) {
        fputs("usage: worker [--solo-test]\n", stderr);
        return EXIT_FAILURE;
    }
    puts(w.solo ? "단독 테스트: help / manual index" : "직원 콘솔: help");
    if (install_signals() < 0 || prepare_session(session) < 0 || fac_mgr_init(&w.mgr, ROOT) < 0) {
        perror("호스트 초기화");
        result = EXIT_FAILURE;
        goto cleanup_session;
    }

    if (access(COOL SENSOR_A, R_OK) < 0 ||
        access(COOL SENSOR_B, R_OK) < 0 ||
        relink_sensor(SENSOR_LINK, SENSOR_B) < 0 ||
        relink_sensor(GEN_LINK, TEMP_B) < 0 ||
        set_generator_mode(0640) < 0 ||
        access(POWER_CFG, R_OK | W_OK) < 0 ||
        fac_mgr_spawn_all(&w.mgr) < 0 ||
        arm_next(&w, BACKTIME_MS) < 0) {
        perror("시설 또는 현장 파일 초기화");
        result = EXIT_FAILURE;
        goto done;
    }

    if (begin_input() < 0) {
        perror("터미널 입력 설정");
        result = EXIT_FAILURE;
        goto done;
    }

    while (!quitting) {
        struct pollfd fds[FAC_COUNT + 1] = {{STDIN_FILENO, POLLIN, 0}};

        if (prompt) {
            draw_input(line, used, w.location);
            prompt = 0;
        }
        for (int i = 0; i < FAC_COUNT; i++) {
            fds[i + 1].fd = fac_mgr_fd(&w.mgr, i);
            fds[i + 1].events = POLLIN;
        }

        int ready = poll(fds, FAC_COUNT + 1, 100);
        if (ready < 0 && errno != EINTR) {
            perror("poll");
            result = EXIT_FAILURE;
            break;
        }
        if (quitting)
            break;

        if (w.active != EV_NONE && now_ms() >= w.deadline) {
            clear_input_line();
            puts("\n수리 제한 시간 초과: 게임 오버");
            break;
        }

        for (int i = 0; i < FAC_COUNT; i++) {
            fac_msg_t msg;
            fac_read_t r;

            if (fds[i + 1].fd < 0)
                continue;

            while ((r = fac_mgr_read(&w.mgr, i, &msg)) == FAC_GOT) {
                int redraw = handle_report(&w, i, &msg);

                if (redraw < 0) {
                    result = EXIT_FAILURE;
                    goto done;
                }
                if (redraw)
                    prompt = 1;
            }

            if (r != FAC_NONE) {
                fprintf(stderr, "\n%s 시설 보고 읽기 실패\n",
                        facility_name(i));
                result = EXIT_FAILURE;
                goto done;
            }
        }

        if (ready <= 0)
            continue;
        if (fds[0].revents & (POLLERR | POLLNVAL)) {
            fputs("입력을 읽을 수 없습니다.\n", stderr);
            result = EXIT_FAILURE;
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char input[256];
            ssize_t n = read(STDIN_FILENO, input, sizeof input);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                perror("입력");
                result = EXIT_FAILURE;
                break;
            }
            if (n == 0)
                break;

            for (ssize_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)input[i];

                /* 방향키의 이스케이프 문자는 명령에 넣지 않는다. */
                if (editing_tty && escape == 1) {
                    escape = (c == '[' || c == 'O') ? 2 : 0;
                    if (escape)
                        continue;
                } else if (editing_tty && escape == 2) {
                    if (c == '\n' || c == '\r') {
                        escape = 0;
                    } else {
                        if (c >= 0x40 && c <= 0x7e)
                            escape = 0;
                        continue;
                    }
                }
                if (editing_tty && c == 27) {
                    escape = 1;
                    continue;
                }

                if (c == '\n' || (editing_tty && c == '\r')) {
                    if (editing_tty) {
                        draw_input(line, used, w.location);
                        putchar('\n');
                    }

                    if (w.active != EV_NONE && now_ms() >= w.deadline) {
                        puts("수리 제한 시간 초과: 게임 오버");
                        goto done;
                    }

                    if (overflow) {
                        puts("명령어가 너무 깁니다.");
                    } else {
                        line[used] = '\0';
                        if (run_command(&w, line))
                            goto done;
                    }

                    used = 0;
                    overflow = 0;
                    prompt = 1;
                } else if (editing_tty && (c == 8 || c == 127)) {
                    if (used > 0) {
                        do {
                            used--;
                        } while (used > 0 &&
                                 ((unsigned char)line[used] & 0xc0) == 0x80);
                    }
                    prompt = 1;
                } else if (editing_tty && c == 21) {
                    used = 0;  /* Ctrl+U: 입력한 글자만 모두 지운다. */
                    prompt = 1;
                } else if (!editing_tty || c >= 32 || c == '\t') {
                    if (used < sizeof line - 1)
                        line[used++] = editing_tty && c == '\t' ? ' ' : (char)c;
                    else if (!editing_tty)
                        overflow = 1;

                    if (editing_tty)
                        prompt = 1;
                }
            }
            fflush(stdout);
        }
    }

done:
    if (editing_tty) {
        clear_input_line();
        fflush(stdout);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty) < 0)
            perror("터미널 설정 복원");
    }
    fac_mgr_shutdown(&w.mgr);
cleanup_session:
    if (session[0] && nftw(session, remove_session_file, 16, FTW_DEPTH | FTW_PHYS) < 0) {
        perror("임시 현장 정리");
        result = EXIT_FAILURE;
    }
    return result;
}
