
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "archive_browser.h"

#define UNLOCK_CODE "4829"
#define RESTRICTED_FILE "generator_restart_guide.txt"

// ==================================================
// 1. archive 내부 경로인지 확인
// ==================================================
static int is_inside_archive(
    const char *root,
    const char *path
) {
    size_t root_len = strlen(root);

    if (strcmp(root, path) == 0)
        return 1;

    return strncmp(root, path, root_len) == 0 &&
           path[root_len] == '/';
}

// ==================================================
// 2. 경로 결합 및 정규화
// ==================================================
static int resolve_archive_path(
    const ArchiveContext *ctx,
    const char *target,
    char *resolved
) {
    char candidate[PATH_MAX];

    if (target == NULL || target[0] == '\0') {
        printf("오류: 경로를 입력하세요.\n");
        return -1;
    }

    // 절대 경로 직접 입력 차단
    if (target[0] == '/') {
        printf("접근 거부: 절대 경로는 사용할 수 없습니다.\n");
        return -1;
    }

    int len = snprintf(
        candidate,
        sizeof(candidate),
        "%s/%s",
        ctx->current_path,
        target
    );

    if (len < 0 || (size_t)len >= sizeof(candidate)) {
        printf("오류: 경로가 너무 깁니다.\n");
        return -1;
    }

    if (realpath(candidate, resolved) == NULL) {
        perror("경로 확인");
        return -1;
    }

    if (!is_inside_archive(ctx->root, resolved)) {
        printf("접근 거부: archive 외부 접근 불가\n");
        return -1;
    }

    return 0;
}

// ==================================================
// 3. 제한 문서 여부 확인
// ==================================================
static int is_restricted_file(
    const char *root,
    const char *resolved
) {
    char restricted_path[PATH_MAX];

    int len = snprintf(
        restricted_path,
        sizeof(restricted_path),
        "%s/restricted/%s",
        root,
        RESTRICTED_FILE
    );

    if (len < 0 || (size_t)len >= sizeof(restricted_path))
        return 0;

    return strcmp(resolved, restricted_path) == 0;
}

// ==================================================
// 4. archive 파일 탐색 환경 초기화
// ==================================================
int archive_init(ArchiveContext *ctx) {
    if (ctx == NULL)
        return -1;

    if (realpath("archive", ctx->root) == NULL) {
        perror("archive");
        return -1;
    }

    strcpy(ctx->current_path, ctx->root);
    ctx->is_unlocked = 0;

    return 0;
}

// ==================================================
// 5. 현재 위치 프롬프트 출력
// ==================================================
void archive_print_prompt(const ArchiveContext *ctx) {
    const char *relative =
        ctx->current_path + strlen(ctx->root);

    printf("[archive%s%s] $ ",
           relative,
           relative[0] == '\0' ? "/" : "/");

    fflush(stdout);
}

// ==================================================
// 6. ls : 파일 목록 출력
// ==================================================
static void list_files(const char *path) {
    DIR *dir = opendir(path);

    if (dir == NULL) {
        perror("ls");
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        printf("%s\n", entry->d_name);
    }

    closedir(dir);
}

// ==================================================
// 7. cd : 현재 탐색 디렉터리 변경
// ==================================================
static void change_directory(
    ArchiveContext *ctx,
    const char *target
) {
    char resolved[PATH_MAX];
    struct stat st;

    if (resolve_archive_path(ctx, target, resolved) < 0)
        return;

    if (stat(resolved, &st) == -1) {
        perror("cd");
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        printf("오류: 디렉터리가 아닙니다.\n");
        return;
    }

    strcpy(ctx->current_path, resolved);
}

