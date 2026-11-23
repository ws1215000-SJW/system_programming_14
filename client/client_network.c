
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "client_network.h"

// ==========================================
// 1. TCP 서버 연결
// ==========================================
int network_connect(
    const char *server_ip,
    int server_port
) {
    if (server_port < 1 || server_port > 65535) {
        fprintf(stderr, "잘못된 포트 번호\n");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server_port);

    if (inet_pton(AF_INET, server_ip,
                  &addr.sin_addr) != 1) {
        fprintf(stderr, "잘못된 IP 주소\n");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr,
                sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    return sock;
}

// ==========================================
// 2. 데이터 전체 전송
// ==========================================
int network_send_all(
    int sock,
    const void *data,
    size_t length
) {
    const unsigned char *bytes = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(
            sock,
            bytes + sent,
            length - sent,
            MSG_NOSIGNAL
        );

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            perror("send");
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}

// ==========================================
// 3. 길이 헤더 + 메시지 전송
// ==========================================
int network_send_packet(
    int sock,
    const char *message
) {
    size_t length = strlen(message);

    if (length == 0 || length > MAX_MESSAGE_SIZE) {
        fprintf(stderr, "잘못된 메시지 길이\n");
        return -1;
    }

    // 32비트 정수를 네트워크 바이트 순서로 변환
    uint32_t header = htonl((uint32_t)length);

    // 먼저 4바이트 길이 전송
    if (network_send_all(
            sock, &header, sizeof(header)) < 0) {
        return -1;
    }

    // 다음으로 본문 전송
    return network_send_all(sock, message, length);
}

// ==========================================
// 4. 관제사 채팅 전송
// ==========================================
int network_send_chat(
    int sock,
    const char *message
) {
    char packet[MAX_MESSAGE_SIZE + 1];

    int length = snprintf(
        packet,
        sizeof(packet),
        "CHAT|CONTROL|%s",
        message
    );

    if (length < 0 ||
        (size_t)length >= sizeof(packet)) {
        fprintf(stderr, "채팅 메시지가 너무 깁니다.\n");
        return -1;
    }

    return network_send_packet(sock, packet);
}

// ==========================================
// 5. 수신 상태 초기화
// ==========================================
void network_receiver_init(
    NetworkReceiver *receiver
) {
    memset(receiver, 0, sizeof(*receiver));
}

// ==========================================
// 6. 수신 데이터에서 메시지 조립
// ==========================================
int network_receive_packets(
    int sock,
    NetworkReceiver *receiver,
    MessageHandler handler
) {
    unsigned char buffer[2048];

    ssize_t n = recv(sock, buffer, sizeof(buffer), 0);

    if (n == 0)
        return 0;

    if (n < 0) {
        if (errno == EINTR)
            return 1;

        perror("recv");
        return -1;
    }

    size_t offset = 0;
    size_t available = (size_t)n;

    while (offset < available) {

        // ----------------------------
        // A. 4바이트 길이 헤더 조립
        // ----------------------------
        if (receiver->header_received < 4) {

            size_t needed =
                4 - receiver->header_received;

            size_t remaining = available - offset;

            size_t take =
                needed < remaining ? needed : remaining;

            memcpy(
                receiver->header + receiver->header_received,
                buffer + offset,
                take
            );

            receiver->header_received += take;
            offset += take;

            // 헤더가 아직 완성되지 않음
            if (receiver->header_received < 4)
                continue;

            uint32_t network_length;

            memcpy(
                &network_length,
                receiver->header,
                sizeof(network_length)
            );

            receiver->message_length =
                ntohl(network_length);

            // 허용하지 않는 길이 차단
            if (receiver->message_length == 0 ||
                receiver->message_length > MAX_MESSAGE_SIZE) {
                fprintf(stderr, "잘못된 패킷 길이\n");
                return -1;
            }
        }

        // ----------------------------
        // B. 메시지 본문 조립
        // ----------------------------
        size_t needed =
            receiver->message_length -
            receiver->message_received;

        size_t remaining = available - offset;

        size_t take =
            needed < remaining ? needed : remaining;

        memcpy(
            receiver->message + receiver->message_received,
            buffer + offset,
            take
        );

        receiver->message_received += take;
        offset += take;

        // ----------------------------
        // C. 메시지 완성
        // ----------------------------
        if (receiver->message_received ==
            receiver->message_length) {

            receiver->message[receiver->message_length] =
                '\0';

            // 본문은 텍스트 프로토콜이므로
            // 중간 NUL 문자를 허용하지 않음
            if (memchr(
                    receiver->message,
                    '\0',
                    receiver->message_length) != NULL) {
                fprintf(stderr, "잘못된 텍스트 메시지\n");
                return -1;
            }

            // 완성된 메시지 처리
            handler(receiver->message);

            // 다음 메시지를 받을 수 있도록 초기화
            network_receiver_init(receiver);
        }
    }

    return 1;
}
