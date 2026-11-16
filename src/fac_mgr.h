/*
 * fac_mgr.h - 호스트 쪽에서 시설 프로세스 세 개를 다루는 도우미.
 * pipe + fork 로 띄우고, 제어 메시지와 시그널을 보내고, 보고를 읽고, waitpid 로 회수한다.
 *
 * 호스트가 할 일
 *   - SIGUSR1 핸들러를 달고 (표시만 세운다), select() 에 fac_mgr_fd() 세 개를 넣는다.
 *   - 읽을 수 있을 때마다 fac_mgr_read() 를 FAC_GOT 가 아닐 때까지 부른다.
 *   - 시설은 ncurses 를 켜기 전에 띄우는 편이 안전하다.
 *
 *   fac_mgr_t m;
 *   fac_mgr_init(&m, "plant");
 *   fac_mgr_spawn_all(&m);
 *   fac_mgr_arm(&m, FAC_COOLING, EV_C01, BACKTIME_COOLING_MS, 0);
 *   ...  FMSG_EVENT 를 받으면 다른 시설에 fac_mgr_pause(), 파일 고장 주입, 제한 시간 시작
 *   ...  수리 명령 뒤 fac_mgr_request_verify() -> FMSG_VERIFY 의 verify_ok 로 판정
 *   fac_mgr_shutdown(&m);
 */
#ifndef FAC_MGR_H
#define FAC_MGR_H

#include <sys/types.h>

#include "common.h"
#include "fac_ipc.h"

typedef struct {
    pid_t        pid;        /* 시설 프로세스 pid, 없으면 -1 */
    int          alive;
    int          msg_fd;     /* 보고 파이프 읽는 쪽 (O_NONBLOCK), 없으면 -1 */
    int          ctrl_fd;    /* 제어 파이프 쓰는 쪽 (O_NONBLOCK), 없으면 -1 */
    fac_reader_t reader;
    fac_msg_t    last;       /* 마지막으로 읽은 보고 (종류 무관) */
    int          has_last;
} fac_handle_t;

typedef struct {
    char         root[PATH_MAX];   /* plant 폴더 (절대 경로로 바꿔 둔다) */
    fac_handle_t fac[FAC_COUNT];
} fac_mgr_t;

/*
 * 준비. plant_root 는 이미 있는 폴더여야 한다. SIGPIPE 를 무시하게 하고,
 * SIGUSR1 이 기본 동작(프로세스 종료)인 채라면 무시로 바꿔 호스트가 죽지 않게 한다.
 * 성공 0, 실패 -1.
 */
int fac_mgr_init(fac_mgr_t *m, const char *plant_root);

/* 시설 하나를 띄운다 (pipe x2 + fork, 자식은 facility_run). 이미 떠 있으면 -1. */
int fac_mgr_spawn(fac_mgr_t *m, int fac);
int fac_mgr_spawn_all(fac_mgr_t *m);

/* select()/poll() 에 넣을 보고 파이프 fd. 없으면 -1. */
int fac_mgr_fd(const fac_mgr_t *m, int fac);

/* 보고 하나 읽기 (막히지 않는다). FAC_EOF 면 그 시설의 보고 파이프를 닫는다. */
fac_read_t fac_mgr_read(fac_mgr_t *m, int fac, fac_msg_t *out);

/* ----- 제어 메시지 (성공 0, 실패 -1) ----- */
/* 백 타임 시작. variant: 0 자동, PF_STOP, PF_KILL (프로세스 고장이 둘 다 되는 이벤트에서만 의미) */
int fac_mgr_arm(fac_mgr_t *m, int fac, int event, int backtime_ms, int variant);
int fac_mgr_pause(fac_mgr_t *m, int fac);
int fac_mgr_resume(fac_mgr_t *m, int fac);
int fac_mgr_disarm(fac_mgr_t *m, int fac);
int fac_mgr_link(fac_mgr_t *m, int fac, int level);
/* fac 만 빼고 모두 멈춤/재개 (이벤트가 터진 시설을 except 로 준다, 없으면 -1) */
void fac_mgr_pause_others(fac_mgr_t *m, int except);
void fac_mgr_resume_others(fac_mgr_t *m, int except);

/* restart <설비>: 설비 프로세스를 새로 띄우게 한다 */
int fac_mgr_equip_restart(fac_mgr_t *m, int fac);

/*
 * resume <설비> 등: 설비 프로세스에 직접 시그널 (SIGCONT, SIGSTOP, SIGKILL...).
 * pid 는 마지막 보고의 equip_pid 를 쓴다. 보고를 받은 적이 없거나 이미 종료 상태면 -1 (ESRCH).
 */
int fac_mgr_equip_signal(fac_mgr_t *m, int fac, int sig);

/* 수리 확인 요청 (SIGUSR2). 답은 FMSG_VERIFY 로 온다. */
int fac_mgr_request_verify(fac_mgr_t *m, int fac);

/* 죽은 시설 프로세스를 회수한다 (waitpid WNOHANG). 새로 죽은 수를 돌려준다. */
int fac_mgr_reap(fac_mgr_t *m);

/* 모두 끝내고 회수한다. 좀비도 고아도 남기지 않는다. 여러 번 불러도 된다. */
void fac_mgr_shutdown(fac_mgr_t *m);

#endif /* FAC_MGR_H */
