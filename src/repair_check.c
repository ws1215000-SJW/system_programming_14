/* repair_check.c - 이벤트별 수리 확인 */
#define _GNU_SOURCE
#include "repair_check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "kvfile.h"

/*
 * 이벤트별 검사 표.
 * 파일 이름은 기획서 5.1 을 따랐고, 키 이름과 정답 값은 현장 파일 내용과 맞춰야 한다.
 * 현장 파일을 바꾸면 여기와 tools/fixture.c 만 같이 고치면 된다.
 */
#define GEN_CFG   "generator/generator_config.cfg"
#define GEN_LINK  "generator/temperature_sensor_link"
#define GEN_LIVE  "generator/temp_sensor_b.dat"
#define COOL_CFG  "cooling/coolant_config.cfg"
#define COOL_LINK "cooling/cooling_sensor_link"
#define COOL_LIVE "cooling/cooling_sensor_b.dat"
#define PWR_CFG   "power/power_config.cfg"

#define GEN_CFG_MODE 0640

static const repair_check_t CHK_G01[] = {
    { CHK_LINK,     GEN_LINK, GEN_LIVE, NULL, NULL, 0, 0 },
    { CHK_READABLE, GEN_LINK, NULL, NULL, NULL, 0, 0 },
};
static const repair_check_t CHK_G02[] = {
    { CHK_EQUIP, NULL, NULL, NULL, NULL, 0, 0 },
};
static const repair_check_t CHK_G03[] = {
    { CHK_MODE,  GEN_CFG, NULL, NULL, NULL, GEN_CFG_MODE, 0 },
    { CHK_EQUIP, NULL, NULL, NULL, NULL, 0, 0 },
};
static const repair_check_t CHK_C01[] = {
    { CHK_CFG_RANGE, COOL_CFG, NULL, "flow_rate", NULL, 110, 130 },
};
static const repair_check_t CHK_C02[] = {
    { CHK_EQUIP, NULL, NULL, NULL, NULL, 0, 0 },
};
static const repair_check_t CHK_C03[] = {
    { CHK_LINK,  COOL_LINK, COOL_LIVE, NULL, NULL, 0, 0 },
    { CHK_FRESH, COOL_LINK, NULL, NULL, NULL, 5, 0 },
};
static const repair_check_t CHK_P01[] = {
    { CHK_CFG_RANGE, PWR_CFG, NULL, "circuit_a_load", NULL, 0, 80 },
    { CHK_CFG_RANGE, PWR_CFG, NULL, "circuit_b_load", NULL, 0, 80 },
    { CHK_CFG_SUM,   PWR_CFG, NULL, "circuit_a_load", "circuit_b_load", 130, 130 },
};
static const repair_check_t CHK_P02[] = {
    { CHK_CFG_RANGE, PWR_CFG, NULL, "fault_isolated", NULL, 1, 1 },
    { CHK_EQUIP,     NULL, NULL, NULL, NULL, 0, 0 },
};
static const repair_check_t CHK_P03[] = {
    { CHK_CFG_RANGE, PWR_CFG, NULL, "supply_generator", NULL, 40, 40 },
    { CHK_CFG_RANGE, PWR_CFG, NULL, "supply_cooling", NULL, 60, 60 },
    { CHK_CFG_SUM,   PWR_CFG, NULL, "supply_generator", "supply_cooling", 100, 100 },
};

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const struct {
    const repair_check_t *list;
    int count;
} TABLE[EV_COUNT] = {
    { CHK_G01, COUNT_OF(CHK_G01) }, { CHK_G02, COUNT_OF(CHK_G02) }, { CHK_G03, COUNT_OF(CHK_G03) },
    { CHK_C01, COUNT_OF(CHK_C01) }, { CHK_C02, COUNT_OF(CHK_C02) }, { CHK_C03, COUNT_OF(CHK_C03) },
    { CHK_P01, COUNT_OF(CHK_P01) }, { CHK_P02, COUNT_OF(CHK_P02) }, { CHK_P03, COUNT_OF(CHK_P03) },
};

const repair_check_t *repair_checks(int event, int *count)
{
    if (event < 0 || event >= EV_COUNT) {
        if (count != NULL)
            *count = 0;
        return NULL;
    }
    if (count != NULL)
        *count = TABLE[event].count;
    return TABLE[event].list;
}

const char *repair_live_sensor(int fac)
{
    if (fac == FAC_GENERATOR)
        return GEN_LIVE;
    if (fac == FAC_COOLING)
        return COOL_LIVE;
    return NULL;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s != NULL ? s + 1 : path;
}

