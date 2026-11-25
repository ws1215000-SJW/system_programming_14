# 시설 프로세스와 화면

기획서의 두 부분을 담는다.

- 시설 프로세스: 백 타임, 위험 이벤트 발생 알림, 시그널을 받아 수리 확인
- ncurses 화면: 발전소 직원(호스트)과 관제사(클라이언트)

호스트 프로그램(`src/worker.c`, 시설 fork·명령 처리·파일 고장 주입)과 클라이언트 프로그램(`client/`)은 이 모듈을 가져다 쓴다. 두 프로그램 모두 `src/ui.c` 화면 위에서 돈다.

## 빌드와 실행

```sh
sudo apt install build-essential libncurses-dev
make                    # equip, worker
./worker    # 발전소 직원 콘솔 (터미널이면 ncurses 화면)
```

관제사는 같은 Linux 또는 WSL 환경에서 두 번째 터미널을 열고 저장소 루트에서 실행한다. 직원 콘솔이 먼저 실행 중이어야 한다.

```sh
gcc -Isrc client/*.c src/ui.c src/timeutil.c -lncursesw -lm -o control_client
./control_client
```

## 파일

| 파일 | 내용 | 주요 시스템 콜 |
|---|---|---|
| `src/common.h` | 시설·이벤트·게임 상태 id, 이름표, 계측값 기준 | |
| `src/fac_ipc.[ch]` | 호스트와 시설 사이의 파이프 메시지 | `read` `write` |
| `src/facility.[ch]` | 시설 프로세스 본체 | `ppoll` `sigaction` `sigprocmask` `kill` `clock_gettime` `prctl` `setpgid` |
| `src/equipment.[ch]`, `src/equip_main.c` | 설비 프로세스 (터빈 제어, 냉각 펌프, 차단기 제어) | `fork` `execv` `waitpid` `kill` |
| `src/repair_check.[ch]` | 이벤트별 수리 확인 | `lstat` `stat` `realpath` `open` `read` |
| `src/kvfile.[ch]` | `key=value` 파일 읽기와 한 줄 고치기 | `open` `read` `write` |
| `src/fac_mgr.[ch]` | 호스트가 시설 세 개를 다루는 도우미 | `pipe` `fork` `kill` `waitpid` `fcntl` |
| `src/ui.[ch]` | 두 역할의 화면 | ncursesw, `ioctl(TIOCGWINSZ)` |

## 시설 프로세스

```
 호스트 ──제어 파이프──▶ 시설 프로세스 x3 ──fork+execv──▶ 설비 프로세스 (시설마다 1개)
        ◀──보고 파이프──        │
        ◀──── SIGUSR1 ──────────┤  이벤트나 확인 결과를 파이프에 썼다
        ───── SIGUSR2 ─────────▶│  수리됐는지 확인해라
        ───── SIGCONT ──────────┼───────────────────────▶ 멈춘 설비 재개
```

1. 호스트가 `FCTL_ARM(이벤트, 백 타임)` 을 보내면 시설이 `CLOCK_MONOTONIC` 으로 백 타임을 센다. `FCTL_PAUSE` / `FCTL_RESUME` 으로 멈췄다가 남은 시간부터 잇는다.
2. 백 타임이 끝나면 시설은 자기 설비 프로세스에 프로세스 고장을 일으키고(아래 표), `FMSG_EVENT` 를 쓰고, 호스트에 `SIGUSR1` 을 보낸다.
3. 호스트가 `SIGUSR2` 를 보내면 시설은 실제 파일과 설비 프로세스 상태를 검사해 `FMSG_VERIFY` 로 답하고 `SIGUSR1` 을 보낸다. 통과하면 정상 상태로 돌아가고 다음 `FCTL_ARM` 을 기다린다.
4. 0.5초마다 계측값과 상태를 `FMSG_STATUS` 로 보고한다. 설비 프로세스 상태가 바뀌면 `FMSG_EQUIP` 으로 한 줄 설명을 보낸다.

