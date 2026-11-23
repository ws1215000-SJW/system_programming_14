
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_state.h"

// ==========================================
// 1. 문자열 복사 보조 함수
// ==========================================
static void copy_text(
    char *dest,
    size_t size,
    const char *src
) {
    snprintf(dest, size, "%s", src);
}

// ==========================================
// 2. 게임 상태 초기화
// ==========================================
void game_state_init(GameState *state) {
    memset(state, 0, sizeof(*state));

    copy_text(
        state->facilities[0].name, 16, "generator"
    );
    copy_text(
        state->facilities[1].name, 16, "cooling"
    );
    copy_text(
        state->facilities[2].name, 16, "power"
    );
}

// ==========================================
// 3. 시설 이름으로 시설 찾기
// ==========================================
static FacilityState *find_facility(
    GameState *state,
    const char *name
) {
    for (int i = 0; i < FACILITY_COUNT; i++) {
        if (strcmp(state->facilities[i].name, name) == 0)
            return &state->facilities[i];
    }

    return NULL;
}

// ==========================================
// 4. 시설 상태 값 갱신
// ==========================================
static int update_field(
    FacilityState *facility,
    const char *key,
    const char *value
) {
    if (strlen(key) >= sizeof(facility->fields[0].key) ||
        strlen(value) >= sizeof(facility->fields[0].value)) {
        return -1;
    }

    // 기존 항목이 있다면 값 변경
    for (int i = 0; i < facility->field_count; i++) {
        if (strcmp(facility->fields[i].key, key) == 0) {
            copy_text(
                facility->fields[i].value,
                sizeof(facility->fields[i].value),
                value
            );
            return 0;
        }
    }

    // 새 항목 추가
    if (facility->field_count >= MAX_FIELDS)
        return -1;

    StateField *field =
        &facility->fields[facility->field_count++];

    copy_text(field->key, sizeof(field->key), key);
    copy_text(field->value, sizeof(field->value), value);

    return 0;
}

// ==========================================
// 5. 서버 메시지 분석 및 상태 갱신
// ==========================================
int game_state_apply(
    GameState *state,
    const char *message
) {
    if (state == NULL || message == NULL)
        return -1;

    if (strlen(message) > 4096)
        return -1;

    char buffer[4097];
    copy_text(buffer, sizeof(buffer), message);

    char *saveptr = NULL;
    char *type = strtok_r(buffer, "|", &saveptr);

    if (type == NULL)
        return -1;

    // --------------------------------------
    // STATE|시설|key=value|key=value...
    // --------------------------------------
    if (strcmp(type, "STATE") == 0) {
        char *name = strtok_r(NULL, "|", &saveptr);

        if (name == NULL)
            return -1;

        FacilityState *facility =
            find_facility(state, name);

        if (facility == NULL)
            return -1;

        int count = 0;
        char *item;

        while ((item = strtok_r(
                    NULL, "|", &saveptr)) != NULL) {

            char *equal = strchr(item, '=');

            if (equal == NULL)
                return -1;

            *equal = '\0';

            if (*item == '\0' || *(equal + 1) == '\0')
                return -1;

            if (update_field(
                    facility, item, equal + 1) < 0)
                return -1;

            count++;
        }

        return count > 0 ? 1 : -1;
    }

    // --------------------------------------
    // EVENT_START|이벤트ID|시설|제한시간
    // --------------------------------------
    if (strcmp(type, "EVENT_START") == 0) {
        char *id = strtok_r(NULL, "|", &saveptr);
        char *facility = strtok_r(NULL, "|", &saveptr);
        char *seconds = strtok_r(NULL, "|", &saveptr);

        if (!id || !facility || !seconds)
            return -1;

        if (!find_facility(state, facility))
            return -1;

        char *end;
        long limit = strtol(seconds, &end, 10);

        if (*seconds == '\0' || *end != '\0' ||
            limit < 0 || limit > 86400)
            return -1;

        if (strlen(id) >= sizeof(state->event_id))
            return -1;

        state->event_active = 1;
        copy_text(
            state->event_id,
            sizeof(state->event_id), id
        );
        copy_text(
            state->event_facility,
            sizeof(state->event_facility), facility
        );
        state->repair_limit = (int)limit;

        return 1;
    }

    // --------------------------------------
    // EVENT_END|이벤트ID|결과
    // --------------------------------------
    if (strcmp(type, "EVENT_END") == 0) {
        char *id = strtok_r(NULL, "|", &saveptr);
        char *result = strtok_r(NULL, "|", &saveptr);

        if (!id || !result)
            return -1;

        if (!state->event_active ||
            strcmp(id, state->event_id) != 0)
            return -1;

        if (strlen(result) >=
            sizeof(state->last_event_result))
            return -1;

        state->event_active = 0;

        copy_text(
            state->last_event_result,
            sizeof(state->last_event_result), result
        );

        return 1;
    }

    // --------------------------------------
    // GAME_END|CLEAR 또는 GAME_END|OVER
    // --------------------------------------
    if (strcmp(type, "GAME_END") == 0) {
        char *result = strtok_r(NULL, "|", &saveptr);

        if (!result ||
            strlen(result) >= sizeof(state->game_result))
            return -1;

        state->event_active = 0;

        copy_text(
            state->game_result,
            sizeof(state->game_result), result
        );

        return 1;
    }

    // CHAT, HELLO, READY 등은 다른 곳에서 처리
    return 0;
}

// ==========================================
// 6. 현재 게임 상태 출력
// ==========================================
void game_state_print(const GameState *state) {
    printf("\n=== CONTROL CENTER STATUS ===\n");

    for (int i = 0; i < FACILITY_COUNT; i++) {
        const FacilityState *facility =
            &state->facilities[i];

        printf("\n[%s]\n", facility->name);

        if (facility->field_count == 0) {
            printf("  아직 수신한 정보 없음\n");
            continue;
        }

        for (int j = 0; j < facility->field_count; j++) {
            printf(
                "  %s: %s\n",
                facility->fields[j].key,
                facility->fields[j].value
            );
        }
    }

    printf("\n[Active Event]\n");

    if (state->event_active) {
        printf("  Event ID: %s\n", state->event_id);
        printf("  Facility: %s\n", state->event_facility);
        printf("  Repair limit: %d seconds\n",
               state->repair_limit);
    }
    else {
        printf("  현재 진행 중인 이벤트 없음\n");
    }

    if (state->last_event_result[0] != '\0') {
        printf("\nLast event result: %s\n",
               state->last_event_result);
    }

    if (state->game_result[0] != '\0') {
        printf("Game result: %s\n",
               state->game_result);
    }

    printf("\n");
}
