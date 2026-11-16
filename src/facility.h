/*
 * facility.h - 시설 프로세스 (발전실, 냉각실, 배전실) 본체.
 *
 * 호스트가 시설마다 파이프 두 개를 만들고 fork() 한 뒤, 자식에서 facility_run() 을 부른다.
 * (fac_mgr.h 의 fac_mgr_spawn 이 이 일을 해 준다.)
 *
 * 시설 프로세스가 하는 일
 *   1. 백 타임을 센다 (CLOCK_MONOTONIC). FCTL_ARM 으로 시작, FCTL_PAUSE/RESUME 으로 멈췄다 잇는다.
 *   2. 백 타임이 끝나면 위험 이벤트를 일으킨다.
 *        - 프로세스 고장이 있는 이벤트면 자기 설비 프로세스를 SIGSTOP/SIGKILL 한다.
 *        - FMSG_EVENT 를 보고 파이프에 쓰고 호스트에 SIGUSR1 을 보낸다.
 *        - 파일 쪽 고장(링크, 권한, 설정값)은 호스트가 FMSG_EVENT 를 받고 넣는다.
 *   3. SIGUSR2 를 받으면 실제 파일과 설비 프로세스 상태로 수리를 확인하고
 *      FMSG_VERIFY 로 답한 뒤 SIGUSR1 을 보낸다. 통과하면 정상 상태로 돌아간다.
 *   4. FAC_STATUS_MS 마다 계측값과 상태를 FMSG_STATUS 로 보고한다.
 *   5. 설비 프로세스의 pid 와 상태를 <시설>/<설비>_status.txt 의 pid=, state= 줄에,
 *      센서 값을 사용 중인 센서 파일의 value= 줄에 적는다 (파일이 있을 때만).
 *
 * 터미널에는 아무것도 쓰지 않는다 (표준 입출력을 /dev/null 로 돌린다).
 */
#ifndef FACILITY_H
#define FACILITY_H

#include <sys/types.h>

#include "common.h"

typedef struct {
    int         fac;       /* facility_id_t */
    int         msg_fd;    /* 보고 파이프의 쓰는 쪽 */
    int         ctrl_fd;   /* 제어 파이프의 읽는 쪽 */
    const char *root;      /* plant 폴더 경로 */
    pid_t       host_pid;  /* fork 하기 전에 잰 호스트 pid. 0 이면 getppid() 를 쓴다 */
} facility_args_t;

/*
 * fork() 직후 자식에서 부른다. 끝날 때까지 돌아오지 않고, 돌아온 값을 _exit() 에 넘기면 된다.
 * 호스트에게서 물려받은 다른 fd 는 모두 닫고, 자기 프로세스 그룹으로 옮겨 터미널 시그널을 받지 않는다.
 * 호스트가 죽으면 같이 죽는다 (PR_SET_PDEATHSIG).
 */
int facility_run(const facility_args_t *args);

#endif /* FACILITY_H */
