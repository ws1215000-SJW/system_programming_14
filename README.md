# system_programming_14
시프 14조 조별 과제

- [시설 프로세스와 화면](docs/facility-ui.md)

## 직원 콘솔과 단독 테스트

Linux 또는 WSL에서 `make` 후 저장소 루트에서 `./worker`를 실행합니다. 직원은
`go cooling`, `go power`, `go generator`로 이동합니다. `help`는 현재 시설의
파일과 조작 명령, `status`는 최근 계측 보고, `cat 파일명`은 현장 단서를 표시합니다.

G03은 실제 POSIX 권한 `0000`과 `0640`을 사용합니다. 원래 저장소에서
`./worker`를 실행하면 권한 변경을 확인합니다. WSL의 Windows 드라이브처럼
권한이 반영되지 않는 곳에서는 현장 데이터만 Linux 임시 폴더에 복사해 사용하고
게임 종료 시 정리합니다. 다음 실행은 새 현장으로 시작합니다.

연결 전 개발 테스트는 직원 콘솔에서 `test on`으로 켜고 `test off`로 끕니다.
`./worker --solo-test`는 시작부터 테스트 도구를 켭니다. 같은 시설 프로세스와
수리 판정을 사용하면서 `manual index`, `manual registry`, `manual history`,
`manual C01` 등으로 관제 자료를 열람할 수 있습니다. 이벤트 대기 중 `debug next`로
대기를 단축합니다. C02·G02 대기 중에는 `debug next stop` 또는 `debug next kill`로
고장 유형을 지정할 수 있습니다. 수리 제한 시간은 두 모드 모두 40초입니다.

‘추가 조작 필요’는 설정·설비를 다시 조사해야 한다는 뜻입니다. ‘회복 확인 중’이면
재입력 없이 계측 보고를 기다리세요. 원본 현장 파일을 보존하며 테스트하려면
`make test` 후 `python3 tests/play_worker.py`를 실행하세요. 플레이 테스트는
임시 디렉터리에 현장과 자료를 복사하고 실제 worker와 equip을 실행합니다.