// ==================================================
// 8. cat : 파일 내용 출력
// ==================================================
static void read_file(
    ArchiveContext *ctx,
    const char *filename
) {
    char resolved[PATH_MAX];
    char buffer[1024];
    struct stat st;

    if (resolve_archive_path(ctx, filename, resolved) < 0)
        return;

    // 제한 문서라면 잠금 상태 확인
    if (is_restricted_file(ctx->root, resolved) &&
        !ctx->is_unlocked) {
        printf("접근 거부: 잠긴 문서입니다.\n");
        printf("unlock 명령어로 인증하세요.\n");
        return;
    }

    if (stat(resolved, &st) == -1) {
        perror("cat");
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        printf("오류: 일반 파일이 아닙니다.\n");
        return;
    }

    int fd = open(resolved, O_RDONLY | O_NONBLOCK);

    if (fd == -1) {
        perror("open");
        return;
    }

    // 열린 파일이 실제 일반 파일인지 재확인
    if (fstat(fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        printf("오류: 일반 파일만 읽을 수 있습니다.\n");
        close(fd);
        return;
    }

    ssize_t bytes_read;

    while ((bytes_read = read(
                fd,
                buffer,
                sizeof(buffer)
            )) > 0) {

        ssize_t total = 0;

        while (total < bytes_read) {
            ssize_t written = write(
                STDOUT_FILENO,
                buffer + total,
                (size_t)(bytes_read - total)
            );

            if (written < 0 && errno == EINTR)
                continue;

            if (written <= 0) {
                perror("write");
                close(fd);
                return;
            }

            total += written;
        }
    }

    if (bytes_read < 0)
        perror("read");

    close(fd);
    printf("\n");
}

// ==================================================
// 9. stat : 파일 메타데이터 조회
// ==================================================
static void show_file_info(
    ArchiveContext *ctx,
    const char *filename
) {
    char resolved[PATH_MAX];
    struct stat st;

    if (resolve_archive_path(ctx, filename, resolved) < 0)
        return;

    if (lstat(resolved, &st) == -1) {
        perror("stat");
        return;
    }

    const char *type;

    if (S_ISREG(st.st_mode))
        type = "Regular file";
    else if (S_ISDIR(st.st_mode))
        type = "Directory";
    else if (S_ISLNK(st.st_mode))
        type = "Symbolic link";
    else
        type = "Other";

    char time_buffer[64];
    struct tm time_info;

    if (localtime_r(&st.st_mtime, &time_info) == NULL) {
        printf("오류: 시간 변환 실패\n");
        return;
    }

    strftime(
        time_buffer,
        sizeof(time_buffer),
        "%Y-%m-%d %H:%M:%S",
        &time_info
    );

    printf("\nFile: %s\n", filename);
    printf("Type: %s\n", type);
    printf("Size: %lld bytes\n",
           (long long)st.st_size);
    printf("Permissions: %03o\n",
           (unsigned)(st.st_mode & 0777));
    printf("Last modified: %s\n\n", time_buffer);
}

// ==================================================
// 10. unlock : 제한 문서 잠금 해제
// ==================================================
static void unlock_file(
    ArchiveContext *ctx,
    const char *arguments
) {
    char filename[256];
    char code[128];
    char extra;

    int parsed = sscanf(
        arguments,
        "%255s %127s %c",
        filename,
        code,
        &extra
    );

    if (parsed != 2) {
        printf("사용법: unlock <파일명> <코드>\n");
        return;
    }

    if (strcmp(filename, RESTRICTED_FILE) != 0) {
        printf("오류: 잠금 대상 문서가 아닙니다.\n");
        return;
    }

    if (strcmp(code, UNLOCK_CODE) == 0) {
        ctx->is_unlocked = 1;
        printf("인증 성공: 문서 잠금 해제 완료!\n");
    }
    else {
        printf("인증 실패: 잘못된 코드입니다.\n");
    }
}

// ==================================================
// 11. 파일 탐색 명령어 통합 처리
// ==================================================
int handle_archive_command(
    ArchiveContext *ctx,
    const char *input
) {
    if (ctx == NULL || input == NULL)
        return 0;

    // ls
    if (strcmp(input, "ls") == 0) {
        list_files(ctx->current_path);
    }

    // cd : archive 최상위 이동
    else if (strcmp(input, "cd") == 0) {
        strcpy(ctx->current_path, ctx->root);
    }

    // cd <경로>
    else if (strncmp(input, "cd ", 3) == 0) {
        change_directory(ctx, input + 3);
    }

    // cat <파일>
    else if (strncmp(input, "cat ", 4) == 0) {
        read_file(ctx, input + 4);
    }

    // stat <파일>
    else if (strncmp(input, "stat ", 5) == 0) {
        show_file_info(ctx, input + 5);
    }

    // unlock <파일> <코드>
    else if (strncmp(input, "unlock ", 7) == 0) {
        unlock_file(ctx, input + 7);
    }

    // 인자가 누락된 명령어
    else if (strcmp(input, "cat") == 0 ||
             strcmp(input, "stat") == 0 ||
             strcmp(input, "unlock") == 0) {
        printf("명령어에 필요한 인자가 없습니다.\n");
    }

    // 파일 탐색 명령이 아님
    else {
        return 0;
    }

    return 1;
}
