/* facility.c - 시설 프로세스 본체 (백 타임, 위험 이벤트 알림, 수리 확인) */
#define _GNU_SOURCE
#include "facility.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "equipment.h"
#include "fac_ipc.h"
#include "kvfile.h"
#include "repair_check.h"
#include "timeutil.h"

#define STALE_COOLANT_TEMP 18.4f   /* C03: 옛 센서 파일에 남아 있는 값 */

typedef struct {
    int     fac;
    int     msg_fd;
    int     ctrl_fd;
    char    root[PATH_MAX];
    pid_t   host;

    int     arm_state;       /* fac_arm_state_t */
    int     event;           /* 예약됐거나 진행 중인 이벤트 */
    int     variant;         /* FCTL_ARM 의 arg3 */
    int64_t total_ms;
    int64_t left_ms;
    int     fault;           /* 위험 이벤트 진행 중 */
    int     pending_event;   /* FMSG_EVENT 를 아직 못 보냈다 */
    int     link;
    int     repairs;

    equip_t equip;

    float    metrics[METRIC_COUNT];
    int      metrics_init;
    unsigned rng;
} facility_t;

/* 핸들러는 표시만 한다. 실제 일은 메인 루프에서 */
static volatile sig_atomic_t g_term, g_verify, g_chld;

static void on_signal(int sig)
{
    if (sig == SIGTERM)
        g_term = 1;
    else if (sig == FAC_SIG_VERIFY)
        g_verify = 1;
    else if (sig == SIGCHLD)
        g_chld = 1;
}

/* ---------- 계측값 ---------- */
static float noise(facility_t *f, float amp)
{
    f->rng = f->rng * 1103515245u + 12345u;
    return ((float)((f->rng >> 8) & 0xFFFF) / 32768.0f - 1.0f) * amp;
}

/* 지금 상태에서 계측값이 향하는 값과 흔들림 크기 */
static void compute_targets(const facility_t *f, float tgt[METRIC_COUNT], float amp[METRIC_COUNT])
{
    int   down = f->equip.state != EQ_RUNNING;
    int   ev = f->fault ? f->event : EV_NONE;
    float L = (float)f->link;

    switch (f->fac) {
    case FAC_GENERATOR:
        tgt[0] = 500.f + 18.f * L;  amp[0] = 3.f;      /* 발전기 온도 */
        tgt[1] = 480.f - 25.f * L;  amp[1] = 4.f;      /* 출력 */
        tgt[2] = 3600.f - 40.f * L; amp[2] = 15.f;     /* 터빈 회전수 */
        tgt[3] = 160.f + 3.f * L;   amp[3] = 1.f;      /* 증기 압력 */
        if (down) {                                    /* 터빈이 서면 출력이 없고 증기가 갇힌다 */
            tgt[0] += 45.f;
            tgt[1] = 0.f;  amp[1] = 0.f;
            tgt[2] = 0.f;  amp[2] = 0.f;
            tgt[3] = 192.f;
        }
        break;
    case FAC_COOLING:
        tgt[0] = 32.f + 0.8f * L;   amp[0] = 0.4f;     /* 냉각수 온도 */
        tgt[1] = 85.f;              amp[1] = 0.3f;     /* 수위 */
        tgt[2] = 120.f - 2.f * L;   amp[2] = 1.5f;     /* 유량 */
        tgt[3] = 94.f - 3.f * L;    amp[3] = 0.5f;     /* 냉각 효율 */
        if (ev == EV_C01) {
            tgt[0] = 52.f; tgt[1] = 58.f; tgt[2] = 45.f; tgt[3] = 52.f;
            amp[2] = 3.f;
        }
        if (down) {                                    /* 펌프가 서면 물이 돌지 않는다 */
            tgt[0] = 66.f;
            tgt[2] = 0.f;  amp[2] = 0.f;
            tgt[3] = 18.f;
        }
        break;
    default: /* FAC_POWER */
        tgt[0] = 380.f - 3.f * L;   amp[0] = 1.f;      /* 모선 전압 */
        tgt[1] = 5200.f - 60.f * L; amp[1] = 20.f;     /* 공급 전력 */
        tgt[2] = 60.f;              amp[2] = 0.02f;    /* 주파수 */
        tgt[3] = 97.f;              amp[3] = 0.5f;     /* 부하 균형 */
        if (ev == EV_P01) {
            tgt[0] = 348.f; tgt[1] = 4300.f; tgt[3] = 52.f;
            amp[0] = 4.f;
        } else if (ev == EV_P03) {
            tgt[0] = 366.f; tgt[2] = 58.7f; tgt[3] = 41.f;
            amp[2] = 0.08f;
        }
        if (down) {                                    /* 차단기가 떨어지면 공급이 끊긴다 */
            tgt[0] = 205.f; amp[0] = 6.f;
            tgt[1] = 900.f;
            tgt[3] = 30.f;
        }
        break;
    }
}

