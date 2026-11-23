
#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

#include <stddef.h>
#include <stdint.h>

#define MAX_MESSAGE_SIZE 4096

// TCP 수신 중인 메시지의 상태
typedef struct {
    unsigned char header[4];
    size_t header_received;

    uint32_t message_length;

    char message[MAX_MESSAGE_SIZE + 1];
    size_t message_received;
} NetworkReceiver;

// 완성된 메시지를 처리할 콜백 함수
typedef void (*MessageHandler)(const char *message);

// 호스트 서버 연결
int network_connect(
    const char *server_ip,
    int server_port
);

// 데이터 전체 전송
int network_send_all(
    int sock,
    const void *data,
    size_t length
);

// 길이 헤더 + 본문 전송
int network_send_packet(
    int sock,
    const char *message
);

// 채팅 전송
int network_send_chat(
    int sock,
    const char *message
);

// 수신 상태 초기화
void network_receiver_init(NetworkReceiver *receiver);

// 데이터 수신 및 메시지 재조립
//  1: 정상 수신
//  0: 연결 종료
// -1: 오류
int network_receive_packets(
    int sock,
    NetworkReceiver *receiver,
    MessageHandler handler
);

#endif
