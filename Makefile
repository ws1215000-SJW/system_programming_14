# 발전소 복구 시뮬레이션 - 시설 프로세스와 화면
#   make        : equip(설비 프로세스), demo(화면 + 시설 프로세스 시연), worker(직원 콘솔)
#   make test   : tests/test_*.c 를 빌드해 실행 (ncurses 없이 돈다)
#   make clean
CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -Wno-format-truncation -O2 -g -Isrc -Itools
BUILD   := build

# 호스트·클라이언트가 링크할 묶음
FAC_NAMES := facility equipment repair_check kvfile fac_ipc fac_mgr timeutil
UI_NAMES  := ui timeutil
FAC_OBJS  := $(FAC_NAMES:%=$(BUILD)/%.o)
UI_OBJS   := $(UI_NAMES:%=$(BUILD)/%.o)
UI_LIBS   := -lncursesw

HEADERS   := $(wildcard src/*.h tools/*.h tests/*.h)
TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)

.PHONY: all test clean
all: equip demo worker

equip: $(BUILD)/equip_main.o $(BUILD)/equipment.o
	$(CC) $(CFLAGS) -o $@ $^

worker: $(BUILD)/worker.o $(FAC_OBJS) | equip
	$(CC) $(CFLAGS) -o $@ $^ -lm

demo: $(BUILD)/tools/demo.o $(BUILD)/tools/fixture.o $(sort $(FAC_OBJS) $(UI_OBJS))
	$(CC) $(CFLAGS) -o $@ $^ $(UI_LIBS) -lm

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/tools/%.o: tools/%.c $(HEADERS) | $(BUILD)/tools
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/tests/%: tests/%.c $(BUILD)/tools/fixture.o $(FAC_OBJS) $(HEADERS) | $(BUILD)/tests
	$(CC) $(CFLAGS) -Itests -o $@ $< $(BUILD)/tools/fixture.o $(FAC_OBJS) -lm

$(BUILD)/tests/test_worker_c02: src/worker.c

$(BUILD) $(BUILD)/tools $(BUILD)/tests:
	mkdir -p $@

# 설비 프로세스를 equip 실행 파일로 띄우는 경우와, 실행 파일이 없어 fork 만으로 도는 경우를 둘 다 본다
test: equip $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
	    echo "=== $$t (equip 실행 파일)"; \
	    PLANT_EQUIP_BIN=$(CURDIR)/equip $$t || fail=1; \
	done; \
	echo "=== $(BUILD)/tests/test_facility (실행 파일 없음)"; \
	PLANT_EQUIP_BIN=/nonexistent/equip $(BUILD)/tests/test_facility || fail=1; \
	if [ $$fail -ne 0 ]; then echo "SOME TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"

clean:
	rm -rf $(BUILD) equip demo worker demo_plant
