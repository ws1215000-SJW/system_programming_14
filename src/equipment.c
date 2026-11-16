/* equipment.c - 설비 프로세스 띄우기, 시그널, 회수 */
#define _GNU_SOURCE
#include "equipment.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

void equip_init(equip_t *e)
{
    memset(e, 0, sizeof *e);
    e->pid = -1;
    e->state = EQ_DEAD;
}

/* equip 실행 파일 경로. 알 수 없으면 빈 문자열 */
static void equip_exe(char *out, size_t n)
{
    const char *env = getenv("PLANT_EQUIP_BIN");
    char self[PATH_MAX];
    ssize_t len;
    char *slash;

    out[0] = '\0';
    if (env != NULL && env[0] != '\0') {
        snprintf(out, n, "%s", env);
        return;
    }
    len = readlink("/proc/self/exe", self, sizeof self - 1);
    if (len <= 0)
        return;
    self[len] = '\0';
    slash = strrchr(self, '/');
    if (slash == NULL)
        return;
    *slash = '\0';
    snprintf(out, n, "%s/equip", self);
}

int equip_spawn(equip_t *e, int fac)
{
    char path[PATH_MAX], parent_arg[16];
    pid_t parent = getpid(), pid;

    equip_destroy(e);
    equip_exe(path, sizeof path);
    snprintf(parent_arg, sizeof parent_arg, "%d", (int)parent);

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int fd;
        sigset_t none;

        /* 시설의 파이프를 물려받지 않는다 (호스트가 EOF 를 못 보게 된다) */
        for (fd = 3; fd < 256; fd++)
            close(fd);
        /* 시설이 막아 둔 시그널 마스크와 핸들러를 물려받지 않는다 (마스크는 exec 뒤에도 남는다) */
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGUSR1, SIG_DFL);
        signal(SIGUSR2, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        if (path[0] != '\0') {
            char *argv[4];

            argv[0] = (char *)equip_key(fac);      /* ps 에 설비 이름으로 보인다 */
            argv[1] = (char *)facility_key(fac);
            argv[2] = parent_arg;
            argv[3] = NULL;
            execv(path, argv);
        }
        _exit(equip_main(fac, parent));            /* 실행 파일이 없으면 여기서 직접 돈다 */
    }
    e->pid = pid;
    e->state = EQ_RUNNING;
    e->exit_code = 0;
    e->term_sig = 0;
    e->spawns++;
    return 0;
}

int equip_signal(equip_t *e, int sig)
{
    if (e->pid <= 0 || e->state == EQ_DEAD) {
        errno = ESRCH;
        return -1;
    }
    return kill(e->pid, sig);
}

int equip_reap(equip_t *e)
{
    equip_state_t before = e->state;

    if (e->pid <= 0 || e->state == EQ_DEAD)
        return 0;
    for (;;) {
        int st;
        pid_t r = waitpid(e->pid, &st, WNOHANG | WUNTRACED | WCONTINUED);

        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            e->state = EQ_DEAD;                    /* ECHILD: 이미 사라졌다 */
            break;
        }
        if (WIFEXITED(st)) {
            e->state = EQ_DEAD;
            e->exit_code = WEXITSTATUS(st);
            break;
        }
        if (WIFSIGNALED(st)) {
            e->state = EQ_DEAD;
            e->term_sig = WTERMSIG(st);
            break;
        }
        if (WIFSTOPPED(st))
            e->state = EQ_STOPPED;
        else if (WIFCONTINUED(st))
            e->state = EQ_RUNNING;
    }
    return e->state != before;
}

void equip_destroy(equip_t *e)
{
    if (e->pid <= 0 || e->state == EQ_DEAD)
        return;
    kill(e->pid, SIGKILL);                         /* 멈춰 있어도 SIGKILL 은 듣는다 */
    while (waitpid(e->pid, NULL, 0) < 0 && errno == EINTR)
        ;
    e->state = EQ_DEAD;
    e->term_sig = SIGKILL;
}

int equip_main(int fac, pid_t parent)
{
    prctl(PR_SET_NAME, equip_key(fac), 0, 0, 0);
    /* 시설이 죽으면 같이 죽는다. 설정하기 전에 이미 죽었는지도 본다 */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent)
        return 0;
    for (;;)
        pause();
}
