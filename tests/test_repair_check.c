/* test_repair_check.c - 키=값 파일과 이벤트별 수리 확인 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "fixture.h"
#include "kvfile.h"
#include "repair_check.h"
#include "t.h"

static char root[PATH_MAX];

/* 루트 아래 경로. 테스트라 해제하지 않는다 */
static const char *P(const char *rel)
{
    char *p = malloc(PATH_MAX);

    snprintf(p, PATH_MAX, "%s/%s", root, rel);
    return p;
}

static void slurp(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY);
    ssize_t len = fd >= 0 ? read(fd, out, n - 1) : -1;

    if (fd >= 0)
        close(fd);
    out[len > 0 ? len : 0] = '\0';
}

static repair_result_t verify(int ev, int eq)
{
    repair_result_t r;

    repair_verify(root, ev, eq, &r);
    return r;
}

static void test_kvfile(void)
{
    const char *cfg = P("power/power_config.cfg");
    char val[64], text[1024];
    double d;

    SECTION("kvfile");
    CHECK(kv_get(cfg, "circuit_a_load", val, sizeof val) == 0 && strcmp(val, "65") == 0);
    CHECK(kv_get_double(cfg, "supply_cooling", &d) == 0 && d == 60.0);
    CHECK(kv_get(cfg, "no_such_key", val, sizeof val) == -1);
    CHECK(kv_get(P("power/none.cfg"), "a", val, sizeof val) == -1);

    CHECK(kv_set(cfg, "circuit_a_load", "70") == 0);
    CHECK(kv_get_double(cfg, "circuit_a_load", &d) == 0 && d == 70.0);
    slurp(cfg, text, sizeof text);
    CHECK(strstr(text, "# 회로별 전력 공급 설정\n") == text);       /* 주석과 순서가 그대로 */
    CHECK(strstr(text, "circuit_b_load=65\n") != NULL);
    CHECK(strstr(text, "circuit_a_load=65") == NULL);

    CHECK(kv_set(cfg, "new_key", "abc") == 0);                       /* 없는 키는 끝에 붙는다 */
    CHECK(kv_get(cfg, "new_key", val, sizeof val) == 0 && strcmp(val, "abc") == 0);
    CHECK(kv_set(P("power/none.cfg"), "a", "1") == -1);              /* 파일을 만들지 않는다 */
    CHECK(access(P("power/none.cfg"), F_OK) != 0);

    /* 공백이 낀 줄과 숫자가 아닌 값 */
    {
        int fd = open(P("power/odd.cfg"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const char *s = "  spaced  =  12.5  \nword=abc\nhalf=3x\nlast=7";
        CHECK(fd >= 0 && write(fd, s, strlen(s)) == (ssize_t)strlen(s));
        close(fd);
    }
    CHECK(kv_get_double(P("power/odd.cfg"), "spaced", &d) == 0 && d == 12.5);
    CHECK(kv_get_double(P("power/odd.cfg"), "word", &d) == -1);
    CHECK(kv_get_double(P("power/odd.cfg"), "half", &d) == -1);
    CHECK(kv_get_double(P("power/odd.cfg"), "last", &d) == 0 && d == 7.0);
    CHECK(kv_set(cfg, "circuit_a_load", "65") == 0);
}

static void test_healthy(void)
{
    int ev;

    SECTION("정상 상태에서는 모든 이벤트가 통과");
    for (ev = 0; ev < EV_COUNT; ev++) {
        repair_result_t r = verify(ev, EQ_RUNNING);
        if (!r.ok) printf("  %s: %s\n", event_code(ev), r.detail);
        CHECK(r.ok == 1 && r.failed == CHK_NONE && r.detail[0] == 0);
    }
    CHECK(verify(EV_NONE, EQ_RUNNING).failed == CHK_NO_EVENT);
    CHECK(verify(EV_COUNT, EQ_RUNNING).ok == 0);
}

static void test_break_fix(void)
{
    static const struct { int ev; check_kind_t kind; } cases[] = {
        { EV_G01, CHK_LINK }, { EV_G03, CHK_MODE }, { EV_C01, CHK_CFG_RANGE },
        { EV_C03, CHK_LINK }, { EV_P01, CHK_CFG_RANGE }, { EV_P02, CHK_CFG_RANGE },
        { EV_P03, CHK_CFG_RANGE },
    };
    size_t i;

    SECTION("파일 고장을 넣으면 실패, 고치면 통과");
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        repair_result_t r;

        CHECK(fixture_break(root, cases[i].ev) == 0);
        r = verify(cases[i].ev, EQ_RUNNING);
        CHECK(r.ok == 0 && r.failed == cases[i].kind && r.detail[0] != '\0');
        CHECK(fixture_fix(root, cases[i].ev) == 0);
        CHECK(verify(cases[i].ev, EQ_RUNNING).ok == 1);
    }
}

static void test_equip(void)
{
    static const int evs[] = { EV_G02, EV_G03, EV_C02, EV_P02 };
    size_t i;

    SECTION("설비 프로세스가 돌지 않으면 실패");
    for (i = 0; i < sizeof evs / sizeof evs[0]; i++) {
        CHECK(verify(evs[i], EQ_STOPPED).failed == CHK_EQUIP);
        CHECK(verify(evs[i], EQ_DEAD).failed == CHK_EQUIP);
        CHECK(verify(evs[i], EQ_RUNNING).ok == 1);
    }
    /* 설비와 상관없는 이벤트는 설비 상태를 보지 않는다 */
    CHECK(verify(EV_C01, EQ_DEAD).ok == 1);
}

/* 저장소에 올라온 실제 현장 파일로도 C01 확인이 맞게 도는지 본다 */
static void test_repo_plant(void)
{
    repair_result_t r;

    SECTION("저장소의 plant/ 파일");
    if (access("plant/cooling/coolant_config.cfg", R_OK) != 0) {
        printf("  (plant/ 가 없어 건너뜀)\n");
        return;
    }
    repair_verify("plant", EV_C01, EQ_RUNNING, &r);
    CHECK(r.ok == 1);
    CHECK(repair_coolant_flow("plant") == 70.0);
    CHECK(repair_coolant_flow(root) == 70.0);
    CHECK(repair_coolant_flow("/nonexistent") == 70.0);
}

static void test_link_forms(void)
{
    const char *link = P("generator/temperature_sensor_link");
    char abs_target[PATH_MAX];
    int fd;

    SECTION("심볼릭 링크 검사");
    snprintf(abs_target, sizeof abs_target, "%s/generator/temp_sensor_b.dat", root);

    unlink(link);
    CHECK(symlink("./temp_sensor_b.dat", link) == 0);
    CHECK(verify(EV_G01, EQ_RUNNING).ok == 1);
    unlink(link);
    CHECK(symlink(abs_target, link) == 0);                 /* 절대 경로로 걸어도 같은 파일이면 통과 */
    CHECK(verify(EV_G01, EQ_RUNNING).ok == 1);

    unlink(link);
    CHECK(symlink("temp_sensor_a.dat", link) == 0);        /* 다른 센서 */
    CHECK(verify(EV_G01, EQ_RUNNING).failed == CHK_LINK);

    unlink(link);                                          /* 링크 없음 */
    CHECK(verify(EV_G01, EQ_RUNNING).failed == CHK_LINK);

    fd = open(link, O_WRONLY | O_CREAT, 0644);             /* 링크 자리에 일반 파일 */
    CHECK(fd >= 0 && write(fd, "value=1\n", 8) == 8);
    close(fd);
    CHECK(verify(EV_G01, EQ_RUNNING).failed == CHK_LINK);

    unlink(link);
    CHECK(symlink("temp_sensor_b.dat", link) == 0);
    CHECK(truncate(abs_target, 0) == 0);                   /* 대상은 맞지만 내용이 비었다 */
    CHECK(verify(EV_G01, EQ_RUNNING).failed == CHK_READABLE);
    CHECK(kv_set(abs_target, "value", "500.0") == 0);
    CHECK(verify(EV_G01, EQ_RUNNING).ok == 1);
}

static void test_fresh(void)
{
    struct timespec ts[2];

    SECTION("센서 데이터 갱신 검사");
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec -= 60;
    ts[1] = ts[0];
    CHECK(utimensat(AT_FDCWD, P("cooling/cooling_sensor_b.dat"), ts, 0) == 0);
    CHECK(verify(EV_C03, EQ_RUNNING).failed == CHK_FRESH);
    CHECK(kv_set(P("cooling/cooling_sensor_b.dat"), "value", "33.1") == 0);
    CHECK(verify(EV_C03, EQ_RUNNING).ok == 1);
}

static void test_values(void)
{
    const char *cfg = P("power/power_config.cfg");

    SECTION("설정값과 권한 경계");
    kv_set(cfg, "circuit_a_load", "80"); kv_set(cfg, "circuit_b_load", "50");
    CHECK(verify(EV_P01, EQ_RUNNING).ok == 1);
    kv_set(cfg, "circuit_a_load", "81"); kv_set(cfg, "circuit_b_load", "49");
    CHECK(verify(EV_P01, EQ_RUNNING).failed == CHK_CFG_RANGE);
    kv_set(cfg, "circuit_a_load", "60"); kv_set(cfg, "circuit_b_load", "60");
    CHECK(verify(EV_P01, EQ_RUNNING).failed == CHK_CFG_SUM);     /* 부하를 그냥 줄이면 안 된다 */
    kv_set(cfg, "circuit_a_load", "abc");
    CHECK(verify(EV_P01, EQ_RUNNING).failed == CHK_CFG_RANGE);
    kv_set(cfg, "circuit_a_load", "65"); kv_set(cfg, "circuit_b_load", "65");
    CHECK(verify(EV_P01, EQ_RUNNING).ok == 1);

    CHECK(chmod(P("generator/generator_config.cfg"), 0644) == 0);
    CHECK(verify(EV_G03, EQ_RUNNING).failed == CHK_MODE);        /* 읽을 수 있어도 모드가 다르면 실패 */
    CHECK(chmod(P("generator/generator_config.cfg"), 0640) == 0);
    CHECK(verify(EV_G03, EQ_RUNNING).ok == 1);
}

static void test_startable(void)
{
    char why[FAC_DETAIL_MAX] = "";

    SECTION("설비 시작 조건");
    CHECK(repair_equip_startable(root, FAC_GENERATOR, why, sizeof why) == 1);
    CHECK(chmod(P("generator/generator_config.cfg"), 0000) == 0);
    CHECK(repair_equip_startable(root, FAC_GENERATOR, why, sizeof why) == 0 && why[0] != '\0');
    CHECK(repair_equip_startable(root, FAC_COOLING, why, sizeof why) == 1);
    CHECK(chmod(P("generator/generator_config.cfg"), 0640) == 0);
    CHECK(unlink(P("generator/generator_config.cfg")) == 0);
    CHECK(repair_equip_startable(root, FAC_GENERATOR, why, sizeof why) == 1);
}

int main(void)
{
    snprintf(root, sizeof root, "/tmp/plant_test_check_%d", (int)getpid());
    if (fixture_create(root) != 0) {
        perror("fixture_create");
        return 1;
    }
    test_kvfile();
    test_healthy();
    test_break_fix();
    test_equip();
    test_repo_plant();
    test_link_forms();
    test_fresh();
    test_values();
    test_startable();
    fixture_remove(root);
    return t_done();
}
