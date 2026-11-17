/*
 * common.h - 시설 프로세스와 화면이 함께 쓰는 id, 상수, 이름표.
 * .c 파일 없이 static inline 함수만 둔다.
 */
#ifndef COMMON_H
#define COMMON_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---------- 시설 ---------- */
typedef enum {
    FAC_GENERATOR = 0,   /* 발전실 */
    FAC_COOLING   = 1,   /* 냉각실 */
    FAC_POWER     = 2,   /* 배전실 */
    FAC_COUNT     = 3
} facility_id_t;

#define METRIC_COUNT 4   /* 시설당 계측값 4개 */

/* ---------- 위험 이벤트 (기획서 4.1 의 코드 순서) ---------- */
typedef enum {
    EV_NONE = -1,
    EV_G01 = 0,   /* 발전기 온도 센서 연결 오류 */
    EV_G02,       /* 터빈 제어 프로세스 정지 */
    EV_G03,       /* 발전기 최종 재가동 실패 */
    EV_C01,       /* 냉각수 유량 저하 */
    EV_C02,       /* 냉각 펌프 프로세스 정지 */
    EV_C03,       /* 냉각 센서 데이터 경로 손상 */
    EV_P01,       /* 배전 회로 과부하 */
    EV_P02,       /* 배전 차단기 비정상 정지 */
    EV_P03,       /* 전력 부하 불균형 */
    EV_COUNT
} event_id_t;

/* ---------- 게임 상태 (기획서 6.5) ---------- */
typedef enum {
    GS_WAITING = 0,
    GS_READY,
    GS_NORMAL,
    GS_EVENT_ACTIVE,
    GS_REPAIR_SUCCESS,
    GS_GAME_OVER,
    GS_GAME_CLEAR
} game_state_t;

typedef enum {
    ROLE_FIELD   = 0,    /* 발전소 직원 (호스트) */
    ROLE_CONTROL = 1     /* 관제사 (클라이언트) */
} role_t;

/* ---------- 설비 프로세스 상태 ---------- */
typedef enum {
    EQ_RUNNING = 0,
    EQ_STOPPED,          /* SIGSTOP 으로 멈춤 */
    EQ_DEAD              /* 종료됨 (재실행 전) */
} equip_state_t;

/* 이벤트가 터질 때 설비 프로세스에 생기는 고장 */
typedef enum {
    PF_NONE = 0,         /* 파일 쪽 고장만 */
    PF_STOP,             /* SIGSTOP: resume 으로 고친다 */
    PF_KILL,             /* SIGKILL: restart 로 고친다 */
    PF_EITHER            /* 정지/종료 중 하나 (ARM 의 variant 로 고르거나 무작위) */
} proc_fault_t;

/* ---------- 시간 상수 (기획서 3.1) ---------- */
#define BACKTIME_COOLING_MS   90000
#define BACKTIME_POWER_MS    120000
#define REPAIR_LIMIT_MS      300000

/* ---------- 이름표 ---------- */
static inline const char *facility_name(int fac)
{
    static const char *const names[FAC_COUNT] = { "발전실", "냉각실", "배전실" };
    return (fac >= 0 && fac < FAC_COUNT) ? names[fac] : "?";
}

/* plant/ 아래 폴더 이름이자 go 명령의 인자 */
static inline const char *facility_key(int fac)
{
    static const char *const keys[FAC_COUNT] = { "generator", "cooling", "power" };
    return (fac >= 0 && fac < FAC_COUNT) ? keys[fac] : "?";
}

static inline int facility_from_key(const char *key)
{
    int i;
    for (i = 0; i < FAC_COUNT; i++)
        if (key != NULL && strcmp(key, facility_key(i)) == 0)
            return i;
    return -1;
}

/* 이벤트 한 번을 고칠 때 오르는 복구율 */
static inline int facility_progress_step(int fac)
{
    return fac == FAC_GENERATOR ? 100 : 50;
}

static inline const char *event_code(int ev)
{
    static const char *const codes[EV_COUNT] = {
        "G01", "G02", "G03", "C01", "C02", "C03", "P01", "P02", "P03"
    };
    return (ev >= 0 && ev < EV_COUNT) ? codes[ev] : "---";
}

static inline int event_from_code(const char *code)
{
    int i;
    for (i = 0; i < EV_COUNT; i++)
        if (code != NULL && strcmp(code, event_code(i)) == 0)
            return i;
    return EV_NONE;
}

static inline const char *event_title(int ev)
{
    static const char *const titles[EV_COUNT] = {
        "발전기 온도 센서 연결 오류", "터빈 제어 프로세스 정지", "발전기 최종 재가동 실패",
        "냉각수 유량 저하", "냉각 펌프 프로세스 정지", "냉각 센서 데이터 경로 손상",
        "배전 회로 과부하", "배전 차단기 비정상 정지", "전력 부하 불균형"
    };
    return (ev >= 0 && ev < EV_COUNT) ? titles[ev] : "";
}

static inline int event_facility(int ev)
{
    if (ev >= EV_G01 && ev <= EV_G03) return FAC_GENERATOR;
    if (ev >= EV_C01 && ev <= EV_C03) return FAC_COOLING;
    if (ev >= EV_P01 && ev <= EV_P03) return FAC_POWER;
    return -1;
}

