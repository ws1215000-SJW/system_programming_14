
import socket
import struct
import time

HOST = "127.0.0.1"
PORT = 12345
MAX_MESSAGE_SIZE = 4096


# ==========================================
# 1. 길이 기반 패킷 생성
# ==========================================
def packet(message):
    data = message.encode("utf-8")

    if not 0 < len(data) <= MAX_MESSAGE_SIZE:
        raise ValueError("Invalid message size")

    # 4바이트 빅엔디언 길이 + 본문
    return struct.pack("!I", len(data)) + data


# ==========================================
# 2. 지정한 바이트 수만큼 수신
# ==========================================
def recv_exact(conn, size):
    data = b""

    while len(data) < size:
        chunk = conn.recv(size - len(data))

        if not chunk:
            raise ConnectionError("Client disconnected")

        data += chunk

    return data


# ==========================================
# 3. 클라이언트 메시지 수신
# ==========================================
def receive_message(conn):
    header = recv_exact(conn, 4)

    length = struct.unpack("!I", header)[0]

    if not 0 < length <= MAX_MESSAGE_SIZE:
        raise ValueError("Invalid packet length")

    body = recv_exact(conn, length)

    return body.decode("utf-8")


# ==========================================
# 4. 메시지 전송
# ==========================================
def send_message(conn, message):
    conn.sendall(packet(message))
    print(f"[SERVER SEND] {message}", flush=True)


# ==========================================
# 5. 서버에서 클라이언트 명령어 처리
# ==========================================
def handle_client_message(conn, message):
    print(f"[CLIENT RECV] {message}", flush=True)

    if message == "HELLO|CONTROL":
        send_message(conn, "HELLO|FIELD")

    elif message == "READY|CONTROL":
        send_message(conn, "READY|FIELD")

    elif message.startswith("CHAT|CONTROL|"):
        send_message(
            conn,
            "CHAT|FIELD|메시지 확인했습니다."
        )


# ==========================================
# 6. 게임 이벤트 테스트
# ==========================================
def run_game_test(conn):

    print("\n=== GAME STATE TEST START ===")

    # STEP 1. 위험 이벤트 발생
    send_message(
        conn,
        "EVENT_START|C02|cooling|300"
    )

    # STEP 2. 펌프 정지 상태
    send_message(
        conn,
        "STATE|cooling|pump=STOPPED|recovery=0"
    )

    print("\n[STEP 1] 위험 이벤트 발생")
    print("클라이언트에서 status를 확인하세요.")

    time.sleep(10)

    # STEP 3. 펌프 복구 상태
    send_message(
        conn,
        "STATE|cooling|pump=RUNNING|recovery=50"
    )

    # STEP 4. 이벤트 종료
    send_message(
        conn,
        "EVENT_END|C02|SUCCESS"
    )

    print("\n[STEP 2] 수리 완료")
    print("클라이언트에서 status를 확인하세요.")

    time.sleep(10)

    # STEP 5. 게임 종료
    send_message(
        conn,
        "GAME_END|CLEAR"
    )

    print("\n[STEP 3] 게임 클리어 메시지 전송")
    print("클라이언트에서 status를 확인하세요.")


# ==========================================
# 7. TCP 서버 실행
# ==========================================
def main():

    with socket.socket(
        socket.AF_INET,
        socket.SOCK_STREAM
    ) as server:

        server.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_REUSEADDR,
            1
        )

        server.bind((HOST, PORT))
        server.listen(1)

        print("================================")
        print(" POWER PLANT TEST SERVER")
        print("================================")
        print(f"Listening on {HOST}:{PORT}")
        print("관제사 클라이언트 접속 대기 중...\n")

        conn, addr = server.accept()

        with conn:
            print(f"클라이언트 연결: {addr}")

            # 클라이언트 최초 HELLO 수신
            try:
                hello = receive_message(conn)
                handle_client_message(conn, hello)

                # 게임 상태 테스트
                run_game_test(conn)

                print("\n게임 상태 테스트 완료")
                print("채팅 및 ready 명령어 테스트 가능")
                print("종료하려면 클라이언트에서 /quit 입력\n")

                # 이후 메시지 송수신
                while True:
                    message = receive_message(conn)
                    handle_client_message(conn, message)

            except (
                ConnectionError,
                ConnectionResetError,
                BrokenPipeError
            ):
                print("\n클라이언트 연결 종료")

            except ValueError as e:
                print(f"\n프로토콜 오류: {e}")

    print("\n테스트 서버 종료")


if __name__ == "__main__":
    main()
