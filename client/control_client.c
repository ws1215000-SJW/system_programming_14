
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#include "archive_browser.h"
#include "client_network.h"
#include "game_state.h"
#include "ui_adapter.h"
#include "ui.h"

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 12345
#define INPUT_SIZE 256

// 네트워크 수신 콜백에서 사용할 게임 상태
static GameState game_state;

// ==========================================
// 1. archive 출력 -> ncurses 명령 출력창
// ==========================================
static void archive_ui_output(
    void *userdata,
    const char *text
) {
    (void)userdata;

    ui_print(UI_OUT, "%s", text);
}

// ==========================================
// 2. 현재 게임 상태 출력
// ==========================================
static void show_game_status(void) {
    ui_print(UI_SYS, "=== GAME STATUS ===");

    for (int i = 0; i < FACILITY_COUNT; i++) {
        const FacilityState *facility =
            &game_state.facilities[i];

        ui_print(
            UI_OUT,
            "[%s]",
            facility->name
        );

        if (facility->field_count == 0) {
            ui_print(
                UI_OUT,
                "  수신된 상태 정보 없음"
            );
        }

        for (int j = 0;
             j < facility->field_count;
             j++) {

            ui_print(
                UI_OUT,
                "  %s: %s",
                facility->fields[j].key,
                facility->fields[j].value
            );
        }
    }

    if (game_state.event_active) {
        ui_print(
            UI_SYS,
            "진행 중인 이벤트: %s / %s / %d초",
            game_state.event_id,
            game_state.event_facility,
            game_state.repair_limit
        );
    }
    else {
        ui_print(
            UI_OUT,
            "진행 중인 이벤트 없음"
        );
    }

    if (game_state.last_event_result[0] != '\0') {
        ui_print(
            UI_OUT,
            "최근 이벤트 결과: %s",
            game_state.last_event_result
        );
    }

    if (game_state.game_result[0] != '\0') {
        ui_print(
            UI_SYS,
            "게임 결과: %s",
            game_state.game_result
        );
    }
}

// ==========================================
// 3. 서버 메시지 수신 처리
// ==========================================
static void handle_server_message(
    const char *message
) {
    // 게임 상태 메시지라면 먼저 저장
    int result = game_state_apply(
        &game_state,
        message
    );

    if (result < 0) {
        ui_print(
            UI_ERR,
            "잘못된 게임 상태 메시지: %s",
            message
        );
        return;
    }

    // CHAT|FIELD|내용
    if (strncmp(message, "CHAT|", 5) == 0) {
        const char *sender = message + 5;
        const char *separator = strchr(sender, '|');

        if (separator == NULL) {
            ui_print(
                UI_ERR,
                "잘못된 CHAT 메시지"
            );
            return;
        }

        size_t sender_length =
            (size_t)(separator - sender);

        if (sender_length == 5 &&
            strncmp(sender, "FIELD", 5) == 0) {

            ui_chat(
                ROLE_FIELD,
                separator + 1
            );
        }
        else if (sender_length == 7 &&
                 strncmp(sender, "CONTROL", 7) == 0) {

            ui_chat(
                ROLE_CONTROL,
                separator + 1
            );
        }
        else {
            ui_print(
                UI_ERR,
                "알 수 없는 CHAT 발신자"
            );
        }

        return;
    }

    // STATE|시설|key=value...
    if (strncmp(message, "STATE|", 6) == 0) {
        ui_print(
            UI_SYS,
            "시설 상태 갱신: %s",
            message + 6
        );
        return;
    }

    // EVENT_START|이벤트ID|시설|제한시간
    if (strncmp(
            message,
            "EVENT_START|",
            12
        ) == 0) {

        ui_print(
            UI_ERR,
            "위험 이벤트 발생: %s",
            message + 12
        );
        return;
    }

    // EVENT_END|이벤트ID|결과
    if (strncmp(
            message,
            "EVENT_END|",
            10
        ) == 0) {

        ui_print(
            UI_OK,
            "위험 이벤트 종료: %s",
            message + 10
        );
        return;
    }

    // GAME_END|결과
    if (strncmp(
            message,
            "GAME_END|",
            9
        ) == 0) {

        ui_print(
            UI_SYS,
            "게임 종료: %s",
            message + 9
        );
        return;
    }

    // HELLO
    if (strcmp(message, "HELLO") == 0 ||
        strncmp(message, "HELLO|", 6) == 0) {

        ui_print(
            UI_OK,
            "호스트 연결 응답: %s",
            message
        );
        return;
    }

    // READY
    if (strcmp(message, "READY") == 0 ||
        strncmp(message, "READY|", 6) == 0) {

        ui_print(
            UI_OK,
            "준비 상태 수신: %s",
            message
        );
        return;
    }

    ui_print(
        UI_OUT,
        "서버 메시지: %s",
        message
    );
}

