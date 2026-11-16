/*
 * equipment.h - 설비 프로세스 (터빈 제어, 냉각 펌프, 차단기 제어).
 * 시설 프로세스가 fork + execv 로 하나씩 띄우고 waitpid 로 상태를 따라간다.
 * 설비 프로세스는 하는 일 없이 살아 있기만 한다. 정지(SIGSTOP)·종료가 곧 고장이다.
 */
#ifndef EQUIPMENT_H
#define EQUIPMENT_H

#include <sys/types.h>

#include "common.h"

typedef struct {
    pid_t         pid;         /* 마지막으로 띄운 pid, 없으면 -1 */
    equip_state_t state;
    int           exit_code;   /* 스스로 끝났을 때의 종료 코드 */
    int           term_sig;    /* 시그널로 죽었으면 그 번호, 아니면 0 */
    int           spawns;      /* 띄운 횟수 */
} equip_t;

void equip_init(equip_t *e);

/*
 * 설비 프로세스를 띄운다. 살아 있는 것이 있으면 먼저 끝내고 회수한다.
 * 실행 파일은 환경변수 PLANT_EQUIP_BIN, 없으면 지금 실행 파일과 같은 폴더의 "equip".
 * execv 가 실패하면 자식이 equip_main 을 직접 돈다 (실행 파일이 없어도 게임은 된다).
 * 성공 0, fork 실패 -1.
 */
int equip_spawn(equip_t *e, int fac);

/* 설비 프로세스에 시그널. 띄운 적이 없거나 이미 끝났으면 -1 (ESRCH). */
int equip_signal(equip_t *e, int sig);

/*
 * waitpid(WNOHANG | WUNTRACED | WCONTINUED) 로 상태 변화를 모두 반영한다.
 * 상태가 바뀌었으면 1.
 */
int equip_reap(equip_t *e);

/* 끝내고 회수한다 (멈춰 있어도 된다). */
void equip_destroy(equip_t *e);

/* 설비 프로세스 본체. 돌아오지 않는다 (시그널로 끝난다). parent 는 띄운 시설의 pid. */
int equip_main(int fac, pid_t parent);

#endif /* EQUIPMENT_H */
