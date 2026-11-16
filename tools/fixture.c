/* fixture.c - 개발·테스트용 임시 현장 파일 */
#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#include "fixture.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "kvfile.h"

#define MARKER ".fixture"

static int put_file(const char *root, const char *rel, const char *text, mode_t mode)
{
    char path[PATH_MAX];
    int fd;
    size_t len = strlen(text);

    snprintf(path, sizeof path, "%s/%s", root, rel);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (write(fd, text, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    close(fd);
    return chmod(path, mode);
}

/* rel 의 수정 시각을 days 일 전으로 돌린다 (옛 센서 파일) */
static int age_file(const char *root, const char *rel, int days)
{
    char path[PATH_MAX];
    struct timespec ts[2];

    snprintf(path, sizeof path, "%s/%s", root, rel);
    clock_gettime(CLOCK_REALTIME, &ts[0]);
    ts[0].tv_sec -= (time_t)days * 86400;
    ts[1] = ts[0];
    return utimensat(AT_FDCWD, path, ts, 0);
}

/* 링크를 지우고 새 대상으로 다시 건다 */
static int relink(const char *root, const char *rel, const char *target)
{
    char path[PATH_MAX];

    snprintf(path, sizeof path, "%s/%s", root, rel);
    if (unlink(path) != 0 && errno != ENOENT)
        return -1;
    return symlink(target, path);
}

static int set_kv(const char *root, const char *rel, const char *key, const char *value)
{
    char path[PATH_MAX];

    snprintf(path, sizeof path, "%s/%s", root, rel);
    return kv_set(path, key, value);
}

static int set_mode(const char *root, const char *rel, mode_t mode)
{
    char path[PATH_MAX];

    snprintf(path, sizeof path, "%s/%s", root, rel);
    return chmod(path, mode);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

int fixture_remove(const char *root)
{
    char marker[PATH_MAX];
    struct stat st;

    if (lstat(root, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    snprintf(marker, sizeof marker, "%s/%s", root, MARKER);
    if (lstat(marker, &st) != 0) {      /* 우리가 만든 폴더가 아니면 손대지 않는다 */
        errno = EEXIST;
        return -1;
    }
    return nftw(root, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

int fixture_create(const char *root)
{
    char path[PATH_MAX];
    int i, bad = 0;

    if (fixture_remove(root) != 0)
        return -1;
    if (mkdir(root, 0755) != 0)
        return -1;
    for (i = 0; i < FAC_COUNT; i++) {
        snprintf(path, sizeof path, "%s/%s", root, facility_key(i));
        if (mkdir(path, 0755) != 0)
            return -1;
    }
    bad |= put_file(root, MARKER, "dev fixture\n", 0644);

    /* 발전실 */
    bad |= put_file(root, "generator/generator_config.cfg",
                    "# 발전기 제어 설정\nmode=auto\ntarget_output=480\n", 0640);
    bad |= put_file(root, "generator/generator_label.txt",
                    "model=GT-480\nserial=GN-2291-K\n", 0644);
    bad |= put_file(root, "generator/generator_status.txt", "output=480\nrestart=ready\n", 0644);
    bad |= put_file(root, "generator/temp_sensor_a.dat", "sensor_id=TS-A\nvalue=0.0\n", 0644);
    bad |= put_file(root, "generator/temp_sensor_b.dat", "sensor_id=TS-B\nvalue=500.0\n", 0644);
    bad |= put_file(root, "generator/turbine_status.txt", "pid=\nstate=\n", 0644);
    bad |= age_file(root, "generator/temp_sensor_a.dat", 40);
    bad |= relink(root, "generator/temperature_sensor_link", "temp_sensor_b.dat");

    /* 냉각실 */
    bad |= put_file(root, "cooling/coolant_config.cfg",
                    "# 냉각수 공급 설정\npump_id=CP-2\nflow_rate=120\n", 0644);
    bad |= put_file(root, "cooling/coolant_level.txt", "flow=120\nlevel=85\n", 0644);
    bad |= put_file(root, "cooling/pump_label.txt", "model=CP-2 Mk3\ndevice_id=CP-2-0417\n", 0644);
    bad |= put_file(root, "cooling/pump_status.txt", "pid=\nstate=\n", 0644);
    bad |= put_file(root, "cooling/cooling_sensor_a.dat", "sensor_id=CS-A\nvalue=18.4\n", 0644);
    bad |= put_file(root, "cooling/cooling_sensor_b.dat", "sensor_id=CS-B\nvalue=32.0\n", 0644);
    bad |= age_file(root, "cooling/cooling_sensor_a.dat", 40);
    bad |= relink(root, "cooling/cooling_sensor_link", "cooling_sensor_b.dat");

    /* 배전실 */
    bad |= put_file(root, "power/power_config.cfg",
                    "# 회로별 전력 공급 설정\ncircuit_a_load=65\ncircuit_b_load=65\n"
                    "fault_isolated=1\nsupply_generator=40\nsupply_cooling=60\n", 0644);
    bad |= put_file(root, "power/circuit_status.txt", "circuit_a=normal\ncircuit_b=normal\n", 0644);
    bad |= put_file(root, "power/breaker_status.txt", "breaker_id=BK-7\npid=\nstate=\n", 0644);
    bad |= put_file(root, "power/load_status.txt", "circuit_a=65\ncircuit_b=65\n", 0644);
    bad |= put_file(root, "power/circuit_label.txt", "circuit_a=CR-A1\ncircuit_b=CR-B4\n", 0644);
    return bad ? -1 : 0;
}

int fixture_break(const char *root, int event)
{
    switch (event) {
    case EV_G01: return relink(root, "generator/temperature_sensor_link", "temp_sensor_x.dat");
    case EV_G03: return set_mode(root, "generator/generator_config.cfg", 0000);
    case EV_C01: return set_kv(root, "cooling/coolant_config.cfg", "flow_rate", "45");
    case EV_C03: return relink(root, "cooling/cooling_sensor_link", "cooling_sensor_a.dat");
    case EV_P01:
        return set_kv(root, "power/power_config.cfg", "circuit_a_load", "118") |
               set_kv(root, "power/power_config.cfg", "circuit_b_load", "12");
    case EV_P02: return set_kv(root, "power/power_config.cfg", "fault_isolated", "0");
    case EV_P03:
        return set_kv(root, "power/power_config.cfg", "supply_generator", "75") |
               set_kv(root, "power/power_config.cfg", "supply_cooling", "25");
    default:     return 0;     /* G02, C02 는 프로세스 고장뿐 */
    }
}

int fixture_fix(const char *root, int event)
{
    switch (event) {
    case EV_G01: return relink(root, "generator/temperature_sensor_link", "temp_sensor_b.dat");
    case EV_G03: return set_mode(root, "generator/generator_config.cfg", 0640);
    case EV_C01: return set_kv(root, "cooling/coolant_config.cfg", "flow_rate", "120");
    case EV_C03: return relink(root, "cooling/cooling_sensor_link", "cooling_sensor_b.dat");
    case EV_P01:
        return set_kv(root, "power/power_config.cfg", "circuit_a_load", "65") |
               set_kv(root, "power/power_config.cfg", "circuit_b_load", "65");
    case EV_P02: return set_kv(root, "power/power_config.cfg", "fault_isolated", "1");
    case EV_P03:
        return set_kv(root, "power/power_config.cfg", "supply_generator", "40") |
               set_kv(root, "power/power_config.cfg", "supply_cooling", "60");
    default:     return 0;
    }
}
