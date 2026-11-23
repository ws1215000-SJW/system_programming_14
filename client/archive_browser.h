
#ifndef ARCHIVE_BROWSER_H
#define ARCHIVE_BROWSER_H

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// 출력 결과를 전달할 함수 형식
typedef void (*ArchiveOutput)(
    void *userdata,
    const char *text
);

// 파일 탐색 상태
typedef struct {
    char root[PATH_MAX];
    char current_path[PATH_MAX];
    int is_unlocked;

    // 출력 방식 관리
    ArchiveOutput output;
    void *output_userdata;
} ArchiveContext;

// 파일 탐색 초기화
int archive_init(ArchiveContext *ctx);

// 출력 함수 지정
void archive_set_output(
    ArchiveContext *ctx,
    ArchiveOutput output,
    void *userdata
);

// 현재 탐색 위치 출력
void archive_print_prompt(const ArchiveContext *ctx);

// 명령어 처리
// 1: 파일 탐색 명령어 처리
// 0: 다른 명령어
int handle_archive_command(
    ArchiveContext *ctx,
    const char *input
);

#endif
