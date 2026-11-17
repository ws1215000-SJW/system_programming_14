#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <time.h>

#define COOLING_DIR "plant/cooling/"
#define C01_FAULT_FLOW 40
#define C01_NORMAL_FLOW 70
#define COOLANT_MAX_FLOW 100
#define C01_BACKTIME_SEC 10.0
#define C01_REPAIR_SEC 20.0

static double now_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static void show_file(const char *path)
{
    char buffer[256];
    ssize_t count;
    int last_char = '\n';
    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        perror(path);
        return;
    }

    while ((count = read(fd, buffer, sizeof buffer)) > 0) {
        fwrite(buffer, 1, (size_t)count, stdout);
        last_char = buffer[count - 1];
    }

    if (last_char != '\n')
        putchar('\n');

    if (count == -1)
        perror("read");

    close(fd);
}

static int save_value(const char *path, const char *key, int value)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        perror(path);
        return 0;
    }

    int written = fprintf(file, "%s=%d\n", key, value);
    int closed = fclose(file);

    if (written < 0 || closed == EOF) {
        fprintf(stderr, "저장 실패: %s\n", path);
        return 0;
    }

    return 1;
}

static void start_c01(int *c01_active)
{
    if (*c01_active) {
        puts("[테스트] C01이 이미 진행 중입니다.");
        return;
    }

    if (!save_value(COOLING_DIR "coolant_config.cfg",
                    "target_flow", C01_FAULT_FLOW))
        return;

    if (!save_value(COOLING_DIR "coolant_level.txt",
                    "current_flow", C01_FAULT_FLOW))
        return;

    *c01_active = 1;
    puts("[테스트] C01 발생: 냉각수 유량 저하");
}

static void repair_coolant(const char *args, int in_cooling, int *c01_active)
{
    char target[32];
    char value[32];
    char extra;
    char *end;

    if (sscanf(args, "%31s %31s %c", target, value, &extra) != 2 ||
        strcmp(target, "coolant_flow") != 0) {
        puts("사용법: repair coolant_flow <값>");
        return;
    }

    if (!in_cooling) {
        puts("먼저 go cooling을 입력하세요.");
        return;
    }

    errno = 0;
    long flow = strtol(value, &end, 10);

    if (errno == ERANGE || end == value || *end != '\0' ||
        flow < 0 || flow > COOLANT_MAX_FLOW) {
        printf("유량은 0~%d 사이의 정수로 입력하세요.\n", COOLANT_MAX_FLOW);
        return;
    }

    if (!save_value(COOLING_DIR "coolant_config.cfg",
                    "target_flow", (int)flow))
        return;

    /* 현재 버전에서는 펌프가 설정값을 바로 따라간다고 가정한다. */
    if (!save_value(COOLING_DIR "coolant_level.txt",
                    "current_flow", (int)flow)) {
        puts("목표 유량은 저장됐지만 현재 유량을 갱신하지 못했습니다.");
        return;
    }

    /* 설정을 저장한 뒤, 진행 중인 C01의 해소 여부를 확인한다. */
    if (*c01_active && flow == C01_NORMAL_FLOW)
        *c01_active = 0;

    printf("목표 유량을 %ld로 설정했습니다.\n", flow);
}   

int main(void)
{
    char command[128];
    int in_cooling = 0;
    int c01_active = 0;
    int auto_event_used = 0;
    int show_prompt = 1;

    double backtime_end = now_seconds() + C01_BACKTIME_SEC;
    double repair_end = 0.0;

    setvbuf(stdin, NULL, _IONBF, 0);

    while (1) {
        if (show_prompt) {
            printf("관리자> ");
            fflush(stdout);
            show_prompt = 0;
        }

        /* 입력이 없어도 0.1초마다 타이머를 확인한다. */
        fd_set inputs;
        FD_ZERO(&inputs);
        FD_SET(STDIN_FILENO, &inputs);

        struct timeval timeout = {0, 100000};
        int ready = select(STDIN_FILENO + 1, &inputs, NULL, NULL, &timeout);

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("select");
            return EXIT_FAILURE;
        }

        double now = now_seconds();

        /* 백타임이 끝나면 자동 고장을 한 번만 발생시킨다. */
        if (!auto_event_used && now >= backtime_end) {
            auto_event_used = 1;
            putchar('\n');
            start_c01(&c01_active);

            if (c01_active) {
                repair_end = now_seconds() + C01_REPAIR_SEC;
                printf("수리 제한 시간: %.0f초\n", C01_REPAIR_SEC);
            }

            show_prompt = 1;
        }

        if (c01_active && now >= repair_end) {
            puts("\n수리 제한 시간 초과: 게임 오버");
            break;
        }

        if (ready == 0)
            continue;

        if (fgets(command, sizeof command, stdin) == NULL)
            break;

        /* 명령을 처리하기 전에 제한 시간을 한 번 더 확인한다. */
        if (c01_active && now_seconds() >= repair_end) {
            puts("수리 제한 시간 초과: 게임 오버");
            break;
        }

        command[strcspn(command, "\r\n")] = '\0';
        show_prompt = 1;

        if (strcmp(command, "quit") == 0) {
            break;

        } else if (strcmp(command, "go cooling") == 0) {
            in_cooling = 1;
            puts("냉각실로 이동했습니다.");

        } else if (strncmp(command, "cat ", 4) == 0) {
            const char *name = command + 4;

            if (!in_cooling) {
                puts("먼저 go cooling을 입력하세요.");
            } else if (strcmp(name, "pump_label.txt") == 0) {
                show_file(COOLING_DIR "pump_label.txt");
            } else if (strcmp(name, "coolant_config.cfg") == 0) {
                show_file(COOLING_DIR "coolant_config.cfg");
            } else if (strcmp(name, "coolant_level.txt") == 0) {
                show_file(COOLING_DIR "coolant_level.txt");
            } else {
                puts("냉각실에서 볼 수 있는 파일이 아닙니다.");
            }

        } else if (strcmp(command, "repair") == 0) {
            puts("사용법: repair coolant_flow <값>");

        } else if (strncmp(command, "repair ", 7) == 0) {
            repair_coolant(command + 7, in_cooling, &c01_active);

        } else if (strcmp(command, "debug c01") == 0) {
            if (c01_active) {
                puts("[테스트] C01이 이미 진행 중입니다.");
            } else {
                start_c01(&c01_active);

                if (c01_active) {
                    auto_event_used = 1;
                    repair_end = now_seconds() + C01_REPAIR_SEC;
                    printf("수리 제한 시간: %.0f초\n", C01_REPAIR_SEC);
                }
            }

        } else if (strcmp(command, "debug status") == 0) {
            puts(c01_active ? "[테스트] C01 진행 중"
                            : "[테스트] 진행 중인 C01 없음");

        } else {
            puts("사용 가능한 명령: go cooling, cat <파일명>, "
                 "repair coolant_flow <값>, quit");
            puts("테스트 명령: debug c01, debug status");
        }
    }

    return 0;
}