// ==========================================
// 4. 명령어 처리
// 반환값: 1 계속 실행, 0 종료
// ==========================================
static int process_command(
    ArchiveContext *archive,
    int sock,
    const char *input
) {
    if (input[0] == '\0')
        return 1;

    // 프로그램 종료
    if (strcmp(input, "/quit") == 0 ||
        strcmp(input, "quit") == 0) {
        return 0;
    }

    // 도움말
    if (strcmp(input, "help") == 0) {
        ui_print(
            UI_SYS,
            "명령어: ls, cd, cat, stat, unlock"
        );

        ui_print(
            UI_SYS,
            "status, say <내용>, ready, /quit"
        );

        return 1;
    }

    // 현재 게임 상태 확인
    if (strcmp(input, "status") == 0) {
        show_game_status();
        return 1;
    }

    // 파일 탐색 명령어
    if (handle_archive_command(
            archive,
            input
        )) {
        return 1;
    }

    // 채팅 전송
    if (strncmp(input, "say ", 4) == 0) {
        const char *message = input + 4;

        if (*message == '\0') {
            ui_print(
                UI_ERR,
                "사용법: say <메시지>"
            );
            return 1;
        }

        if (network_send_chat(
                sock, message) < 0) {

            ui_print(
                UI_ERR,
                "채팅 전송 실패"
            );
            return 0;
        }

        // 내가 보낸 채팅도 통신창에 표시
        ui_chat(
            ROLE_CONTROL,
            message
        );

        return 1;
    }

    if (strcmp(input, "say") == 0) {
        ui_print(
            UI_ERR,
            "사용법: say <메시지>"
        );
        return 1;
    }

    // 준비 완료
    if (strcmp(input, "ready") == 0) {
        if (network_send_packet(
                sock, "READY|CONTROL") < 0) {

            ui_print(
                UI_ERR,
                "READY 전송 실패"
            );
            return 0;
        }

        ui_print(
            UI_OK,
            "준비 완료 메시지 전송"
        );

        return 1;
    }

    ui_print(
        UI_ERR,
        "알 수 없는 명령어: %s",
        input
    );

    return 1;
}

// ==========================================
// 5. 관제사 프로그램 메인
// ==========================================
int main(void) {
    ArchiveContext archive;
    NetworkReceiver receiver;
    ui_view_t view;

    char input[INPUT_SIZE];
    int running = 1;
    int peer_connected = 0;

    // ----------------------------
    // 게임 상태 초기화
    // ----------------------------
    game_state_init(&game_state);

    if (archive_init(&archive) < 0) {
        fprintf(
            stderr,
            "archive 초기화 실패\n"
        );
        return 1;
    }

    // ----------------------------
    // 호스트 연결
    // ----------------------------
    printf("현장 직원 서버에 연결 중...\n");

    int sock = network_connect(
        SERVER_IP,
        SERVER_PORT
    );

    if (sock < 0) {
        fprintf(
            stderr,
            "호스트 연결 실패\n"
        );
        return 1;
    }

    peer_connected = 1;
    network_receiver_init(&receiver);

    // ----------------------------
    // ncurses 초기화
    // ----------------------------
    if (ui_init(ROLE_CONTROL) < 0) {
        fprintf(
            stderr,
            "ncurses 초기화 실패\n"
        );
        close(sock);
        return 1;
    }

    // 파일 탐색 출력 함수를 ncurses로 전환
    archive_set_output(
        &archive,
        archive_ui_output,
        NULL
    );

    ui_print(
        UI_OK,
        "현장 직원 서버 연결 성공"
    );

    ui_print(
        UI_SYS,
        "관제사 프로그램 시작"
    );

    ui_print(
        UI_OUT,
        "명령어 목록: help"
    );

    // 연결 알림
    if (network_send_packet(
            sock, "HELLO|CONTROL") < 0) {

        ui_print(
            UI_ERR,
            "HELLO 전송 실패"
        );
        running = 0;
    }

    // ----------------------------
    // 메인 루프
    // ----------------------------
    while (running) {

        // 1. ncurses 키 입력 처리
        ui_input_t input_result;

        while ((input_result = ui_handle_input(
                    input,
                    sizeof(input))) != UI_IN_NONE) {

            if (input_result == UI_IN_QUIT) {
                running = 0;
                break;
            }

            if (input_result == UI_IN_LINE) {
                running = process_command(
                    &archive,
                    sock,
                    input
                );

                if (!running)
                    break;
            }
        }

        if (!running)
            break;

        // 2. 서버 소켓 수신 확인
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;

        int ready = select(
            sock + 1,
            &readfds,
            NULL,
            NULL,
            &timeout
        );

        if (ready < 0) {
            if (errno != EINTR) {
                ui_print(
                    UI_ERR,
                    "select 오류: %s",
                    strerror(errno)
                );
                running = 0;
            }
        }
        else if (ready > 0 &&
                 FD_ISSET(sock, &readfds)) {

            int result = network_receive_packets(
                sock,
                &receiver,
                handle_server_message
            );

            if (result == 0) {
                peer_connected = 0;

                ui_print(
                    UI_ERR,
                    "호스트 연결 종료"
                );

                running = 0;
            }
            else if (result < 0) {
                peer_connected = 0;

                ui_print(
                    UI_ERR,
                    "TCP 수신 오류"
                );

                running = 0;
            }
        }

        // 3. 게임 상태 -> 화면 데이터 변환
        ui_adapter_build(
            &game_state,
            &archive,
            peer_connected,
            &view
        );

        // 4. 화면 갱신
        ui_update(&view);
        ui_draw();
    }

    // ----------------------------
    // 종료
    // ----------------------------
    ui_shutdown();
    close(sock);

    printf("관제사 프로그램 종료\n");

    return 0;
}
