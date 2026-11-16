/*
 * ui.h - ncursesw 화면. 발전소 직원(호스트)과 관제사(클라이언트)가 같은 모듈을 쓴다.
 *
 * 화면은 게임 규칙을 모른다. 프로그램이 ui_view_t 를 채워 넘기면 그대로 그리고,
 * 입력줄에서 완성된 한 줄을 돌려준다. 명령 해석과 네트워크는 부르는 쪽이 한다.
 *
 *   발전소 직원                                관제사
 *   +--------------------------------------+   +--------------------------------------+
 *   | 머리줄: 상태, 이벤트, 남은 시간      |   | 머리줄                               |
 *   +----------------------+---------------+   +------------+------------+------------+
 *   | 명령 출력            | 지금 있는     |   | 발전실     | 냉각실     | 배전실     |
 *   |                      | 시설의 계기판 |   | 복구율, 수치 (세 시설 모두)          |
 *   |                      +---------------+   +----------------------+---------------+
 *   |                      | 통신          |   | 명령 출력            | 통신          |
 *   +----------------------+---------------+   +----------------------+---------------+
 *   | 입력줄                               |   | 입력줄                               |
 *   +--------------------------------------+   +--------------------------------------+
 *
 * 쓰는 순서
 *   ui_init(role);
 *   loop:  select(STDIN_FILENO, 소켓, 파이프..., 200ms 쯤)
 *          stdin 을 읽을 수 있으면  while ((r = ui_handle_input(line, sizeof line)) != UI_IN_NONE) ...
 *          ui_update(&view);  ui_draw();
 *   ui_shutdown();
 * 시설 프로세스는 ui_init 전에 fork 한다.
 */
#ifndef UI_H
#define UI_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

#define UI_MIN_ROWS 24
#define UI_MIN_COLS 80

/* 시설 하나의 표시 값 */
typedef struct {
    int   progress;                 /* 복구율 0..100 (관제사 화면만 그린다) */
    int   fault;                    /* 위험 이벤트 진행 중 */
    float metrics[METRIC_COUNT];    /* metric_label 순서. NaN 은 수신 불가 */
    int   equip_pid;                /* 설비 프로세스 (직원 화면만 그린다). 모르면 0 */
    int   equip_state;              /* equip_state_t */
} ui_fac_t;

typedef struct {
    int      game_state;            /* game_state_t */
    int      peer_connected;        /* 상대 플레이어 접속 여부 */
    int      event;                 /* 진행 중인 이벤트, 없으면 EV_NONE */
    int64_t  limit_left_ms;         /* 남은 수리 제한 시간 (GS_EVENT_ACTIVE 일 때) */
    int      location;              /* 직원이 있는 시설, 어디에도 없으면 -1 (관제사 화면은 무시) */
    char     prompt[64];            /* 입력줄 앞에 보일 경로. 예: "plant/cooling", "archive/power" */
    char     notice[160];           /* 접속 대기·준비 화면의 안내 한 줄. 예: "포트 5000 에서 대기 중" */
    ui_fac_t fac[FAC_COUNT];
} ui_view_t;

typedef enum {
    UI_OUT = 0,     /* 명령 출력 */
    UI_ERR,         /* 오류 */
    UI_OK,          /* 성공 */
    UI_SYS          /* 게임이 알리는 말 */
} ui_kind_t;

typedef enum {
    UI_IN_NONE = 0, /* 읽을 입력이 더 없다 */
    UI_IN_LINE,     /* line 에 한 줄이 들어 있다 */
    UI_IN_QUIT      /* 빈 입력줄에서 Ctrl-D */
} ui_input_t;

/* ncurses 시작. 표준 입출력이 터미널이 아니면 -1. */
int  ui_init(role_t role);

/* 터미널을 원래대로. 여러 번 불러도 된다 (시그널로 끝날 때도 부른다). */
void ui_shutdown(void);

/* 그릴 내용을 바꾼다 (복사해 둔다). */
void ui_update(const ui_view_t *view);

/* 명령 출력 영역에 줄을 더한다. 줄바꿈이 들어 있으면 여러 줄로 나눈다. */
void ui_print(ui_kind_t kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* 통신 영역에 메시지 하나를 더한다. from 은 말한 사람. */
void ui_chat(role_t from, const char *text);

/* 명령 출력 영역을 비운다. */
void ui_clear(void);

/* 한 프레임 그리기. 터미널 크기가 바뀌면 여기서 따라간다. */
void ui_draw(void);

/*
 * 키 입력 처리 (막히지 않는다). Enter 로 한 줄이 완성되면 line 에 복사하고 UI_IN_LINE.
 * 입력한 줄은 화면이 명령 출력 영역에 따라 적는다.
 *   ←→ Home End 이동, ↑↓ 기록, PgUp PgDn 출력 넘기기, Ctrl-U 줄 지우기, Ctrl-L 다시 그리기
 */
ui_input_t ui_handle_input(char *line, size_t cap);

#endif /* UI_H */
