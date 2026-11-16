/*
 * fac_ipc.h - 호스트 <-> 시설 프로세스 사이의 파이프 메시지와 시그널 약속.
 *
 * 파이프 두 개 (시설마다 한 쌍)
 *   보고 파이프 (시설 -> 호스트): fac_msg_t
 *   제어 파이프 (호스트 -> 시설): fac_ctrl_t
 * 둘 다 고정 크기 구조체이고 PIPE_BUF 보다 작아서 write() 한 번이 원자적이다.
 * 맨 앞 magic 이 어긋나면 읽는 쪽이 1바이트씩 버리며 다시 맞춘다.
 *
 * 시그널
 *   시설 -> 호스트  SIGUSR1  : 방금 FMSG_EVENT 또는 FMSG_VERIFY 를 파이프에 썼다
 *   호스트 -> 시설  SIGUSR2  : 지금 수리됐는지 확인해서 FMSG_VERIFY 로 답하라
 *   호스트 -> 시설  SIGTERM  : 종료
 *   호스트 -> 설비  SIGCONT  : 멈춘 설비 프로세스 재개 (pid 는 fac_msg_t.equip_pid)
 */
#ifndef FAC_IPC_H
#define FAC_IPC_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

#define FAC_MAGIC      0x504C4E54u   /* "PLNT" */
#define FAC_SIG_NOTIFY SIGUSR1
#define FAC_SIG_VERIFY SIGUSR2

#define FAC_TICK_MS     200          /* 시설 프로세스 한 바퀴 */
#define FAC_STATUS_MS   500          /* FMSG_STATUS 주기 */
#define FAC_SENSOR_MS  1000          /* 센서 파일 갱신 주기 */
#define FAC_LINK_MAX     10          /* 연계 고장 레벨 0..10 */
#define FAC_DETAIL_MAX   96

/* ---------- 보고 (시설 -> 호스트) ---------- */
typedef enum {
    FMSG_STATUS = 1,   /* 주기 보고 */
    FMSG_EVENT  = 2,   /* 백 타임이 끝나 위험 이벤트가 터졌다 (뒤이어 SIGUSR1) */
    FMSG_VERIFY = 3,   /* SIGUSR2 에 대한 답: 수리 확인 결과 (뒤이어 SIGUSR1) */
    FMSG_EQUIP  = 4    /* 설비 프로세스 상태가 바뀌었다. detail 에 한 줄 설명 */
} fac_msg_type_t;

/* 백 타임 상태 */
typedef enum {
    ARM_IDLE   = 0,    /* 백 타임 없음 */
    ARM_ARMED  = 1,    /* 줄어드는 중 */
    ARM_PAUSED = 2,    /* 멈춤 (다른 시설에서 이벤트 진행 중) */
    ARM_FIRED  = 3     /* 이벤트가 터졌고 수리를 기다린다 */
} fac_arm_state_t;

typedef struct {
    uint32_t magic;
    int32_t  type;                 /* fac_msg_type_t */
    int32_t  facility;             /* facility_id_t */
    int32_t  pid;                  /* 시설 프로세스 pid */
    int32_t  arm_state;            /* fac_arm_state_t */
    int32_t  event;                /* 예약됐거나 진행 중인 이벤트, 없으면 EV_NONE */
    int32_t  backtime_left_ms;     /* 남은 백 타임 (ARMED/PAUSED 일 때만) */
    int32_t  backtime_total_ms;
    int32_t  fault;                /* 1 = 위험 이벤트 진행 중 */
    int32_t  link_level;           /* 다른 시설 고장의 영향 0..10 */
    int32_t  equip_pid;            /* 설비 프로세스 pid, 한 번도 띄운 적 없으면 -1 */
    int32_t  equip_state;          /* equip_state_t */
    int32_t  repairs;              /* 수리 확인에 성공한 횟수 */
    int32_t  verify_ok;            /* FMSG_VERIFY: 1 성공, 0 실패 */
    int32_t  verify_failed;        /* FMSG_VERIFY: 통과하지 못한 검사 (check_kind_t), 성공이면 0 */
    float    metrics[METRIC_COUNT];/* metric_label 순서. NaN 은 수신 불가 */
    char     detail[FAC_DETAIL_MAX];/* VERIFY 실패 사유, EQUIP 설명 (UTF-8, NUL 로 끝남) */
} fac_msg_t;

/* ---------- 제어 (호스트 -> 시설) ---------- */
typedef enum {
    FCTL_ARM = 1,        /* arg1 = 이벤트, arg2 = 백 타임 ms, arg3 = 프로세스 고장 종류(0 자동, PF_STOP, PF_KILL) */
    FCTL_PAUSE,          /* 백 타임 멈춤 */
    FCTL_RESUME,         /* 남은 시간부터 다시 */
    FCTL_DISARM,         /* 백 타임 취소. 진행 중인 이벤트도 걷어 낸다 */
    FCTL_LINK,           /* arg1 = 0..10: 연계 고장으로 수치가 나빠지는 정도 */
    FCTL_EQUIP_RESTART,  /* 설비 프로세스를 새로 띄운다 (restart 명령) */
    FCTL_SHUTDOWN
} fac_ctrl_cmd_t;

typedef struct {
    uint32_t magic;
    int32_t  cmd;        /* fac_ctrl_cmd_t */
    int32_t  arg1;
    int32_t  arg2;
    int32_t  arg3;
} fac_ctrl_t;

/* ---------- 읽기 ---------- */
/* 파이프 하나에 reader 하나. 부분 수신, EAGAIN, EOF 를 처리한다. */
typedef struct {
    int     fd;
    size_t  len;
    uint8_t buf[512];
} fac_reader_t;

typedef enum {
    FAC_GOT  = 1,    /* 메시지 하나를 꺼냈다. 더 있을 수 있으니 다시 부른다 */
    FAC_NONE = 0,    /* 지금은 완전한 메시지가 없다 */
    FAC_EOF  = -1    /* 상대가 닫았거나 읽기 오류 */
} fac_read_t;

void       fac_reader_init(fac_reader_t *r, int fd);
fac_read_t fac_read_msg(fac_reader_t *r, fac_msg_t *out);
fac_read_t fac_read_ctrl(fac_reader_t *r, fac_ctrl_t *out);

/* ---------- 쓰기 ---------- */
/* 성공 0, 실패 -1 (EAGAIN 파이프 가득 참, EPIPE 상대가 사라짐). magic 은 여기서 채운다. */
int fac_write_msg(int fd, fac_msg_t *msg);
int fac_write_ctrl(int fd, fac_ctrl_cmd_t cmd, int arg1, int arg2, int arg3);

#endif /* FAC_IPC_H */