static int fail(repair_result_t *out, check_kind_t kind, const char *fmt, const char *a,
                const char *b, const char *c)
{
    out->ok = 0;
    out->failed = kind;
    snprintf(out->detail, sizeof out->detail, fmt, a, b, c);
    return 0;
}

/* 검사 하나. 통과 1, 실패 0 (out 에 사유) */
static int run_check(const char *root, const repair_check_t *c, int fac, int equip_state,
                     repair_result_t *out)
{
    char path[PATH_MAX];
    const char *name = "";
    struct stat st;

    if (c->path != NULL) {
        snprintf(path, sizeof path, "%s/%s", root, c->path);
        name = base_name(c->path);
    }

    switch (c->kind) {
    case CHK_LINK: {
        char want[PATH_MAX], got_real[PATH_MAX], want_real[PATH_MAX];

        if (lstat(path, &st) != 0 || !S_ISLNK(st.st_mode))
            return fail(out, c->kind, "%s: 심볼릭 링크가 아니다", name, "", "");
        if (realpath(path, got_real) == NULL)
            return fail(out, c->kind, "%s: 링크 대상이 없다", name, "", "");
        snprintf(want, sizeof want, "%s/%s", root, c->target);
        if (realpath(want, want_real) == NULL || strcmp(got_real, want_real) != 0)
            return fail(out, c->kind, "%s: 다른 파일을 가리킨다", name, "", "");
        return 1;
    }
    case CHK_READABLE: {
        char byte;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = -1;

        if (fd >= 0) {
            do {
                n = read(fd, &byte, 1);
            } while (n < 0 && errno == EINTR);
            close(fd);
        }
        if (n < 1)
            return fail(out, c->kind, "%s: 데이터를 읽을 수 없다", name, "", "");
        return 1;
    }
    case CHK_FRESH: {
        struct timespec now;

        clock_gettime(CLOCK_REALTIME, &now);
        if (stat(path, &st) != 0 || now.tv_sec - st.st_mtim.tv_sec > (time_t)c->lo)
            return fail(out, c->kind, "%s: 데이터가 갱신되지 않는다", name, "", "");
        return 1;
    }
    case CHK_MODE:
        if (stat(path, &st) != 0 || (st.st_mode & 07777) != (mode_t)c->lo)
            return fail(out, c->kind, "%s: 접근 권한이 맞지 않다", name, "", "");
        return 1;
    case CHK_CFG_RANGE: {
        double v;

        if (kv_get_double(path, c->key, &v) != 0)
            return fail(out, c->kind, "%s: %s 값이 없다", name, c->key, "");
        if (v < c->lo || v > c->hi)
            return fail(out, c->kind, "%s: %s 값이 정상 범위 밖이다", name, c->key, "");
        return 1;
    }
    case CHK_CFG_SUM: {
        double a, b;

        if (kv_get_double(path, c->key, &a) != 0 || kv_get_double(path, c->key2, &b) != 0 ||
            a + b < c->lo || a + b > c->hi)
            return fail(out, c->kind, "%s: %s + %s 합이 맞지 않다", name, c->key, c->key2);
        return 1;
    }
    case CHK_EQUIP:
        if (equip_state != EQ_RUNNING)
            return fail(out, c->kind, "%s 프로세스가 가동 중이 아니다", equip_name(fac), "", "");
        return 1;
    default:
        return 1;
    }
}

void repair_verify(const char *root, int event, int equip_state, repair_result_t *out)
{
    const repair_check_t *list;
    int count, i;

    memset(out, 0, sizeof *out);
    list = repair_checks(event, &count);
    if (list == NULL) {
        fail(out, CHK_NO_EVENT, "진행 중인 위험 이벤트가 없다%s%s%s", "", "", "");
        return;
    }
    for (i = 0; i < count; i++)
        if (!run_check(root, &list[i], event_facility(event), equip_state, out))
            return;
    out->ok = 1;
    out->failed = CHK_NONE;
}

int repair_equip_startable(const char *root, int fac, char *why, size_t n)
{
    char path[PATH_MAX];
    struct stat st;

    if (fac != FAC_GENERATOR)
        return 1;
    /*
     * 터빈 제어는 발전기 설정 파일을 읽어야 뜬다. root 로 돌리면 open() 이 권한을 무시하므로
     * 권한 비트를 직접 본다. 파일이 아예 없으면 막지 않는다.
     */
    snprintf(path, sizeof path, "%s/%s", root, GEN_CFG);
    if (stat(path, &st) == 0 && (st.st_mode & S_IRUSR) == 0) {
        if (why != NULL)
            snprintf(why, n, "%s 를 읽을 수 없어 %s 시작 실패",
                     base_name(GEN_CFG), equip_name(fac));
        return 0;
    }
    return 1;
}