/* 저장한 값을 목표 쪽으로 반씩 옮긴다 (값이 서서히 변해 보이게) */
static void update_metrics(facility_t *f)
{
    float tgt[METRIC_COUNT], amp[METRIC_COUNT];
    int i;

    compute_targets(f, tgt, amp);
    for (i = 0; i < METRIC_COUNT; i++) {
        if (!f->metrics_init)
            f->metrics[i] = tgt[i];
        else
            f->metrics[i] += (tgt[i] - f->metrics[i]) * 0.5f;
    }
    f->metrics_init = 1;
}

/* ---------- 보고 ---------- */
static void fill_msg(facility_t *f, fac_msg_t *m, int type)
{
    float tgt[METRIC_COUNT], amp[METRIC_COUNT];
    int i;

    memset(m, 0, sizeof *m);
    m->type = type;
    m->facility = f->fac;
    m->pid = (int32_t)getpid();
    m->arm_state = f->arm_state;
    m->event = f->event;
    if (f->arm_state == ARM_ARMED || f->arm_state == ARM_PAUSED) {
        m->backtime_left_ms = (int32_t)f->left_ms;
        m->backtime_total_ms = (int32_t)f->total_ms;
    }
    m->fault = f->fault;
    m->link_level = f->link;
    m->equip_pid = (int32_t)f->equip.pid;
    m->equip_state = f->equip.state;
    m->repairs = f->repairs;

    compute_targets(f, tgt, amp);
    for (i = 0; i < METRIC_COUNT; i++) {
        float v = f->metrics[i] + noise(f, amp[i]);
        m->metrics[i] = v < 0.f ? 0.f : v;
    }
    /* 센서 링크가 망가지면 관제 쪽에는 그 값이 제대로 가지 않는다 */
    if (f->fault && f->event == EV_G01)
        m->metrics[0] = NAN;                      /* 수신 불가 */
    else if (f->fault && f->event == EV_C03)
        m->metrics[0] = STALE_COOLANT_TEMP;       /* 옛 파일의 멈춘 값 */
}

static void notify_host(const facility_t *f)
{
    /* 호스트가 죽어 다른 프로세스에 입양됐다면 엉뚱한 곳에 보내지 않는다 */
    if (getppid() == f->host)
        kill(f->host, FAC_SIG_NOTIFY);
}

/* 주기 보고. 파이프가 가득 찼으면 (호스트가 안 읽으면) 버린다 */
static void send_status(facility_t *f)
{
    fac_msg_t m;

    update_metrics(f);
    fill_msg(f, &m, FMSG_STATUS);
    (void)fac_write_msg(f->msg_fd, &m);
}

/* 한 번뿐인 보고는 파이프가 가득 차 있어도 잠깐 기다리며 다시 써 본다 */
static int send_important(facility_t *f, fac_msg_t *m)
{
    int tries;

    for (tries = 0; tries < 20; tries++) {
        if (fac_write_msg(f->msg_fd, m) == 0)
            return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        sleep_ms(5);
    }
    return -1;
}

/* ---------- 설비 프로세스 ---------- */
static void write_equip_status(const facility_t *f)
{
    char path[PATH_MAX], pid[16];

    snprintf(path, sizeof path, "%s/%s/%s", f->root, facility_key(f->fac), equip_status_file(f->fac));
    snprintf(pid, sizeof pid, "%d", (int)f->equip.pid);
    (void)kv_set(path, "pid", pid);
    (void)kv_set(path, "state", equip_state_key(f->equip.state));
}

