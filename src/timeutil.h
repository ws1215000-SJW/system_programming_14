/*
 * timeutil.h - 단조 시계(CLOCK_MONOTONIC) 기반 시간 도우미.
 * 벽시계가 바뀌어도 타이머가 흔들리지 않게 모든 게임 타이머는 이걸 쓴다.
 */
#ifndef TIMEUTIL_H
#define TIMEUTIL_H

#include <stddef.h>
#include <stdint.h>

/* 현재 단조 시각(ms). clock_gettime(CLOCK_MONOTONIC) 사용. */
int64_t now_ms(void);

/* ms 만큼 잔다 (nanosleep, EINTR 이면 남은 시간만큼 다시 잔다). */
void sleep_ms(int ms);

/* ms 를 "MM:SS" 로 (음수는 0 으로). out 은 최소 8바이트 권장. */
void format_mmss(int64_t ms, char *out, size_t n);

#endif /* TIMEUTIL_H */
