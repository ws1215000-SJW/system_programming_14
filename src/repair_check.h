/*
 * repair_check.h - 위험 이벤트가 실제로 수리됐는지 파일과 프로세스 상태로 확인한다.
 * 이벤트마다 검사 목록이 있고 (repair_check.c 의 표), 모두 통과해야 수리 성공이다.
 * 시설 프로세스가 SIGUSR2 를 받았을 때 부른다. 터미널은 건드리지 않는다.
 */
#ifndef REPAIR_CHECK_H
#define REPAIR_CHECK_H

#include "common.h"
#include "fac_ipc.h"

typedef enum {
    CHK_NONE = 0,
    CHK_LINK,        /* path 가 심볼릭 링크이고 target 파일을 가리킨다   lstat, realpath */
    CHK_READABLE,    /* path 를 열어 1바이트 이상 읽을 수 있다           open, read */
    CHK_FRESH,       /* path 의 수정 시각이 lo 초 이내다 (갱신 중)       stat */
    CHK_MODE,        /* path 의 권한 비트가 lo 와 같다                   stat */
    CHK_CFG_RANGE,   /* path 의 key 값이 lo..hi 안에 있다 */
    CHK_CFG_SUM,     /* path 의 key + key2 가 lo..hi 안에 있다 */
    CHK_EQUIP,       /* 설비 프로세스가 가동 중이다                      waitpid, kill */
    CHK_NO_EVENT     /* 결과 전용: 확인할 이벤트가 없다 */
} check_kind_t;

typedef struct {
    check_kind_t kind;
    const char  *path;     /* plant 루트 기준 상대 경로 */
    const char  *target;   /* CHK_LINK: 가리켜야 할 파일 (루트 기준 상대 경로) */
    const char  *key;
    const char  *key2;
    double       lo, hi;
} repair_check_t;

typedef struct {
    int          ok;
    check_kind_t failed;                   /* 처음 통과하지 못한 검사, 성공이면 CHK_NONE */
    char         detail[FAC_DETAIL_MAX];   /* 실패 사유 (정답은 담지 않는다) */
} repair_result_t;

/* 이벤트의 검사 목록. count 에 개수를 준다. */
const repair_check_t *repair_checks(int event, int *count);

/*
 * root(plant 폴더) 아래의 실제 파일과 설비 상태로 event 의 검사를 차례로 돌린다.
 * equip_state 는 방금 waitpid 로 확인한 값이어야 한다.
 */
void repair_verify(const char *root, int event, int equip_state, repair_result_t *out);

/*
 * 시설이 주기적으로 값을 써 넣는 센서 파일 (루트 기준 상대 경로). 센서가 없는 시설은 NULL.
 * 센서 링크가 가리켜야 하는 파일과 같다.
 */
const char *repair_live_sensor(int fac);

/*
 * 냉각수 목표 유량 (cooling/coolant_config.cfg 의 target_flow, 0..100).
 * 파일이나 키가 없으면 정상값 70. 냉각실의 유량 수치가 이 값을 따라간다.
 */
double repair_coolant_flow(const char *root);

/* 설비 프로세스를 띄우기 전에 필요한 조건. 못 띄우면 0 이고 why 에 사유를 쓴다. */
int repair_equip_startable(const char *root, int fac, char *why, size_t n);

#endif /* REPAIR_CHECK_H */
