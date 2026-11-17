/* t.h - 테스트용 최소 도구 */
#ifndef T_H
#define T_H

#include <stdio.h>

static int t_fail, t_pass;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            t_pass++;                                                        \
        } else {                                                             \
            t_fail++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define SECTION(name) printf("- %s\n", name)

static inline int t_done(void)
{
    printf("%d passed, %d failed\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}

#endif /* T_H */
