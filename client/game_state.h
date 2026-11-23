
#ifndef GAME_STATE_H
#define GAME_STATE_H

#include <stdint.h>

#define FACILITY_COUNT 3
#define MAX_FIELDS 16

// 시설별 상태 항목 하나
typedef struct {
    char key[32];
    char value[64];
} StateField;

// 시설 하나의 상태
typedef struct {
    char name[16];
    StateField fields[MAX_FIELDS];
    int field_count;
} FacilityState;

// 관제사가 기억하는 전체 게임 상태
typedef struct {
    FacilityState facilities[FACILITY_COUNT];

    int event_active;
    char event_id[16];
    char event_facility[16];
    int repair_limit;
    int64_t event_deadline_ms;   // EVENT_START 수신 시각 + 제한시간 (단조 시계)

    char last_event_result[32];
    char game_result[32];
} GameState;

// 초기화
void game_state_init(GameState *state);

// 수신 메시지를 분석하여 상태 갱신
// 1: 처리 성공, 0: 대상 아님, -1: 잘못된 형식
int game_state_apply(
    GameState *state,
    const char *message
);

// 진행 중인 이벤트의 남은 시간(ms). 이벤트가 없거나 지났으면 0
int64_t game_state_time_left_ms(const GameState *state);

// 현재 상태를 CLI에 출력
void game_state_print(const GameState *state);

#endif
