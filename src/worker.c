#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fac_mgr.h"
#include "kvfile.h"
#include "timeutil.h"

#define ROOT "plant"
#define COOL ROOT "/cooling/"
#define POWER ROOT "/power/"
#define FLOW_CFG COOL "coolant_config.cfg"
#define FLOW_LEVEL COOL "coolant_level.txt"
#define POWER_CFG POWER "power_config.cfg"
#define SENSOR_LINK COOL "cooling_sensor_link"
#define SENSOR_A "cooling_sensor_a.dat"
#define SENSOR_B "cooling_sensor_b.dat"
#define BACKTIME_MS 10000
#define REPAIR_MS 20000

static const int events[] = {
    EV_C01, EV_C03, EV_P01, EV_P02, EV_P03, EV_NONE
};

typedef struct {
    fac_mgr_t mgr;
    int step, active, location, pending, breaker_wait;
    int64_t deadline;
} worker_t;

static volatile sig_atomic_t quitting;

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
    return set_pair(FLOW_CFG, "target_flow", value,
                    FLOW_LEVEL, "current_flow", value);
}

static int set_load(int a, int b)
{
    return set_pair(POWER_CFG, "circuit_a_load", a,
                    POWER_CFG, "circuit_b_load", b);
}

static int set_split(int generator, int cooling)
{
    return set_pair(POWER_CFG, "supply_generator", generator,
                    POWER_CFG, "supply_cooling", cooling);
}

