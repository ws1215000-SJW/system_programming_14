/*
 * demo.c - 화면과 시설 프로세스를 붙여서 돌려 보는 시연 프로그램.
 *
 *   ./demo field   [--fast] [--limit <초>]     발전소 직원 화면
 *   ./demo control [--fast] [--limit <초>]     관제사 화면
 *     --fast   백 타임을 몇 초로 줄인다
 *     --limit  수리 제한 시간 (기본 300초, --fast 면 60초)
 *
 * 호스트·클라이언트 프로그램이 나오기 전까지 두 모듈을 눈으로 확인하려고 만든 것이다.
 * 여기 있는 게임 진행(이벤트 순서, 복구율, 제한 시간)과 명령은 흉내만 낸 것이고,
 * 실제 구현은 호스트·클라이언트 쪽에 있다. 현장 파일은 tools/fixture.c 가 demo_plant/ 에 만든다.
 * select() 로 키보드와 보고 파이프 세 개를 함께 기다리는 구조는 호스트에서 그대로 쓸 수 있다.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "common.h"
#include "fac_mgr.h"
#include "fixture.h"
#include "timeutil.h"
#include "ui.h"

#define ROOT "demo_plant"

static volatile sig_atomic_t g_quit, g_notify;

static void on_signal(int sig)
{
    if (sig == FAC_SIG_NOTIFY)
        g_notify = 1;       /* 시설이 이벤트나 확인 결과를 파이프에 썼다 */
    else
        g_quit = 1;
}

/* 시설마다 차례로 일어날 이벤트 */
static const int SCRIPT[FAC_COUNT][3] = {
    { EV_G03, EV_NONE, EV_NONE },
    { EV_C02, EV_C03, EV_NONE },
    { EV_P01, EV_P02, EV_NONE },
};

static struct {
    role_t    role;
    fac_mgr_t mgr;
    int       backtime_ms[FAC_COUNT];
    int       limit_ms;

    int       state;                /* game_state_t */
    int       progress[FAC_COUNT];
    int       step[FAC_COUNT];      /* SCRIPT 에서 다음에 쓸 칸 */
    int       waiting[FAC_COUNT];   /* 터졌지만 차례를 기다리는 이벤트 */
    int       active_fac;
    int       active_event;
    int64_t   limit_end;
    int64_t   success_until;
    int       location;
    fac_msg_t last[FAC_COUNT];
    int       has_last[FAC_COUNT];
} G;

static void arm_next(int fac)
{
    int ev = SCRIPT[fac][G.step[fac]];

    if (ev == EV_NONE || G.progress[fac] >= 100) {
        fac_mgr_disarm(&G.mgr, fac);
        return;
    }
    fac_mgr_arm(&G.mgr, fac, ev, G.backtime_ms[fac], 0);
    G.step[fac]++;
}

static void start_event(int fac, int ev)
{
    G.active_fac = fac;
    G.active_event = ev;
    G.state = GS_EVENT_ACTIVE;
    G.limit_end = now_ms() + G.limit_ms;
    fac_mgr_pause_others(&G.mgr, fac);              /* 다른 시설의 백 타임을 멈춘다 */
    fixture_break(ROOT, ev);                        /* 파일 쪽 고장 */
    if (fac == FAC_COOLING)                         /* 연계 고장: 냉각이 죽으면 발전기가 뜨거워진다 */
        fac_mgr_link(&G.mgr, FAC_GENERATOR, 6);
    if (fac == FAC_POWER)
        fac_mgr_link(&G.mgr, FAC_COOLING, 6);
    ui_print(UI_ERR, "%s %s 발생", facility_name(fac), event_code(ev));
    if (G.role == ROLE_FIELD)
        ui_chat(ROLE_CONTROL, "지금 있는 곳 상태부터 알려 줘.");
    else
        ui_chat(ROLE_FIELD, "현장 확인 중. 절차 찾아 줘.");
}

static void finish_event(void)
{
    int fac = G.active_fac, i;

    G.progress[fac] += facility_progress_step(fac);
    if (G.progress[fac] > 100)
        G.progress[fac] = 100;
    fac_mgr_link(&G.mgr, FAC_GENERATOR, 0);
    fac_mgr_link(&G.mgr, FAC_COOLING, 0);
    G.active_fac = -1;
    G.active_event = EV_NONE;
    ui_print(UI_OK, "%s 복구 확인. 복구율 %d%%", facility_name(fac), G.progress[fac]);

    if (G.progress[FAC_GENERATOR] >= 100) {
        G.state = GS_GAME_CLEAR;
        return;
    }
    G.state = GS_REPAIR_SUCCESS;
    G.success_until = now_ms() + 2000;
    fac_mgr_resume_others(&G.mgr, fac);
    arm_next(fac);
    /* 냉각실과 배전실이 모두 끝나면 발전실의 마지막 이벤트 */
    if (G.progress[FAC_COOLING] >= 100 && G.progress[FAC_POWER] >= 100 && G.step[FAC_GENERATOR] == 0)
        arm_next(FAC_GENERATOR);
    for (i = FAC_COUNT - 1; i >= 0; i--) {          /* 기다리던 이벤트가 있으면 이어서 */
        if (G.waiting[i] != EV_NONE) {
            int ev = G.waiting[i];

            G.waiting[i] = EV_NONE;
            start_event(i, ev);
            break;
        }
    }
}