static void send_equip_notice(facility_t *f, const char *text)
{
    fac_msg_t m;

    fill_msg(f, &m, FMSG_EQUIP);
    snprintf(m.detail, sizeof m.detail, "%s", text);
    (void)send_important(f, &m);
}

/* 설비 상태가 바뀐 뒤: 상태 파일을 고치고 호스트에 알린다 */
static void equip_changed(facility_t *f)
{
    char text[FAC_DETAIL_MAX];
    const char *what = f->equip.state == EQ_RUNNING ? "다시 가동"
                     : f->equip.state == EQ_STOPPED ? "정지" : "종료";

    write_equip_status(f);
    snprintf(text, sizeof text, "%s 프로세스 %s (pid %d)", equip_name(f->fac), what, (int)f->equip.pid);
    send_equip_notice(f, text);
}

static void start_equip(facility_t *f)
{
    char text[FAC_DETAIL_MAX];

    if (!repair_equip_startable(f->root, f->fac, text, sizeof text)) {
        (void)equip_reap(&f->equip);
        equip_destroy(&f->equip);
        write_equip_status(f);
        send_equip_notice(f, text);
        return;
    }
    if (equip_spawn(&f->equip, f->fac) != 0) {
        snprintf(text, sizeof text, "%s 프로세스 시작 실패", equip_name(f->fac));
        send_equip_notice(f, text);
        return;
    }
    write_equip_status(f);
    snprintf(text, sizeof text, "%s 프로세스 시작 (pid %d)", equip_name(f->fac), (int)f->equip.pid);
    send_equip_notice(f, text);
}

/* 방금 보낸 SIGSTOP/SIGKILL 이 반영될 때까지 잠깐 기다린다 (최대 200ms) */
static int reap_soon(facility_t *f)
{
    int i;

    for (i = 0; i < 100; i++) {
        if (equip_reap(&f->equip))
            return 1;
        sleep_ms(2);
    }
    return 0;
}

/* 이벤트에 딸린 프로세스 고장을 자기 설비에 일으킨다 */
static void apply_proc_fault(facility_t *f)
{
    proc_fault_t pf = event_proc_fault(f->event);

    if (pf == PF_NONE)
        return;
    if (pf == PF_EITHER) {
        if (f->variant == PF_STOP || f->variant == PF_KILL)
            pf = (proc_fault_t)f->variant;
        else
            pf = (noise(f, 1.f) < 0.f) ? PF_STOP : PF_KILL;
    }
    if (equip_reap(&f->equip))
        equip_changed(f);
    if (f->equip.state == EQ_DEAD)
        return;
    if (pf == PF_STOP && f->equip.state == EQ_STOPPED)
        return;
    if (equip_signal(&f->equip, pf == PF_STOP ? SIGSTOP : SIGKILL) == 0 && reap_soon(f))
        equip_changed(f);
}

/* ---------- 위험 이벤트 ---------- */
static void fire(facility_t *f)
{
    f->left_ms = 0;
    f->arm_state = ARM_FIRED;
    f->fault = 1;
    apply_proc_fault(f);
    f->pending_event = 1;
}

/* FMSG_EVENT 를 쓰고, 써졌을 때만 시그널을 보낸다. 실패하면 다음 틱에 다시 */
static void try_send_event(facility_t *f)
{
    fac_msg_t m;

    update_metrics(f);
    fill_msg(f, &m, FMSG_EVENT);
    if (fac_write_msg(f->msg_fd, &m) == 0) {
        f->pending_event = 0;
        notify_host(f);
    }
}

/* SIGUSR2: 실제 파일과 설비 상태로 수리를 확인하고 결과를 알린다 */
static void do_verify(facility_t *f)
{
    repair_result_t res;
    fac_msg_t m;
    int event = f->event;

    if (f->fault) {
        repair_verify(f->root, event, f->equip.state, &res);
    } else {
        repair_verify(f->root, EV_NONE, f->equip.state, &res);
        event = EV_NONE;
    }
    if (res.ok) {                 /* 정상 상태로 복귀. 다음 백 타임은 호스트가 FCTL_ARM 으로 준다 */
        f->fault = 0;
        f->arm_state = ARM_IDLE;
        f->event = EV_NONE;
        f->repairs++;
    }
    update_metrics(f);
    fill_msg(f, &m, FMSG_VERIFY);
    m.event = event;
    m.verify_ok = res.ok;
    m.verify_failed = res.failed;
    snprintf(m.detail, sizeof m.detail, "%s", res.detail);
    if (send_important(f, &m) == 0)
        notify_host(f);
}

