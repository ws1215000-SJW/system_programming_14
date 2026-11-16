/* timeutil.c - 단조 시계 도우미 구현 */
#include "timeutil.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int ms)
{
    struct timespec req, rem;
    if (ms <= 0)
        return;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    /* 시그널(SIGCHLD 등)에 끊기면 남은 시간만큼 이어서 잔다 */
    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
        req = rem;
}

void format_mmss(int64_t ms, char *out, size_t n)
{
    int64_t s;
    if (ms < 0)
        ms = 0;
    s = (ms + 999) / 1000;   /* 올림: 0.4초 남아도 00:01 */
    snprintf(out, n, "%02lld:%02lld", (long long)(s / 60), (long long)(s % 60));
}
