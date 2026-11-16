/* kvfile.c - "key=value" 파일 읽기/고치기 */
#include "kvfile.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 파일 전체를 buf 에 읽는다 (최대 cap-1 바이트, NUL 로 끝냄). 읽은 길이, 실패 -1. */
static ssize_t read_all(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t len = 0;

    if (fd < 0)
        return -1;
    while (len < cap - 1) {
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n > 0) {
            len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(fd);
            return -1;
        }
        break;
    }
    close(fd);
    buf[len] = '\0';
    return (ssize_t)len;
}

/*
 * [line, end) 한 줄이 "key = value" 이고 키가 key 와 같으면 1.
 * 같을 때 값의 범위(앞뒤 공백 제외)를 vstart/vlen 에 준다.
 */
static int line_match(const char *line, const char *end, const char *key,
                      const char **vstart, size_t *vlen)
{
    const char *p = line, *eq, *ke, *vs, *ve;
    size_t klen = strlen(key);

    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if (p >= end || *p == '#')
        return 0;
    eq = memchr(p, '=', (size_t)(end - p));
    if (eq == NULL)
        return 0;
    ke = eq;
    while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t'))
        ke--;
    if ((size_t)(ke - p) != klen || memcmp(p, key, klen) != 0)
        return 0;
    vs = eq + 1;
    ve = end;
    while (vs < ve && (*vs == ' ' || *vs == '\t'))
        vs++;
    while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r'))
        ve--;
    *vstart = vs;
    *vlen = (size_t)(ve - vs);
    return 1;
}

int kv_get(const char *path, const char *key, char *out, size_t n)
{
    char buf[KV_FILE_MAX];
    ssize_t len = read_all(path, buf, sizeof buf);
    const char *line, *stop;

    if (len < 0 || n == 0)
        return -1;
    stop = buf + len;
    for (line = buf; line < stop;) {
        const char *end = memchr(line, '\n', (size_t)(stop - line));
        const char *vs;
        size_t vl;

        if (end == NULL)
            end = stop;
        if (line_match(line, end, key, &vs, &vl)) {
            if (vl >= n)
                vl = n - 1;
            memcpy(out, vs, vl);
            out[vl] = '\0';
            return 0;
        }
        line = end + 1;
    }
    return -1;
}

int kv_get_double(const char *path, const char *key, double *out)
{
    char val[64], *endp;
    double d;

    if (kv_get(path, key, val, sizeof val) != 0 || val[0] == '\0')
        return -1;
    errno = 0;
    d = strtod(val, &endp);
    while (*endp != '\0' && isspace((unsigned char)*endp))
        endp++;
    if (errno != 0 || *endp != '\0')
        return -1;
    *out = d;
    return 0;
}

int kv_set(const char *path, const char *key, const char *value)
{
    char in[KV_FILE_MAX], out[KV_FILE_MAX + 256];
    ssize_t len = read_all(path, in, sizeof in);
    const char *line, *stop;
    size_t olen = 0, off = 0;
    int found = 0, fd, n;

    if (len < 0)
        return -1;
    if (strlen(key) + strlen(value) + 3 > 250)
        return -1;
    stop = in + len;
    for (line = in; line < stop;) {
        const char *end = memchr(line, '\n', (size_t)(stop - line));
        const char *vs;
        size_t vl, linelen;

        if (end == NULL)
            end = stop;
        if (!found && line_match(line, end, key, &vs, &vl)) {
            n = snprintf(out + olen, sizeof out - olen, "%s=%s\n", key, value);
            found = 1;
        } else {
            linelen = (size_t)(end - line);
            memcpy(out + olen, line, linelen);
            out[olen + linelen] = '\n';
            n = (int)linelen + 1;
        }
        olen += (size_t)n;
        if (olen > KV_FILE_MAX)     /* 원본이 한도에 가까우면 손대지 않는다 */
            return -1;
        line = end + 1;
    }
    if (!found) {
        n = snprintf(out + olen, sizeof out - olen, "%s=%s\n", key, value);
        olen += (size_t)n;
    }

    /* 같은 inode 에 덮어쓴다 (rename 을 쓰면 권한과 링크가 바뀐다) */
    fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0)
        return -1;
    while (off < olen) {
        ssize_t w = write(fd, out + off, olen - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    close(fd);
    return 0;
}