static int relink_sensor(const char *target)
{
    struct stat st;
    char temp[sizeof SENSOR_LINK + 32];

    if (lstat(SENSOR_LINK, &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            errno = EEXIST;
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    snprintf(temp, sizeof temp, "%s.tmp.%ld", SENSOR_LINK, (long)getpid());
    if (symlink(target, temp) < 0)
        return -1;
    if (rename(temp, SENSOR_LINK) < 0) {
        int saved = errno;
        unlink(temp);
        errno = saved;
        return -1;
    }
    return 0;
}

static int inject_fault(int event)
{
    switch (event) {
    case EV_C01:
        return set_flow(40);
    case EV_C03:
        return relink_sensor(SENSOR_A);
    case EV_P01:
        return set_load(118, 12);
    case EV_P02:
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
    return fac_mgr_arm(&w->mgr, event_facility(event), event, delay, 0);
}

static void request_verify(worker_t *w)
{
    if (fac_mgr_request_verify(&w->mgr, event_facility(w->active)) < 0) {
        perror("수리 확인 요청");
        return;
    }
    w->pending = 1;
}

/* -1: 오류, 0: 기존 프롬프트 유지, 1: 프롬프트 다시 표시 */
static int handle_report(worker_t *w, int fac, const fac_msg_t *msg)
{
    int event = msg->event;

    if (msg->type == FMSG_EVENT && w->active == EV_NONE &&
        event != EV_NONE && event == events[w->step] &&
        fac == event_facility(event)) {
        if (inject_fault(event) < 0) {
            fprintf(stderr, "%s 고장 적용 실패\n", event_code(event));
            return -1;
        }
        fac_mgr_pause_others(&w->mgr, fac);
        w->active = event;
        w->deadline = now_ms() + REPAIR_MS;
        printf("\n%s 발생: %s (제한 %d초)\n",
               event_code(event), event_title(event), REPAIR_MS / 1000);
        return 1;
    }

    /* P02는 차단기가 가동 중이라는 보고를 받은 뒤 수리 판정을 요청한다. */
    if (w->active == EV_P02 && fac == FAC_POWER && event == EV_P02 &&
        w->breaker_wait && (msg->type == FMSG_STATUS || msg->type == FMSG_EQUIP) &&
        msg->equip_state == EQ_RUNNING) {
        w->breaker_wait = 0;
        request_verify(w);
    }

    if (msg->type == FMSG_VERIFY && w->pending && event == w->active &&
        fac == event_facility(event)) {
        w->pending = 0;
        if (!msg->verify_ok) {
            printf("\n%s 수리 미완료: %s\n", event_code(event), msg->detail);
            return 1;
        }

        printf("\n%s 수리 완료\n", event_code(event));
        w->active = EV_NONE;
        w->step++;
        fac_mgr_resume_others(&w->mgr, fac);

        if (arm_next(w, BACKTIME_MS) < 0) {
            perror("다음 이벤트 예약");
            return -1;
        }
        if (events[w->step] == EV_NONE)
            puts("냉각실·배전실 테스트 이벤트를 모두 수리했습니다.");
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

    if (location == FAC_COOLING) {
        if (!strcmp(name, "pump_label.txt"))
            path = COOL "pump_label.txt";
        else if (!strcmp(name, "coolant_config.cfg"))
            path = FLOW_CFG;
        else if (!strcmp(name, "coolant_level.txt"))
            path = FLOW_LEVEL;
    } else if (location == FAC_POWER) {
        if (!strcmp(name, "power_config.cfg"))
            path = POWER_CFG;
        else if (!strcmp(name, "breaker_status.txt"))
            path = POWER "breaker_status.txt";
        else if (!strcmp(name, "circuit_label.txt"))
            path = POWER "circuit_label.txt";
    }

    if (path)
        show_file(path);
    else
        puts("현재 위치에서 열람할 수 없는 파일입니다.");
}

static void show_sensor_link(void)
{
    char target[256];
    ssize_t n = readlink(SENSOR_LINK, target, sizeof target - 1);

    if (n < 0) {
        perror(SENSOR_LINK);
        return;
    }
    target[n] = '\0';
    printf("cooling_sensor_link -> %s\n", target);
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

    if (n < 3)
        goto usage;

    if (!strcmp(v[1], "coolant_flow"))
        expected = EV_C01;
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
        if (relink_sensor(!strcmp(v[2], "A") ? SENSOR_A : SENSOR_B) < 0) {
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
        if (n != 3 || !number(v[2], 1, &a))
            goto usage;
        if (set_number(POWER_CFG, "fault_isolated", a) < 0)
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
    puts("사용법: repair coolant_flow <0~100> | cooling_sensor <A|B>");
    puts("        repair circuit_load <A> <B> | fault_isolated <0|1>");
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

    if (n == 1 && !strcmp(v[0], "quit"))
        return 1;

    if (n == 2 && !strcmp(v[0], "go") && !strcmp(v[1], "cooling")) {
        w->location = FAC_COOLING;
        puts("냉각실로 이동했습니다.");
    } else if (n == 2 && !strcmp(v[0], "go") && !strcmp(v[1], "power")) {
        w->location = FAC_POWER;
        puts("배전실로 이동했습니다.");
    } else if (n == 2 && !strcmp(v[0], "cat")) {
        cat_file(w->location, v[1]);
    } else if (n == 2 && !strcmp(v[0], "stat") &&
               !strcmp(v[1], "cooling_sensor_link")) {
        if (w->location == FAC_COOLING)
            show_sensor_link();
        else
            puts("먼저 go cooling을 입력하세요.");
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
    } else if (n == 2 && !strcmp(v[0], "debug") &&
               (!strcmp(v[1], "next") ||
                (events[w->step] == EV_C01 && !strcmp(v[1], "c01")) ||
                (events[w->step] == EV_C03 && !strcmp(v[1], "c03")))) {
        if (w->active != EV_NONE || events[w->step] == EV_NONE)
            puts("지금 발생시킬 대기 이벤트가 없습니다.");
        else if (arm_next(w, 0) < 0)
            perror("이벤트 예약");
    } else {
        puts("go cooling | go power | cat <파일> | stat cooling_sensor_link");
        puts("repair | resume breaker | restart breaker | debug status | debug next | quit");
    }
    return 0;
}

int main(void)
{
    worker_t w = {.active = EV_NONE, .location = -1};
    char line[128];
    size_t used = 0;
    int overflow = 0, prompt = 1, result = EXIT_SUCCESS;

    if (install_signals() < 0 || fac_mgr_init(&w.mgr, ROOT) < 0) {
        perror("호스트 초기화");
        return EXIT_FAILURE;
    }

    if (access(COOL SENSOR_A, R_OK) < 0 ||
        access(COOL SENSOR_B, R_OK) < 0 ||
        relink_sensor(SENSOR_B) < 0 ||
        access(POWER_CFG, R_OK | W_OK) < 0 ||
        fac_mgr_spawn_all(&w.mgr) < 0 ||
        arm_next(&w, BACKTIME_MS) < 0) {
        perror("시설 또는 현장 파일 초기화");
        result = EXIT_FAILURE;
        goto done;
    }

    while (!quitting) {
        struct pollfd fds[FAC_COUNT + 1] = {{STDIN_FILENO, POLLIN, 0}};

        if (prompt) {
            printf("발전소 직원> ");
            fflush(stdout);
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
                if (input[i] == '\n') {
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
                } else if (used < sizeof line - 1) {
                    line[used++] = input[i];
                } else {
                    overflow = 1;
                }
            }
            fflush(stdout);
        }
    }

done:
    fac_mgr_shutdown(&w.mgr);
    return result;
}