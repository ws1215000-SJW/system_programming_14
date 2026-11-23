
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include "ui_adapter.h"

// ==========================================
// 1. 시설 상태 항목 검색
// ==========================================
static const char *find_field(
    const FacilityState *facility,
    const char *key
) {
    for (int i = 0; i < facility->field_count; i++) {
        if (strcmp(facility->fields[i].key, key) == 0) {
            return facility->fields[i].value;
        }
    }

    return NULL;
}

// ==========================================
// 2. 정수형 데이터 변환
// ==========================================
static int parse_integer(
    const char *text,
    int *result
) {
    if (text == NULL || *text == '\0')
        return -1;

    char *end;
    errno = 0;

    long value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value < 0 ||
        value > 100) {
        return -1;
    }

    *result = (int)value;
    return 0;
}

// ==========================================
// 3. 실수형 계측값 변환
// ==========================================
static float parse_metric(const char *text) {
    if (text == NULL || *text == '\0')
        return NAN;

    char *end;
    errno = 0;

    float value = strtof(text, &end);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        !isfinite(value)) {
        return NAN;
    }

    return value;
}

// ==========================================
// 4. 시설별 UI 데이터 변환
// ==========================================
static void convert_facility(
    const FacilityState *facility,
    ui_fac_t *output
) {
    const char *progress;

    // 복구율은 recovery 또는 progress 지원
    progress = find_field(facility, "recovery");

    if (progress == NULL)
        progress = find_field(facility, "progress");

    int parsed_progress = 0;

    if (parse_integer(
            progress,
            &parsed_progress) == 0) {
        output->progress = parsed_progress;
    }

    // 계측값은 서버가 보내는 metric0~metric3
    // 또는 metric_0~metric_3 형식을 지원
    for (int i = 0; i < METRIC_COUNT; i++) {
        char key[32];

        snprintf(key, sizeof(key), "metric%d", i);

        const char *value = find_field(
            facility, key
        );

        if (value == NULL) {
            snprintf(
                key, sizeof(key), "metric_%d", i
            );

            value = find_field(facility, key);
        }

        output->metrics[i] = parse_metric(value);
    }
}

// ==========================================
// 5. 현재 archive 경로 변환
// ==========================================
static void convert_archive_path(
    const ArchiveContext *archive,
    char *output,
    size_t output_size
) {
    if (archive == NULL) {
        snprintf(output, output_size, "archive");
        return;
    }

    size_t root_len = strlen(archive->root);

    if (strncmp(
            archive->root,
            archive->current_path,
            root_len
        ) != 0) {
        snprintf(output, output_size, "archive");
        return;
    }

    const char *relative =
        archive->current_path + root_len;

    if (*relative != '\0' && *relative != '/') {
        snprintf(output, output_size, "archive");
        return;
    }

    snprintf(
        output,
        output_size,
        "archive%s",
        relative
    );
}

// ==========================================
// 6. GameState -> ui_view_t
// ==========================================
void ui_adapter_build(
    const GameState *state,
    const ArchiveContext *archive,
    int peer_connected,
    ui_view_t *view
) {
    if (state == NULL || view == NULL)
        return;

    // UI 구조체 초기화
    memset(view, 0, sizeof(*view));

    view->peer_connected = peer_connected;
    view->location = -1;
    view->event = EV_NONE;

    // 기본 화면 상태
    view->game_state =
        peer_connected ? GS_NORMAL : GS_WAITING;

    // 시설별 기본 계측값 초기화
    for (int i = 0; i < FAC_COUNT; i++) {
        for (int j = 0; j < METRIC_COUNT; j++) {
            view->fac[i].metrics[j] = NAN;
        }
    }

    // 시설 상태 변환
    for (int i = 0; i < FAC_COUNT; i++) {

        int found = -1;

        for (int j = 0; j < FACILITY_COUNT; j++) {
            if (strcmp(
                    state->facilities[j].name,
                    facility_key(i)
                ) == 0) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            convert_facility(
                &state->facilities[found],
                &view->fac[i]
            );
        }
    }

    // 위험 이벤트 진행 중
    if (state->event_active) {

        int event = event_from_code(
            state->event_id
        );

        if (event != EV_NONE) {
            view->event = event;
            view->game_state = GS_EVENT_ACTIVE;

            int facility = event_facility(event);

            if (facility >= 0 &&
                facility < FAC_COUNT) {
                view->fac[facility].fault = 1;
            }
        }

        view->limit_left_ms =
            game_state_time_left_ms(state);
    }

    // 이벤트 수리 성공
    else if (
        strcmp(
            state->last_event_result,
            "SUCCESS"
        ) == 0
    ) {
        view->game_state = GS_REPAIR_SUCCESS;
    }

    // 게임 종료
    if (strcmp(
            state->game_result,
            "CLEAR"
        ) == 0) {
        view->game_state = GS_GAME_CLEAR;
        view->event = EV_NONE;
    }
    else if (
        strcmp(
            state->game_result,
            "OVER"
        ) == 0 ||
        strcmp(
            state->game_result,
            "FAIL"
        ) == 0
    ) {
        view->game_state = GS_GAME_OVER;
        view->event = EV_NONE;
    }

    // 현재 파일 탐색 경로
    convert_archive_path(
        archive,
        view->prompt,
        sizeof(view->prompt)
    );

    // 접속 및 게임 안내
    if (!peer_connected) {
        snprintf(
            view->notice,
            sizeof(view->notice),
            "현장 직원 서버와 연결 대기 중"
        );
    }
    else if (view->game_state == GS_NORMAL) {
        snprintf(
            view->notice,
            sizeof(view->notice),
            "관제 시스템 정상 가동 중"
        );
    }
}