시설 프로세스는 터미널에 아무것도 쓰지 않고, 자기 프로세스 그룹으로 옮겨 가 Ctrl-C 를 받지 않는다. 호스트가 죽으면 시설과 설비가 모두 따라 죽는다.

### 이벤트별 고장과 수리 확인

| 이벤트 | 시설이 일으키는 프로세스 고장 | 호스트가 넣어야 할 파일 고장 | 수리 확인 (`src/repair_check.c`) |
|---|---|---|---|
| G01 | | `generator/temperature_sensor_link` 를 잘못된 대상으로 | 링크가 `temp_sensor_b.dat` 를 가리키고 읽힌다 |
| G02 | 터빈 제어 정지 또는 종료 | | 터빈 제어 프로세스 가동 |
| G03 | 터빈 제어 종료 | `generator/generator_config.cfg` 권한 | 권한이 `0640`, 터빈 제어 프로세스 가동 |
| C01 | | `cooling/coolant_config.cfg` 의 `target_flow` | `target_flow` 가 70 |
| C02 | 냉각 펌프 정지 또는 종료 | | 냉각 펌프 프로세스 가동 |
| C03 | | `cooling/cooling_sensor_link` 를 옛 센서로 | 링크가 `cooling_sensor_b.dat` 를 가리키고 수정 시각이 5초 이내 |
| P01 | | `power/power_config.cfg` 의 `circuit_a_load`, `circuit_b_load` | 둘 다 80 이하, 합 130 |
| P02 | 차단기 제어 정지 | `power_config.cfg` 의 `fault_isolated` | `fault_isolated=1`, 차단기 제어 프로세스 가동 |
| P03 | | `power_config.cfg` 의 `supply_generator`, `supply_cooling` | 40 과 60, 합 100 |

- 키 이름과 정답 값은 저장소의 `plant/` 파일과 `src/worker.c` 가 넣는 고장 값에 맞춰져 있다. 바꿀 때는 `src/repair_check.c` 맨 위의 표도 같이 고친다.
- 냉각실의 유량 수치는 `coolant_config.cfg` 의 `target_flow` 를 따라간다. 직원이 `repair coolant_flow <값>` 으로 바꾸면 관제사 화면의 유량·온도·효율이 같이 움직인다.
- `generator_config.cfg` 에 소유자 읽기 권한이 없으면 터빈 제어 프로세스 재실행이 거부된다. G03 에서는 권한을 먼저 고쳐야 한다.
- 링크는 상대 경로든 절대 경로든 같은 파일을 가리키면 통과한다.

### 시설이 고치는 현장 파일

파일이 있을 때만, 해당 줄만 바꾼다. 다른 줄과 권한은 그대로 둔다.

| 파일 | 줄 | 때 |
|---|---|---|
| `generator/turbine_status.txt`, `cooling/pump_status.txt`, `power/breaker_status.txt` | `pid=`, `state=` (`RUNNING` / `STOPPED` / `DEAD`) | 설비 프로세스 상태가 바뀔 때 |
| `generator/temp_sensor_b.dat`, `cooling/cooling_sensor_b.dat` | `value=` | 1초마다 |

### 호스트에 붙이는 법

```c
#include "fac_mgr.h"

static volatile sig_atomic_t notified;
static void on_usr1(int sig) { (void)sig; notified = 1; }   /* select() 를 깨우는 역할 */

fac_mgr_t m;
sigaction(SIGUSR1, ...on_usr1...);
fac_mgr_init(&m, "plant");
fac_mgr_spawn_all(&m);                   /* ncurses 를 켜기 전에 */
fac_mgr_arm(&m, FAC_COOLING, EV_C01, BACKTIME_COOLING_MS, 0);
fac_mgr_arm(&m, FAC_POWER,   EV_P01, BACKTIME_POWER_MS, 0);

/* select() 에 fac_mgr_fd(&m, 0..2) 를 넣고, 읽을 수 있으면 */
fac_msg_t msg;
while (fac_mgr_read(&m, fac, &msg) == FAC_GOT) {
    switch (msg.type) {
    case FMSG_STATUS: /* msg.metrics, msg.equip_pid, msg.equip_state 를 화면과 STATE 메시지로 */ break;
    case FMSG_EVENT:  /* fac_mgr_pause_others(&m, fac); 파일 고장 주입; 제한 시간 시작 */ break;
    case FMSG_VERIFY: /* msg.verify_ok 면 복구율 올리고 fac_mgr_resume_others, 다음 fac_mgr_arm */ break;
    case FMSG_EQUIP:  /* msg.detail 을 직원 화면에 출력 */ break;
    }
}

/* 수리 명령 */
fac_mgr_equip_signal(&m, fac, SIGCONT);  /* resume <설비> */
fac_mgr_equip_restart(&m, fac);          /* restart <설비> */
fac_mgr_request_verify(&m, fac);         /* 수리 명령 뒤에 확인 요청 (SIGUSR2) */

fac_mgr_shutdown(&m);
```

