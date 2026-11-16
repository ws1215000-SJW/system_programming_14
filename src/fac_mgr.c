/* fac_mgr.c - 호스트 쪽 시설 프로세스 관리 */
#define _GNU_SOURCE
#include "fac_mgr.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "facility.h"
#include "timeutil.h"

static int valid(const fac_mgr_t *m, int fac)
{
    return m != NULL && fac >= 0 && fac < FAC_COUNT;
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

int fac_mgr_init(fac_mgr_t *m, const char *plant_root)
{
    struct sigaction sa;
    struct stat st;
    int i;

    memset(m, 0, sizeof *m);
    for (i = 0; i < FAC_COUNT; i++) {
        m->fac[i].pid = -1;
        m->fac[i].msg_fd = -1;
        m->fac[i].ctrl_fd = -1;
    }
    if (plant_root == NULL || realpath(plant_root, m->root) == NULL)
        return -1;
    if (stat(m->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);       /* 죽은 시설의 제어 파이프에 쓰면 EPIPE 로 받는다 */
    if (sigaction(FAC_SIG_NOTIFY, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL)
        signal(FAC_SIG_NOTIFY, SIG_IGN);
    return 0;
}

int fac_mgr_spawn(fac_mgr_t *m, int fac)
{
    int msg[2], ctl[2];
    pid_t host = getpid(), pid;
    fac_handle_t *h;

    if (!valid(m, fac) || m->fac[fac].alive) {
        errno = EINVAL;
        return -1;
    }
    h = &m->fac[fac];
    if (pipe(msg) != 0)
        return -1;
    if (pipe(ctl) != 0) {
        close(msg[0]);
        close(msg[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(msg[0]); close(msg[1]);
        close(ctl[0]); close(ctl[1]);
        return -1;
    }
    if (pid == 0) {
        facility_args_t a;

        /* 나머지 fd (호스트 쪽 끝, 소켓, 다른 시설의 파이프)는 facility_run 이 닫는다 */
        a.fac = fac;
        a.msg_fd = msg[1];
        a.ctrl_fd = ctl[0];
        a.root = m->root;
        a.host_pid = host;
        _exit(facility_run(&a));
    }

    close(msg[1]);
    close(ctl[0]);
    close_fd(&h->msg_fd);
    close_fd(&h->ctrl_fd);
    h->msg_fd = msg[0];
    h->ctrl_fd = ctl[1];
    fcntl(h->msg_fd, F_SETFL, fcntl(h->msg_fd, F_GETFL) | O_NONBLOCK);
    fcntl(h->ctrl_fd, F_SETFL, fcntl(h->ctrl_fd, F_GETFL) | O_NONBLOCK);
    fcntl(h->msg_fd, F_SETFD, FD_CLOEXEC);
    fcntl(h->ctrl_fd, F_SETFD, FD_CLOEXEC);
    fac_reader_init(&h->reader, h->msg_fd);
    h->pid = pid;
    h->alive = 1;
    h->has_last = 0;
    return 0;
}

int fac_mgr_spawn_all(fac_mgr_t *m)
{
    int i;

    for (i = 0; i < FAC_COUNT; i++)
        if (fac_mgr_spawn(m, i) != 0)
            return -1;
    return 0;
}

int fac_mgr_fd(const fac_mgr_t *m, int fac)
{
    return valid(m, fac) ? m->fac[fac].msg_fd : -1;
}

fac_read_t fac_mgr_read(fac_mgr_t *m, int fac, fac_msg_t *out)
{
    fac_handle_t *h;
    fac_read_t r;

    if (!valid(m, fac) || m->fac[fac].msg_fd < 0)
        return FAC_EOF;
    h = &m->fac[fac];
    r = fac_read_msg(&h->reader, out);
    if (r == FAC_GOT) {
        h->last = *out;
        h->has_last = 1;
    } else if (r == FAC_EOF) {
        close_fd(&h->msg_fd);       /* select 에서 빠지게 */
    }
    return r;
}

static int send_ctrl(fac_mgr_t *m, int fac, fac_ctrl_cmd_t cmd, int a1, int a2, int a3)
{
    if (!valid(m, fac) || !m->fac[fac].alive || m->fac[fac].ctrl_fd < 0) {
        errno = EPIPE;
        return -1;
    }
    return fac_write_ctrl(m->fac[fac].ctrl_fd, cmd, a1, a2, a3);
}

int fac_mgr_arm(fac_mgr_t *m, int fac, int event, int backtime_ms, int variant)
{
    if (event_facility(event) != fac) {
        errno = EINVAL;
        return -1;
    }
    return send_ctrl(m, fac, FCTL_ARM, event, backtime_ms, variant);
}

int fac_mgr_pause(fac_mgr_t *m, int fac)   { return send_ctrl(m, fac, FCTL_PAUSE, 0, 0, 0); }
int fac_mgr_resume(fac_mgr_t *m, int fac)  { return send_ctrl(m, fac, FCTL_RESUME, 0, 0, 0); }
int fac_mgr_disarm(fac_mgr_t *m, int fac)  { return send_ctrl(m, fac, FCTL_DISARM, 0, 0, 0); }
int fac_mgr_link(fac_mgr_t *m, int fac, int level) { return send_ctrl(m, fac, FCTL_LINK, level, 0, 0); }

void fac_mgr_pause_others(fac_mgr_t *m, int except)
{
    int i;

    for (i = 0; i < FAC_COUNT; i++)
        if (i != except)
            (void)fac_mgr_pause(m, i);
}

void fac_mgr_resume_others(fac_mgr_t *m, int except)
{
    int i;

    for (i = 0; i < FAC_COUNT; i++)
        if (i != except)
            (void)fac_mgr_resume(m, i);
}

int fac_mgr_equip_restart(fac_mgr_t *m, int fac)
{
    return send_ctrl(m, fac, FCTL_EQUIP_RESTART, 0, 0, 0);
}

int fac_mgr_equip_signal(fac_mgr_t *m, int fac, int sig)
{
    const fac_handle_t *h;

    if (!valid(m, fac) || !m->fac[fac].alive || !m->fac[fac].has_last) {
        errno = ESRCH;
        return -1;
    }
    h = &m->fac[fac];
    if (h->last.equip_pid <= 0 || h->last.equip_state == EQ_DEAD) {
        errno = ESRCH;
        return -1;
    }
    return kill((pid_t)h->last.equip_pid, sig);
}

int fac_mgr_request_verify(fac_mgr_t *m, int fac)
{
    if (!valid(m, fac) || !m->fac[fac].alive) {
        errno = ESRCH;
        return -1;
    }
    return kill(m->fac[fac].pid, FAC_SIG_VERIFY);
}

int fac_mgr_reap(fac_mgr_t *m)
{
    int i, n = 0;

    for (i = 0; i < FAC_COUNT; i++) {
        fac_handle_t *h = &m->fac[i];
        pid_t r;

        if (!h->alive)
            continue;
        do {
            r = waitpid(h->pid, NULL, WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r == h->pid || (r < 0 && errno == ECHILD)) {
            h->alive = 0;
            close_fd(&h->ctrl_fd);  /* 보고 파이프는 남은 메시지를 읽도록 둔다 */
            n++;
        }
    }
    return n;
}

void fac_mgr_shutdown(fac_mgr_t *m)
{
    int i, waited;

    if (m == NULL)
        return;
    for (i = 0; i < FAC_COUNT; i++) {
        if (!m->fac[i].alive)
            continue;
        (void)send_ctrl(m, i, FCTL_SHUTDOWN, 0, 0, 0);
        kill(m->fac[i].pid, SIGTERM);
    }
    /* 시설이 설비 프로세스를 정리하고 끝날 시간을 준다 (최대 1초) */
    for (waited = 0; waited < 1000; waited += 10) {
        int alive = 0;

        (void)fac_mgr_reap(m);
        for (i = 0; i < FAC_COUNT; i++)
            alive += m->fac[i].alive;
        if (alive == 0)
            break;
        sleep_ms(10);
    }
    for (i = 0; i < FAC_COUNT; i++) {
        fac_handle_t *h = &m->fac[i];

        if (h->alive) {             /* 시설이 SIGKILL 로 죽으면 설비는 PDEATHSIG 로 따라 죽는다 */
            kill(h->pid, SIGKILL);
            while (waitpid(h->pid, NULL, 0) < 0 && errno == EINTR)
                ;
            h->alive = 0;
        }
        close_fd(&h->msg_fd);
        close_fd(&h->ctrl_fd);
    }
}
