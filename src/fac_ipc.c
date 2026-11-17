/* fac_ipc.c - 파이프 메시지 읽기/쓰기 */
#include "fac_ipc.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

void fac_reader_init(fac_reader_t *r, int fd)
{
    memset(r, 0, sizeof *r);
    r->fd = fd;
}

/*
 * size 바이트짜리 메시지 하나를 out 에 꺼낸다.
 * 버퍼에 완전한 메시지가 있으면 그것부터 주고, 없으면 read() 로 더 채운다.
 */
static fac_read_t read_one(fac_reader_t *r, void *out, size_t size)
{
    for (;;) {
        ssize_t n;

        while (r->len >= sizeof(uint32_t)) {
            uint32_t magic;

            memcpy(&magic, r->buf, sizeof magic);
            if (magic != FAC_MAGIC) {           /* 어긋남: 1바이트 버리고 다시 맞춘다 */
                memmove(r->buf, r->buf + 1, r->len - 1);
                r->len--;
                continue;
            }
            if (r->len < size)
                break;
            memcpy(out, r->buf, size);
            memmove(r->buf, r->buf + size, r->len - size);
            r->len -= size;
            return FAC_GOT;
        }

        n = read(r->fd, r->buf + r->len, sizeof r->buf - r->len);
        if (n > 0) {
            r->len += (size_t)n;
            continue;
        }
        if (n == 0)
            return FAC_EOF;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return FAC_NONE;
        return FAC_EOF;
    }
}

fac_read_t fac_read_msg(fac_reader_t *r, fac_msg_t *out)
{
    fac_read_t res = read_one(r, out, sizeof *out);

    if (res == FAC_GOT)
        out->detail[sizeof out->detail - 1] = '\0';
    return res;
}

fac_read_t fac_read_ctrl(fac_reader_t *r, fac_ctrl_t *out)
{
    return read_one(r, out, sizeof *out);
}

static int write_once(int fd, const void *buf, size_t size)
{
    ssize_t n;

    do {
        n = write(fd, buf, size);
    } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)size)
        return 0;
    if (n >= 0)
        errno = EIO;
    return -1;
}

int fac_write_msg(int fd, fac_msg_t *msg)
{
    msg->magic = FAC_MAGIC;
    return write_once(fd, msg, sizeof *msg);
}

int fac_write_ctrl(int fd, fac_ctrl_cmd_t cmd, int arg1, int arg2, int arg3)
{
    fac_ctrl_t c;

    memset(&c, 0, sizeof c);
    c.magic = FAC_MAGIC;
    c.cmd = cmd;
    c.arg1 = arg1;
    c.arg2 = arg2;
    c.arg3 = arg3;
    return write_once(fd, &c, sizeof c);
}