- 재실행이나 `SIGCONT` 직후에 바로 확인을 요청해도 순서가 지켜진다.
- 연계 고장은 `fac_mgr_link(&m, 영향받는_시설, 0..10)` 으로 그 시설의 수치를 나쁘게 만든다.
- 발전실 이벤트는 호스트가 냉각실·배전실 복구를 확인한 뒤 `fac_mgr_arm(&m, FAC_GENERATOR, EV_G03, ...)` 으로 시작한다. 백 타임 0 이면 바로 터진다.
## 화면

| | 발전소 직원 | 관제사 |
|---|---|---|
| 수치 | 지금 있는 시설의 계기판 | 세 시설 모두 |
| 복구율 | 보이지 않음 | 세 시설 모두 |
| 설비 프로세스 pid·상태 | 지금 있는 시설 | 보이지 않음 |
| 공통 | 머리줄(상태, 이벤트, 남은 시간, 상대 접속), 명령 출력, 통신, 입력줄 |

화면은 게임 규칙을 모른다. `ui_view_t` 를 채워 넘기면 그대로 그리고, 입력줄에서 완성된 한 줄을 돌려준다. 명령 해석과 소켓은 부르는 쪽 일이다.

```c
#include "ui.h"

ui_init(ROLE_FIELD);                     /* 관제사는 ROLE_CONTROL */
for (;;) {
    /* select(STDIN_FILENO, 소켓, 파이프..., 200ms) */
    char line[256];
    ui_input_t in;
    while ((in = ui_handle_input(line, sizeof line)) != UI_IN_NONE) {
        if (in == UI_IN_QUIT) goto done;
        /* line 을 명령으로 해석. 결과는 ui_print(UI_OUT / UI_ERR / UI_OK / UI_SYS, ...) */
        /* say 는 소켓으로 보내고 ui_chat(내 역할, 내용). 받은 CHAT 은 ui_chat(상대 역할, 내용) */
    }
    ui_view_t v = {0};
    v.game_state = GS_NORMAL;            /* GS_WAITING, GS_READY 면 가운데에 v.notice 가 뜬다 */
    v.peer_connected = 1;
    v.event = EV_NONE;                   /* 진행 중이면 이벤트 id 와 v.limit_left_ms */
    v.location = FAC_COOLING;            /* 직원이 있는 시설 */
    snprintf(v.prompt, sizeof v.prompt, "plant/cooling");
    /* v.fac[i].metrics, progress, fault, equip_pid, equip_state */
    ui_update(&v);
    ui_draw();                           /* 남은 시간이 흐르도록 주기적으로 */
}
done:
ui_shutdown();
```

- 터미널은 80x24 이상, UTF-8 로캘이어야 한다. 크기가 바뀌면 다음 `ui_draw` 에서 따라간다.
- 계측값이 NaN 이면 `--` 로 나온다 (G01 의 발전기 온도).
- 키: ←→ Home End 이동, ↑↓ 입력 기록, PgUp PgDn 출력 넘기기, Ctrl-U 줄 지우기, Ctrl-L 다시 그리기, 빈 줄에서 Ctrl-D 종료.
- Makefile 의 `FAC_OBJS`, `UI_OBJS`, `UI_LIBS` 를 호스트·클라이언트 타깃에 그대로 링크하면 된다.
