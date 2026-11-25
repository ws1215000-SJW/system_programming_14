# 발전소 복구 시뮬레이션 - 시설 프로세스와 화면
#   make        : equip(설비 프로세스), worker(직원 콘솔)
#   make clean
CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -Wno-format-truncation -O2 -g -Isrc
BUILD   := build

# 호스트·클라이언트가 링크할 묶음
FAC_NAMES := facility equipment repair_check kvfile fac_ipc fac_mgr timeutil
UI_NAMES  := ui timeutil
FAC_OBJS  := $(FAC_NAMES:%=$(BUILD)/%.o)
UI_OBJS   := $(UI_NAMES:%=$(BUILD)/%.o)
UI_LIBS   := -lncursesw

HEADERS   := $(wildcard src/*.h)

.PHONY: all clean
all: equip worker

equip: $(BUILD)/equip_main.o $(BUILD)/equipment.o
	$(CC) $(CFLAGS) -o $@ $^

worker: $(BUILD)/worker.o $(BUILD)/host_net.o $(sort $(FAC_OBJS) $(UI_OBJS)) | equip
	$(CC) $(CFLAGS) -o $@ $^ $(UI_LIBS) -lm

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD) equip worker
