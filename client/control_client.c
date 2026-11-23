
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

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 12345
#define INPUT_SIZE 1024

static GameState game_state;

// ==========================================
// 1. 수신 메시지 종류별 처리
// ==========================================
static void handle_server_message(const char *message) {

     // 수신한 메시지를 게임 상태에 반영
    int result = game_state_apply(
        &game_state,
        message
    );

    if (result < 0) {
        printf("\n[ERROR] 잘못된 게임 상태 메시지\n");
        return;
    }

    // CHAT|FIELD|내용
    if (strncmp(message, "CHAT|", 5) == 0) {
        const char *sender = message + 5;
        const char *separator = strchr(sender, '|');

        if (separator == NULL) {
            printf("\n[CHAT] 잘못된 메시지\n");
            return;
        }

        printf("\n[%.*s] %s\n",
               (int)(separator - sender),
               sender,
               separator + 1);
    }

    // STATE|...
    else if (strncmp(message, "STATE|", 6) == 0) {
        printf("\n[시설 상태] %s\n", message + 6);
    }

    // EVENT_START|이벤트ID|시설|제한시간
    else if (strncmp(message, "EVENT_START|", 12) == 0) {
        printf("\n================================\n");
        printf("[위험 이벤트 발생]\n");
        printf("%s\n", message + 12);
        printf("================================\n");
    }

    // EVENT_END|이벤트ID|결과
    else if (strncmp(message, "EVENT_END|", 10) == 0) {
        printf("\n[이벤트 종료] %s\n", message + 10);
    }

    // GAME_END|결과
    else if (strncmp(message, "GAME_END|", 9) == 0) {
        printf("\n[게임 종료] %s\n", message + 9);
    }

    // READY 메시지
    else if (strcmp(message, "READY") == 0 ||
             strncmp(message, "READY|", 6) == 0) {
        printf("\n[시스템] 준비 상태: %s\n", message);
    }

    // HELLO 메시지
    else if (strcmp(message, "HELLO") == 0 ||
             strncmp(message, "HELLO|", 6) == 0) {
        printf("\n[시스템] 연결 응답: %s\n", message);
    }

    else {
        printf("\n[알 수 없는 메시지] %s\n", message);
    }
}

// ==========================================
// 2. 전체 관제사 프로그램
// ==========================================
int main(void) {
    ArchiveContext archive;
    NetworkReceiver receiver;

    game_state_init(&game_state);

    char input[INPUT_SIZE];

    // 관제 파일 탐색 초기화
    if (archive_init(&archive) < 0) {
        fprintf(stderr, "archive 초기화 실패\n");
        return 1;
    }

    // TCP 연결
    printf("현장 직원 서버에 접속 중...\n");

    int sock = network_connect(
        SERVER_IP,
        SERVER_PORT
    );

    if (sock < 0)
        return 1;

    network_receiver_init(&receiver);

    printf("\n=== CONTROL CENTER ===\n");
    printf("서버 연결 성공!\n");
    printf("명령어: ls, cd, cat, stat, unlock, say, ready, /quit\n\n");

    // 클라이언트 접속 알림
    if (network_send_packet(sock, "HELLO|CONTROL") < 0) {
        close(sock);
        return 1;
    }

    archive_print_prompt(&archive);

    // 사용자 입력 + 서버 수신 감시
    while (1) {
        fd_set readfds;

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sock, &readfds);

        int max_fd = sock > STDIN_FILENO
                   ? sock : STDIN_FILENO;

        int ready = select(
            max_fd + 1,
            &readfds,
            NULL,
            NULL,
            NULL
        );

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("select");
            break;
        }

        // ----------------------------
        // 사용자 명령어
        // ----------------------------
        if (FD_ISSET(STDIN_FILENO, &readfds)) {

            if (fgets(input, sizeof(input), stdin) == NULL)
                break;

            input[strcspn(input, "\n")] = '\0';

            if (strcmp(input, "/quit") == 0)
                break;

            if (input[0] == '\0') {
                archive_print_prompt(&archive);
                continue;
            }

            // 현재 게임 상태 확인
            if (strcmp(input, "status") == 0) {
                game_state_print(&game_state);
                archive_print_prompt(&archive);
                continue;
            }

            printf("명령어: ls, cd, cat, stat, unlock, status, say, ready, /quit\n\n");

            // 파일 탐색 명령어
            if (handle_archive_command(&archive, input)) {
                archive_print_prompt(&archive);
                continue;
            }

            // 채팅
            if (strncmp(input, "say ", 4) == 0) {
                if (input[4] == '\0') {
                    printf("사용법: say <메시지>\n");
                }
                else if (network_send_chat(
                             sock, input + 4) < 0) {
                    break;
                }
                else {
                    printf("[CONTROL] 메시지 전송 완료\n");
                }
            }

            // 준비 완료
            else if (strcmp(input, "ready") == 0) {
                if (network_send_packet(
                        sock, "READY|CONTROL") < 0) {
                    break;
                }

                printf("[CONTROL] 준비 완료 전송\n");
            }

            else {
                printf("알 수 없는 명령어: %s\n", input);
            }

            archive_print_prompt(&archive);
        }

        // ----------------------------
        // 서버 메시지 수신
        // ----------------------------
        if (FD_ISSET(sock, &readfds)) {

            int result = network_receive_packets(
                sock,
                &receiver,
                handle_server_message
            );

            if (result == 0) {
                printf("\n서버 연결 종료\n");
                break;
            }

            if (result < 0) {
                printf("\n네트워크 수신 오류\n");
                break;
            }

            archive_print_prompt(&archive);
        }
    }

    close(sock);

    printf("\n관제사 프로그램 종료\n");
    return 0;
}