static void on_fac_msg(int fac, const fac_msg_t *m)
{
    G.last[fac] = *m;
    G.has_last[fac] = 1;

    switch (m->type) {
    case FMSG_EVENT:
        if (G.state == GS_EVENT_ACTIVE)
            G.waiting[fac] = m->event;              /* 동시에 터지면 하나씩 */
        else if (G.state == GS_NORMAL || G.state == GS_REPAIR_SUCCESS)
            start_event(fac, m->event);
        break;
    case FMSG_VERIFY:
        if (m->verify_ok && fac == G.active_fac)
            finish_event();
        else if (!m->verify_ok)
            ui_print(UI_ERR, "%s", m->detail);
        break;
    case FMSG_EQUIP:
        if (G.role == ROLE_FIELD && fac == G.location)   /* 눈앞의 설비만 보인다 */
            ui_print(UI_SYS, "%s", m->detail);
        break;
    default:
        break;
    }
}

static void read_facilities(void)
{
    int fac;

    for (fac = 0; fac < FAC_COUNT; fac++) {
        fac_msg_t m;

        while (fac_mgr_read(&G.mgr, fac, &m) == FAC_GOT)
            on_fac_msg(fac, &m);
    }
}

static void cmd_help(void)
{
    /* 관제사는 원래 수리를 못 하지만, 시연에서는 혼자서도 끝까지 가 볼 수 있게 열어 둔다 */
    if (G.role == ROLE_FIELD)
        ui_print(UI_OUT, "go <generator|cooling|power>");
    ui_print(UI_OUT, "ps   resume <turbine|pump|breaker>   restart <turbine|pump|breaker>\n"
                     "fix   verify   say <내용>   quit");
}

static void cmd_ps(void)
{
    int fac;

    ui_print(UI_OUT, "%-10s %7s  %s", "EQUIP", "PID", "STATE");
    for (fac = 0; fac < FAC_COUNT; fac++)
        if (G.has_last[fac])
            ui_print(UI_OUT, "%-10s %7d  %s", equip_key(fac), G.last[fac].equip_pid,
                     equip_state_key(G.last[fac].equip_state));
}

/* 한 줄 명령. 끝내야 하면 1 */
static int run_command(char *line)
{
    char *cmd = strtok(line, " "), *arg = strtok(NULL, "");
    int fac;

    if (cmd == NULL)
        return 0;
    while (arg != NULL && *arg == ' ')
        arg++;

    if (strcmp(cmd, "quit") == 0)
        return 1;
    if (G.state == GS_READY) {                      /* 준비 화면: start 로 시작 */
        if (strcmp(cmd, "start") == 0) {
            G.state = GS_NORMAL;
            arm_next(FAC_COOLING);
            arm_next(FAC_POWER);
            cmd_help();
        }
        return 0;
    }
    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "say") == 0) {
        if (arg != NULL && *arg != '\0')
            ui_chat(G.role, arg);
    } else if (strcmp(cmd, "verify") == 0) {
        if (G.active_fac >= 0)
            fac_mgr_request_verify(&G.mgr, G.active_fac);
        else
            ui_print(UI_ERR, "진행 중인 위험 이벤트가 없다");
    } else if (strcmp(cmd, "fix") == 0) {           /* 시연용: 파일 고장을 정답대로 고친다 */
        if (G.active_fac >= 0) {
            fixture_fix(ROOT, G.active_event);
            fac_mgr_request_verify(&G.mgr, G.active_fac);
        }
    } else if (G.role == ROLE_FIELD && strcmp(cmd, "go") == 0) {
        fac = facility_from_key(arg);
        if (fac < 0)
            ui_print(UI_ERR, "go <generator|cooling|power>");
        else
            G.location = fac;
    } else if (strcmp(cmd, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(cmd, "resume") == 0 || strcmp(cmd, "restart") == 0) {
        fac = equip_from_key(arg);
        if (fac < 0) {
            ui_print(UI_ERR, "%s <turbine|pump|breaker>", cmd);
        } else if (strcmp(cmd, "resume") == 0) {    /* resume: 설비에 직접 SIGCONT */
            if (fac_mgr_equip_signal(&G.mgr, fac, SIGCONT) != 0)
                ui_print(UI_ERR, "%s: 재개할 프로세스가 없다", arg);
            fac_mgr_request_verify(&G.mgr, fac);
        } else {                                    /* restart: 시설이 새로 띄운다 */
            fac_mgr_equip_restart(&G.mgr, fac);
            fac_mgr_request_verify(&G.mgr, fac);
        }
    } else {
        ui_print(UI_ERR, "%s: 시연에는 없는 명령", cmd);
    }
    return 0;
}

