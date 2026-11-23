
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "ui.h"

// 관제사 UI 단독 실행 테스트
int main(void) {
    ui_view_t view;
    memset(&view, 0, sizeof(view));

    // 초기 게임 상태 설정
    view.game_state = GS_NORMAL;
    view.peer_connected = 1;
    view.event = EV_NONE;
    view.location = -1;

    snprintf(
        view.prompt,
        sizeof(view.prompt),
        "archive/"
    );

    // 시설별 테스트 정보
    for (int i = 0; i < FAC_COUNT; i++) {
        view.fac[i].progress = 0;
        view.fac[i].fault = 0;

        for (int j = 0; j < METRIC_COUNT; j++) {
            view.fac[i].metrics[j] = NAN;
        }
    }

    // 냉각실 테스트 데이터
    view.fac[FAC_COOLING].progress = 50;
    view.fac[FAC_COOLING].metrics[0] = 45.0f;
    view.fac[FAC_COOLING].metrics[1] = 80.0f;
    view.fac[FAC_COOLING].metrics[2] = 120.0f;
    view.fac[FAC_COOLING].metrics[3] = 90.0f;

    // ncurses 초기화
    if (ui_init(ROLE_CONTROL) < 0) {
        fprintf(stderr, "UI 초기화 실패\n");
        return 1;
    }

    ui_update(&view);

    ui_print(
        UI_SYS,
        "관제사 UI 테스트 시작"
    );

    ui_print(
        UI_OUT,
        "명령어를 입력하세요. 종료: quit"
    );

    ui_chat(
        ROLE_FIELD,
        "냉각실 상태를 확인해 주세요."
    );

    // 입력 처리 루프
    int running = 1;
    char input[256];

    while (running) {
        ui_input_t result;

        while ((result = ui_handle_input(
                    input,
                    sizeof(input))) != UI_IN_NONE) {

            if (result == UI_IN_QUIT) {
                running = 0;
                break;
            }

            if (strcmp(input, "quit") == 0 ||
                strcmp(input, "/quit") == 0) {
                running = 0;
                break;
            }

            if (strcmp(input, "event") == 0) {
                view.game_state = GS_EVENT_ACTIVE;
                view.event = EV_C02;
                view.limit_left_ms = 300000;
                view.fac[FAC_COOLING].fault = 1;

                ui_print(
                    UI_SYS,
                    "C02 위험 이벤트 발생"
                );
            }
            else if (strcmp(input, "repair") == 0) {
                view.game_state = GS_REPAIR_SUCCESS;
                view.event = EV_NONE;
                view.fac[FAC_COOLING].fault = 0;
                view.fac[FAC_COOLING].progress = 100;

                ui_print(
                    UI_OK,
                    "냉각실 복구 성공"
                );
            }
            else if (strncmp(input, "say ", 4) == 0) {
                ui_chat(ROLE_CONTROL, input + 4);
            }
            else {
                ui_print(
                    UI_OUT,
                    "입력: %s",
                    input
                );
            }
        }

        ui_update(&view);
        ui_draw();

        // CPU 과다 사용 방지
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 20000000L
        };

        nanosleep(&delay, NULL);
    }

    ui_shutdown();

    printf("UI 테스트 종료\n");
    return 0;
}