/* ---------- 제어 ---------- */
/* 제어 메시지 하나. 종료 요청이면 1 */
static int handle_ctrl(facility_t *f, const fac_ctrl_t *c)
{
    switch (c->cmd) {
    case FCTL_ARM:
        if (event_facility(c->arg1) != f->fac)
            break;
        f->event = c->arg1;
        f->total_ms = c->arg2 > 0 ? c->arg2 : 0;
        f->left_ms = f->total_ms;
        f->variant = c->arg3;
        f->arm_state = ARM_ARMED;
        f->fault = 0;
        f->pending_event = 0;
        break;
    case FCTL_PAUSE:
        if (f->arm_state == ARM_ARMED)
            f->arm_state = ARM_PAUSED;
        break;
    case FCTL_RESUME:
        if (f->arm_state == ARM_PAUSED)
            f->arm_state = ARM_ARMED;
        break;
    case FCTL_DISARM:
        f->arm_state = ARM_IDLE;
        f->event = EV_NONE;
        f->total_ms = f->left_ms = 0;
        f->fault = 0;
        f->pending_event = 0;
        break;
    case FCTL_LINK:
        f->link = c->arg1 < 0 ? 0 : (c->arg1 > FAC_LINK_MAX ? FAC_LINK_MAX : c->arg1);
        break;
    case FCTL_EQUIP_RESTART:
        start_equip(f);
        break;
    case FCTL_SHUTDOWN:
        return 1;
    default:
        break;
    }
    return 0;
}

/* 쌓인 제어 메시지를 모두 처리. 끝내야 하면 1 (종료 요청 또는 호스트가 파이프를 닫음) */
static int drain_ctrl(facility_t *f, fac_reader_t *rd, int *handled)
{
    for (;;) {
        fac_ctrl_t c;
        fac_read_t r = fac_read_ctrl(rd, &c);

        if (r == FAC_GOT) {
            *handled = 1;
            if (handle_ctrl(f, &c))
                return 1;
            continue;
        }
        return r == FAC_EOF;
    }
}

/* 사용 중인 센서 파일의 value= 줄에 실제 값을 쓴다 (수정 시각도 같이 움직인다) */
static void write_sensor(const facility_t *f)
{
    const char *rel = repair_live_sensor(f->fac);
    char path[PATH_MAX], val[32];

    if (rel == NULL || !f->metrics_init)
        return;
    snprintf(path, sizeof path, "%s/%s", f->root, rel);
    snprintf(val, sizeof val, "%.1f", (double)f->metrics[0]);
    (void)kv_set(path, "value", val);
}

/* ---------- 시작 준비 ---------- */
static void detach_from_terminal(int keep1, int keep2)
{
    int nullfd = open("/dev/null", O_RDWR);
    long max = sysconf(_SC_OPEN_MAX);
    int fd;

    if (nullfd >= 0) {
        dup2(nullfd, STDIN_FILENO);
        dup2(nullfd, STDOUT_FILENO);
        dup2(nullfd, STDERR_FILENO);
    }
    if (max < 0 || max > 4096)
        max = 4096;
    /* 호스트에게서 물려받은 소켓과 다른 시설의 파이프를 닫는다 */
    for (fd = 3; fd < (int)max; fd++)
        if (fd != keep1 && fd != keep2)
            close(fd);
}

static void setup_signals(sigset_t *wait_mask)
{
    struct sigaction sa;
    sigset_t block;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(FAC_SIG_VERIFY, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(FAC_SIG_NOTIFY, SIG_IGN);
    signal(SIGWINCH, SIG_DFL);      /* 호스트(ncurses)의 핸들러를 물려받지 않는다 */
    signal(SIGALRM, SIG_DFL);

    /*
     * 세 시그널은 평소에 막아 두고 ppoll() 안에서만 받는다.
     * 표시를 확인한 직후 ppoll 에 들어가기 전에 시그널이 와서 한 틱 늦는 일이 없다.
     */
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, FAC_SIG_VERIFY);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_SETMASK, &block, NULL);
    sigemptyset(wait_mask);
}