static inline proc_fault_t event_proc_fault(int ev)
{
    switch (ev) {
    case EV_G02: case EV_C02: return PF_EITHER;
    case EV_P02:              return PF_STOP;
    case EV_G03:              return PF_KILL;
    default:                  return PF_NONE;
    }
}

/* 시설마다 설비 프로세스가 하나 있다 */
static inline const char *equip_key(int fac)
{
    static const char *const keys[FAC_COUNT] = { "turbine", "pump", "breaker" };
    return (fac >= 0 && fac < FAC_COUNT) ? keys[fac] : "?";
}

static inline const char *equip_name(int fac)
{
    static const char *const names[FAC_COUNT] = { "터빈 제어", "냉각 펌프", "차단기 제어" };
    return (fac >= 0 && fac < FAC_COUNT) ? names[fac] : "?";
}

static inline int equip_from_key(const char *key)
{
    int i;
    for (i = 0; i < FAC_COUNT; i++)
        if (key != NULL && strcmp(key, equip_key(i)) == 0)
            return i;
    return -1;
}

/* 설비 프로세스의 pid 와 상태를 적어 두는 현장 파일 (시설 폴더 안) */
static inline const char *equip_status_file(int fac)
{
    static const char *const files[FAC_COUNT] = {
        "turbine_status.txt", "pump_status.txt", "breaker_status.txt"
    };
    return (fac >= 0 && fac < FAC_COUNT) ? files[fac] : "";
}

static inline const char *equip_state_name(int st)
{
    static const char *const names[] = { "가동", "정지", "종료" };
    return (st >= 0 && st <= EQ_DEAD) ? names[st] : "?";
}

/* 상태 파일의 state= 값 */
static inline const char *equip_state_key(int st)
{
    static const char *const keys[] = { "RUNNING", "STOPPED", "DEAD" };
    return (st >= 0 && st <= EQ_DEAD) ? keys[st] : "?";
}

static inline const char *game_state_name(int st)
{
    static const char *const names[] = {
        "접속 대기", "준비 완료", "정상 가동", "위험 이벤트", "복구 완료", "게임 오버", "게임 클리어"
    };
    return (st >= 0 && st <= GS_GAME_CLEAR) ? names[st] : "?";
}

static inline const char *role_name(int role)
{
    return role == ROLE_CONTROL ? "관제사" : "발전소 직원";
}

/* ---------- 계측값 ---------- */
static inline const char *metric_label(int fac, int idx)
{
    static const char *const labels[FAC_COUNT][METRIC_COUNT] = {
        { "발전기 온도", "출력", "터빈 회전수", "증기 압력" },
        { "냉각수 온도", "냉각수 수위", "냉각수 유량", "냉각 효율" },
        { "모선 전압", "공급 전력", "주파수", "부하 균형" }
    };
    if (fac < 0 || fac >= FAC_COUNT || idx < 0 || idx >= METRIC_COUNT)
        return "?";
    return labels[fac][idx];
}

static inline const char *metric_unit(int fac, int idx)
{
    static const char *const units[FAC_COUNT][METRIC_COUNT] = {
        { "°C", "MW", "rpm", "bar" },
        { "°C", "%", "L/s", "%" },
        { "V", "kW", "Hz", "%" }
    };
    if (fac < 0 || fac >= FAC_COUNT || idx < 0 || idx >= METRIC_COUNT)
        return "";
    return units[fac][idx];
}

static inline int metric_decimals(int fac, int idx)
{
    return (fac == FAC_POWER && idx == 2) ? 2 : 0;
}

/*
 * 계측값 위험도: 0 정상, 1 주의, 2 위험. 화면이 색을 정할 때 쓴다.
 * 값이 NaN 이면 수신 불가라는 뜻이고 화면이 따로 처리한다 (여기서는 2).
 */
static inline int metric_level(int fac, int idx, float v)
{
    /* { 주의 하한, 주의 상한, 위험 하한, 위험 상한 } */
    static const float t[FAC_COUNT][METRIC_COUNT][4] = {
        { { -1e9f, 540.f, -1e9f, 600.f },
          { 440.f, 1e9f, 350.f, 1e9f },
          { 3450.f, 3750.f, 3200.f, 3900.f },
          { 140.f, 180.f, 120.f, 200.f } },
        { { -1e9f, 45.f, -1e9f, 60.f },
          { 60.f, 1e9f, 35.f, 1e9f },
          { 60.f, 1e9f, 45.f, 1e9f },             /* 냉각수 유량: 정상 70 */
          { 80.f, 1e9f, 55.f, 1e9f } },
        { { 360.f, 400.f, 330.f, 430.f },
          { 4700.f, 1e9f, 3500.f, 1e9f },
          { 59.5f, 60.5f, 58.5f, 61.5f },
          { 85.f, 1e9f, 65.f, 1e9f } }
    };
    const float *r;
    if (fac < 0 || fac >= FAC_COUNT || idx < 0 || idx >= METRIC_COUNT)
        return 0;
    if (v != v)
        return 2;
    r = t[fac][idx];
    if (v < r[2] || v > r[3]) return 2;
    if (v < r[0] || v > r[1]) return 1;
    return 0;
}

#endif /* COMMON_H */