static void push_view(void)
{
    ui_view_t v;
    int fac;

    memset(&v, 0, sizeof v);
    v.game_state = G.state;
    v.peer_connected = 1;
    v.event = G.active_event;
    v.limit_left_ms = G.state == GS_EVENT_ACTIVE ? G.limit_end - now_ms() : 0;
    v.location = G.location;
    if (G.role == ROLE_FIELD)
        snprintf(v.prompt, sizeof v.prompt, "plant/%s", facility_key(G.location));
    else
        snprintf(v.prompt, sizeof v.prompt, "archive");
    if (G.state == GS_READY)
        snprintf(v.notice, sizeof v.notice, "start 를 입력하면 시작");
    for (fac = 0; fac < FAC_COUNT; fac++) {
        v.fac[fac].progress = G.progress[fac];
        v.fac[fac].fault = fac == G.active_fac;
        if (G.has_last[fac]) {
            memcpy(v.fac[fac].metrics, G.last[fac].metrics, sizeof v.fac[fac].metrics);
            v.fac[fac].equip_pid = G.last[fac].equip_pid;
            v.fac[fac].equip_state = G.last[fac].equip_state;
        }
    }
    ui_update(&v);
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int fast = 0, limit_sec = 0, i;

    if (argc < 2 || (strcmp(argv[1], "field") != 0 && strcmp(argv[1], "control") != 0)) {
        fprintf(stderr, "usage: %s field|control [--fast] [--limit <sec>]\n", argv[0]);
        return 2;
    }
    memset(&G, 0, sizeof G);
    G.role = strcmp(argv[1], "control") == 0 ? ROLE_CONTROL : ROLE_FIELD;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--fast") == 0)
            fast = 1;
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit_sec = atoi(argv[++i]);
    }
    G.backtime_ms[FAC_GENERATOR] = fast ? 4000 : 15000;
    G.backtime_ms[FAC_COOLING] = fast ? 6000 : BACKTIME_COOLING_MS;
    G.backtime_ms[FAC_POWER] = fast ? 9000 : BACKTIME_POWER_MS;
    G.limit_ms = limit_sec > 0 ? limit_sec * 1000 : (fast ? 60000 : REPAIR_LIMIT_MS);
    G.active_fac = -1;
    G.active_event = EV_NONE;
    G.location = FAC_COOLING;
    for (i = 0; i < FAC_COUNT; i++)
        G.waiting[i] = EV_NONE;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(FAC_SIG_NOTIFY, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (fixture_create(ROOT) != 0) {
        perror(ROOT);
        return 1;
    }
    /* 시설 프로세스는 ncurses 를 켜기 전에 띄운다 */
    if (fac_mgr_init(&G.mgr, ROOT) != 0 || fac_mgr_spawn_all(&G.mgr) != 0) {
        perror("fac_mgr");
        fac_mgr_shutdown(&G.mgr);
        return 1;
    }
    if (ui_init(G.role) != 0) {
        fprintf(stderr, "터미널에서 실행해야 한다\n");
        fac_mgr_shutdown(&G.mgr);
        return 1;
    }

    G.state = GS_READY;

    while (!g_quit) {
        fd_set rfds;
        struct timeval tv = { 0, 200000 };
        int maxfd = STDIN_FILENO, fd, r;
        char line[256];
        ui_input_t in;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        for (i = 0; i < FAC_COUNT; i++) {
            fd = fac_mgr_fd(&G.mgr, i);
            if (fd >= 0) {
                FD_SET(fd, &rfds);
                if (fd > maxfd)
                    maxfd = fd;
            }
        }
        r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR)
            break;

        /* SIGUSR1 은 select 를 깨우는 역할이다. 내용은 파이프에서 읽는다 */
        g_notify = 0;
        read_facilities();

        while ((in = ui_handle_input(line, sizeof line)) != UI_IN_NONE) {
            if (in == UI_IN_QUIT || run_command(line)) {
                g_quit = 1;
                break;
            }
        }

        if (G.state == GS_EVENT_ACTIVE && now_ms() >= G.limit_end)
            G.state = GS_GAME_OVER;
        if (G.state == GS_REPAIR_SUCCESS && now_ms() >= G.success_until)
            G.state = GS_NORMAL;

        push_view();
        ui_draw();
    }

    ui_shutdown();
    fac_mgr_shutdown(&G.mgr);
    fixture_remove(ROOT);
    return 0;
}