int facility_run(const facility_args_t *args)
{
    facility_t f;
    fac_reader_t rd;
    sigset_t wait_mask;
    char name[16];
    int64_t last, next_status, next_sensor;
    int quit = 0;

    if (args == NULL || args->fac < 0 || args->fac >= FAC_COUNT || args->root == NULL)
        return 2;

    memset(&f, 0, sizeof f);
    f.fac = args->fac;
    f.msg_fd = args->msg_fd;
    f.ctrl_fd = args->ctrl_fd;
    snprintf(f.root, sizeof f.root, "%s", args->root);
    f.host = args->host_pid > 0 ? args->host_pid : getppid();
    f.event = EV_NONE;
    f.rng = (unsigned)getpid() * 2654435761u + (unsigned)f.fac;
    equip_init(&f.equip);
    g_term = g_verify = g_chld = 0;

    /* 호스트가 죽으면 같이 죽는다. 설정하기 전에 이미 죽었는지도 본다 */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != f.host)
        return 0;

    setpgid(0, 0);                   /* 터미널의 Ctrl-C, Ctrl-Z 가 오지 않게 */
    snprintf(name, sizeof name, "fac-%s", facility_key(f.fac));
    prctl(PR_SET_NAME, name, 0, 0, 0);
    detach_from_terminal(f.msg_fd, f.ctrl_fd);
    setup_signals(&wait_mask);

    /* 설비 프로세스가 물려받지 않게 CLOEXEC, 호스트가 안 읽어도 멈추지 않게 O_NONBLOCK */
    fcntl(f.msg_fd, F_SETFD, FD_CLOEXEC);
    fcntl(f.ctrl_fd, F_SETFD, FD_CLOEXEC);
    fcntl(f.msg_fd, F_SETFL, fcntl(f.msg_fd, F_GETFL) | O_NONBLOCK);
    fcntl(f.ctrl_fd, F_SETFL, fcntl(f.ctrl_fd, F_GETFL) | O_NONBLOCK);
    fac_reader_init(&rd, f.ctrl_fd);

    start_equip(&f);

    last = now_ms();
    next_status = last;
    next_sensor = last;

    while (!g_term && !quit) {
        struct pollfd pfd;
        struct timespec tick;
        int64_t now, delta;
        int send_now = 0;

        pfd.fd = f.ctrl_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        tick.tv_sec = 0;
        tick.tv_nsec = FAC_TICK_MS * 1000000L;
        if (ppoll(&pfd, 1, &tick, &wait_mask) < 0 && errno != EINTR)
            break;

        if (drain_ctrl(&f, &rd, &send_now))
            break;

        g_chld = 0;
        if (equip_reap(&f.equip)) {
            equip_changed(&f);
            send_now = 1;
        }

        now = now_ms();
        delta = now - last;
        last = now;
        if (delta < 0)
            delta = 0;
        if (delta > 1000)            /* 기계가 잠들었다 깨어나도 백 타임이 한꺼번에 사라지지 않게 */
            delta = 1000;

        if (f.arm_state == ARM_ARMED) {
            f.left_ms -= delta;
            if (f.left_ms <= 0)
                fire(&f);
        }
        if (f.pending_event) {
            try_send_event(&f);
            send_now = 1;
        }

        if (g_verify) {
            g_verify = 0;
            /* 호스트가 시그널보다 먼저 써 둔 제어 메시지(재실행 등)와 설비 상태부터 반영한다 */
            if (drain_ctrl(&f, &rd, &send_now))
                quit = 1;
            if (equip_reap(&f.equip))
                equip_changed(&f);
            if (!quit)
                do_verify(&f);
            send_now = 1;
        }

        if (send_now || now >= next_status) {
            send_status(&f);
            next_status = now + FAC_STATUS_MS;
        }
        if (now >= next_sensor) {
            write_sensor(&f);
            next_sensor = now + FAC_SENSOR_MS;
        }
    }

    equip_destroy(&f.equip);
    write_equip_status(&f);
    close(f.msg_fd);
    close(f.ctrl_fd);
    return 0;
}
