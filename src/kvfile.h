/*
 * kvfile.h - "key=value" 줄로 된 작은 현장 파일 읽기/고치기.
 * 설정 파일(.cfg), 센서 데이터(.dat), 설비 상태 파일(_status.txt)이 이 형식을 쓴다.
 *   - 한 줄에 하나, '=' 양쪽 공백은 무시한다.
 *   - '#' 으로 시작하는 줄과 '=' 가 없는 줄은 건드리지 않는다.
 * 파일 크기는 KV_FILE_MAX 바이트까지만 다룬다.
 */
#ifndef KVFILE_H
#define KVFILE_H

#include <stddef.h>

#define KV_FILE_MAX 8192

/* key 의 값을 out 에 복사한다. 찾으면 0, 없거나 읽지 못하면 -1. */
int kv_get(const char *path, const char *key, char *out, size_t n);

/* 값을 숫자로 읽는다. 숫자가 아니면 -1. */
int kv_get_double(const char *path, const char *key, double *out);

/*
 * key 줄만 바꾸고 나머지 줄은 그대로 둔다. key 가 없으면 끝에 덧붙인다.
 * 파일이 없으면 만들지 않고 -1 을 돌려준다 (현장 파일은 다른 모듈이 만든다).
 */
int kv_set(const char *path, const char *key, const char *value);

#endif /* KVFILE_H */
