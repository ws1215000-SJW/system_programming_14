/* Drive the console report handler with deterministic facility replies. */
#define main worker_main
#define fac_mgr_equip_signal test_signal
#define fac_mgr_equip_restart test_restart
#define fac_mgr_request_verify test_verify
#define fac_mgr_arm test_arm
#define fac_mgr_link test_link
#include "../src/worker.c"
#undef main
#include "t.h"

static int resumes, restarts, checks, next_event;

int test_link(fac_mgr_t *m, int fac, int level)
{
    (void)m;
    CHECK(fac == FAC_GENERATOR && level == 0);
    return 0;
}

int test_signal(fac_mgr_t *m, int fac, int sig)
{
    (void)m;
    CHECK(fac == FAC_COOLING && sig == SIGCONT);
    resumes++;
    return 0;
}

int test_restart(fac_mgr_t *m, int fac)
{
    (void)m;
    CHECK(fac == FAC_COOLING);
    restarts++;
    return 0;
}

int test_verify(fac_mgr_t *m, int fac)
{
    (void)m;
    CHECK(fac == FAC_COOLING);
    checks++;
    return 0;
}

int test_arm(fac_mgr_t *m, int fac, int event, int delay, int variant)
{
    (void)m;
    CHECK(fac == FAC_COOLING && delay == BACKTIME_MS && variant == 0);
    next_event = event;
    return 0;
}

static void report(worker_t *w, fac_msg_t *msg)
{
    w->mgr.fac[FAC_COOLING].last = *msg;
    w->mgr.fac[FAC_COOLING].has_last = 1;
    handle_report(w, FAC_COOLING, msg);
}

static void command(worker_t *w, const char *input)
{
    char line[32];
    snprintf(line, sizeof line, "%s", input);
    run_command(w, line);
}

static void scenario(int state, const char *input)
{
    worker_t w = {.active = EV_C01, .pending = 1};
    fac_msg_t msg = {.event = EV_C01, .type = FMSG_VERIFY, .verify_ok = 1,
                     .metrics = {32, 85, 70, 94}};
    resumes = restarts = checks = next_event = 0;
    report(&w, &msg);
    CHECK(next_event == EV_C02);
    msg.event = EV_C02;
    msg.type = FMSG_EVENT;
    msg.equip_state = state;
    report(&w, &msg);
    msg.type = FMSG_STATUS;
    report(&w, &msg);
    msg.type = FMSG_EQUIP;
    msg.equip_state = EQ_RUNNING;
    report(&w, &msg);
    msg.type = FMSG_VERIFY;
    report(&w, &msg);
    CHECK(!resumes && !restarts && !checks && next_event == EV_C02 && w.active == EV_C02);
    msg.type = FMSG_STATUS;
    msg.equip_state = state;
    report(&w, &msg);
    command(&w, "go cooling");
    command(&w, state == EQ_STOPPED ? "restart pump" : "resume pump");
    CHECK(!resumes && !restarts && !checks);
    command(&w, input);
    command(&w, input);
    CHECK(resumes == (state == EQ_STOPPED) && restarts == (state == EQ_DEAD));
    if (state != EQ_RUNNING) {
        msg.type = FMSG_STATUS;
        report(&w, &msg);
        CHECK(checks == 0 && !w.pending && next_event == EV_C02);
        msg.type = FMSG_VERIFY;
        report(&w, &msg);
        CHECK(w.active == EV_C02);
        msg.type = FMSG_EQUIP;
        msg.equip_state = EQ_RUNNING;
        report(&w, &msg);
        report(&w, &msg);
    }
    CHECK(checks == 1 && w.pending);
    msg.type = FMSG_VERIFY;
    msg.verify_ok = 0;
    report(&w, &msg);
    CHECK(w.active == EV_C02 && !w.pending && next_event == EV_C02);
    command(&w, "repair pump");
    CHECK(checks == 2 && w.pending);
    msg.verify_ok = 1;
    report(&w, &msg);
    CHECK(w.active == EV_NONE && next_event == EV_C03);
}

int main(void)
{
    worker_t w = {.active = EV_C02};
    fac_msg_t *m = &w.mgr.fac[FAC_COOLING].last;
    m->equip_state = EQ_RUNNING;
    CHECK(recovery_reason(&w) != NULL);
    m->metrics[2] = 70;
    CHECK(recovery_reason(&w) == NULL);
    w.active = EV_P03;
    m = &w.mgr.fac[FAC_POWER].last;
    m->delivered_kw[0] = 2080;
    m->delivered_kw[1] = 3120;
    CHECK(recovery_reason(&w) != NULL);
    m->metrics[1] = 5200;
    CHECK(recovery_reason(&w) == NULL);
    m->delivered_kw[0] = 2400;
    CHECK(recovery_reason(&w) != NULL);
    w.active = EV_G03;
    m->delivered_kw[0] = 2080;
    w.mgr.fac[FAC_GENERATOR].last.metrics[1] = 480;
    float normal[FAC_COUNT][4] = {{500,480,3600,160},{32,85,70,94},{380,5200,60,97}};
    for (int i = FAC_COOLING; i <= FAC_POWER; ++i) {
        w.mgr.fac[i].has_last = 1;
        w.reported_at[i] = now_ms();
        m = &w.mgr.fac[i].last;
        m->repairs = 3;
        m->equip_state = EQ_RUNNING;
        memcpy(m->metrics, normal[i], sizeof m->metrics);
    }
    CHECK(recovery_reason(&w) == NULL);
    w.mgr.fac[FAC_POWER].last.equip_state = EQ_DEAD;
    CHECK(recovery_reason(&w) != NULL);
    scenario(EQ_STOPPED, "resume pump");
    scenario(EQ_DEAD, "restart pump");
    scenario(EQ_STOPPED, "repair pump");
    scenario(EQ_DEAD, "repair pump");
    scenario(EQ_RUNNING, "repair pump");
    return t_done();
}
