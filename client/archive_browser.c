
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
#define OUTPUT_LINE_SIZE 512

// ==========================================
// 1. 기본 CLI 출력 함수
// ==========================================
static void default_output(
    void *userdata,
    const char *text
) {
    (void)userdata;
    puts(text);
}

// ==========================================
// 2. 출력 방식 설정
// ==========================================
void archive_set_output(
    ArchiveContext *ctx,
    ArchiveOutput output,
    void *userdata
) {
    if (ctx == NULL)
        return;

    ctx->output = output != NULL
                ? output : default_output;

    ctx->output_userdata = userdata;
}

// ==========================================
// 3. 출력 메시지 전달
// ==========================================
static void archive_emit(
    ArchiveContext *ctx,
    const char *format,
    ...
) {
    char buffer[1024];
    va_list args;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (ctx->output != NULL) {
        ctx->output(ctx->output_userdata, buffer);
    }
}

// ==========================================
// 4. archive 내부 경로 검사
// ==========================================
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

// ==========================================
// 5. 경로 정규화
// ==========================================
static int resolve_archive_path(
    ArchiveContext *ctx,
    const char *target,
    char *resolved
) {
    char candidate[PATH_MAX];

    if (target == NULL || target[0] == '\0') {
        archive_emit(ctx, "오류: 경로를 입력하세요.");
        return -1;
    }

    if (target[0] == '/') {
        archive_emit(
            ctx,
            "접근 거부: 절대 경로는 사용할 수 없습니다."
        );
        return -1;
    }

    int length = snprintf(
        candidate,
        sizeof(candidate),
        "%s/%s",
        ctx->current_path,
        target
    );

    if (length < 0 ||
        (size_t)length >= sizeof(candidate)) {
        archive_emit(ctx, "오류: 경로가 너무 깁니다.");
        return -1;
    }

    if (realpath(candidate, resolved) == NULL) {
        archive_emit(
            ctx,
            "경로 확인 실패: %s",
            strerror(errno)
        );
        return -1;
    }

    if (!is_inside_archive(ctx->root, resolved)) {
        archive_emit(
            ctx,
            "접근 거부: archive 외부 접근 불가"
        );
        return -1;
    }

    return 0;
}

// ==========================================
// 6. 제한 문서 검사
// ==========================================
static int is_restricted_file(
    const char *root,
    const char *resolved
) {
    char restricted[PATH_MAX];

    int length = snprintf(
        restricted,
        sizeof(restricted),
        "%s/restricted/%s",
        root,
        RESTRICTED_FILE
    );

    if (length < 0 ||
        (size_t)length >= sizeof(restricted)) {
        return 0;
    }

    return strcmp(restricted, resolved) == 0;
}

// ==========================================
// 7. 파일 탐색 환경 초기화
// ==========================================
int archive_init(ArchiveContext *ctx) {
    if (ctx == NULL)
        return -1;

    memset(ctx, 0, sizeof(*ctx));

    if (realpath("archive", ctx->root) == NULL) {
        perror("archive");
        return -1;
    }

    struct stat st;

    if (stat(ctx->root, &st) < 0 ||
        !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "archive 디렉터리 오류\n");
        return -1;
    }

    strcpy(ctx->current_path, ctx->root);

    ctx->is_unlocked = 0;

    archive_set_output(ctx, NULL, NULL);

    return 0;
}

// ==========================================
// 8. 현재 경로 프롬프트
// ==========================================
void archive_print_prompt(
    const ArchiveContext *ctx
) {
    if (ctx == NULL)
        return;

    const char *relative =
        ctx->current_path + strlen(ctx->root);

    printf(
        "[archive%s%s] $ ",
        relative,
        relative[0] == '\0' ? "/" : "/"
    );

    fflush(stdout);
}

// ==========================================
// 9. ls : 파일 및 폴더 목록
// ==========================================
static void list_files(ArchiveContext *ctx) {
    DIR *dir = opendir(ctx->current_path);

    if (dir == NULL) {
        archive_emit(
            ctx, "ls 오류: %s", strerror(errno)
        );
        return;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        archive_emit(ctx, "%s", entry->d_name);
    }

    closedir(dir);
}

// ==========================================
// 10. cd : 디렉터리 변경
// ==========================================
static void change_directory(
    ArchiveContext *ctx,
    const char *target
) {
    char resolved[PATH_MAX];
    struct stat st;

    if (resolve_archive_path(
            ctx, target, resolved) < 0)
        return;

    if (stat(resolved, &st) < 0) {
        archive_emit(
            ctx, "cd 오류: %s", strerror(errno)
        );
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        archive_emit(
            ctx, "오류: 디렉터리가 아닙니다."
        );
        return;
    }

    strcpy(ctx->current_path, resolved);
}

// ==========================================
// 11. cat : 파일 내용 출력
// ==========================================
static void read_file(
    ArchiveContext *ctx,
    const char *filename
) {
    char resolved[PATH_MAX];
    char buffer[1024];
    char line[OUTPUT_LINE_SIZE];

    size_t used = 0;

    struct stat st;

    if (resolve_archive_path(
            ctx, filename, resolved) < 0)
        return;

    // 제한 문서 잠금 검사
    if (is_restricted_file(ctx->root, resolved) &&
        !ctx->is_unlocked) {

        archive_emit(
            ctx,
            "접근 거부: 잠긴 문서입니다."
        );

        archive_emit(
            ctx,
            "unlock 명령어로 인증하세요."
        );

        return;
    }

    if (stat(resolved, &st) < 0) {
        archive_emit(
            ctx, "cat 오류: %s", strerror(errno)
        );
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        archive_emit(
            ctx,
            "오류: 일반 파일이 아닙니다."
        );
        return;
    }

    int fd = open(
        resolved,
        O_RDONLY | O_NONBLOCK
    );

    if (fd < 0) {
        archive_emit(
            ctx, "open 오류: %s", strerror(errno)
        );
        return;
    }

    if (fstat(fd, &st) < 0 ||
        !S_ISREG(st.st_mode)) {

        archive_emit(
            ctx, "일반 파일만 읽을 수 있습니다."
        );

        close(fd);
        return;
    }

    ssize_t count;

    while (1) {
        count = read(fd, buffer, sizeof(buffer));

        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            break;

        for (ssize_t i = 0; i < count; i++) {
            unsigned char ch =
                (unsigned char)buffer[i];

            // 줄바꿈을 만나면 UI로 한 줄 전송
            if (ch == '\n') {
                if (used > 0 && line[used - 1] == '\r')
                    used--;

                line[used] = '\0';
                archive_emit(ctx, "%s", line);
                used = 0;
                continue;
            }

            // 지나치게 긴 줄은 나누어 출력
            if (used >= sizeof(line) - 1) {
                line[used] = '\0';
                archive_emit(ctx, "%s", line);
                used = 0;
            }

            // UI 화면에 영향을 주는 제어 문자 제외
            // UTF-8 한글 바이트는 유지
            if (ch >= 32 || ch == '\t') {
                line[used++] = (char)ch;
            }
        }
    }

    if (count < 0) {
        archive_emit(
            ctx, "read 오류: %s", strerror(errno)
        );
    }

    if (used > 0) {
        line[used] = '\0';
        archive_emit(ctx, "%s", line);
    }

    close(fd);
}

// ==========================================
// 12. stat : 파일 메타데이터
// ==========================================
static void show_file_info(
    ArchiveContext *ctx,
    const char *filename
) {
    char resolved[PATH_MAX];
    struct stat st;

    if (resolve_archive_path(
            ctx, filename, resolved) < 0)
        return;

    if (lstat(resolved, &st) < 0) {
        archive_emit(
            ctx, "stat 오류: %s", strerror(errno)
        );
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

    struct tm time_info;
    char time_buffer[64];

    if (localtime_r(
            &st.st_mtime, &time_info) == NULL) {
        archive_emit(ctx, "시간 변환 실패");
        return;
    }

    strftime(
        time_buffer,
        sizeof(time_buffer),
        "%Y-%m-%d %H:%M:%S",
        &time_info
    );

    archive_emit(ctx, "File: %s", filename);
    archive_emit(ctx, "Type: %s", type);

    archive_emit(
        ctx,
        "Size: %lld bytes",
        (long long)st.st_size
    );

    archive_emit(
        ctx,
        "Permissions: %03o",
        (unsigned)(st.st_mode & 0777)
    );

    archive_emit(
        ctx,
        "Last modified: %s",
        time_buffer
    );
}

// ==========================================
// 13. unlock : 제한 문서 잠금 해제
// ==========================================
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
        archive_emit(
            ctx,
            "사용법: unlock <파일명> <코드>"
        );
        return;
    }

    if (strcmp(filename, RESTRICTED_FILE) != 0) {
        archive_emit(
            ctx,
            "잠금 대상 문서가 아닙니다."
        );
        return;
    }

    if (strcmp(code, UNLOCK_CODE) == 0) {
        ctx->is_unlocked = 1;

        archive_emit(
            ctx,
            "인증 성공: 문서 잠금 해제 완료!"
        );
    }
    else {
        archive_emit(
            ctx,
            "인증 실패: 잘못된 코드입니다."
        );
    }
}

// ==========================================
// 14. 명령어 통합 처리
// ==========================================
int handle_archive_command(
    ArchiveContext *ctx,
    const char *input
) {
    if (ctx == NULL || input == NULL)
        return 0;

    if (strcmp(input, "ls") == 0) {
        list_files(ctx);
    }

    else if (strcmp(input, "cd") == 0) {
        strcpy(ctx->current_path, ctx->root);
    }

    else if (strncmp(input, "cd ", 3) == 0) {
        change_directory(ctx, input + 3);
    }

    else if (strncmp(input, "cat ", 4) == 0) {
        read_file(ctx, input + 4);
    }

    else if (strncmp(input, "stat ", 5) == 0) {
        show_file_info(ctx, input + 5);
    }

    else if (strncmp(input, "unlock ", 7) == 0) {
        unlock_file(ctx, input + 7);
    }

    else if (strcmp(input, "cat") == 0 ||
             strcmp(input, "stat") == 0 ||
             strcmp(input, "unlock") == 0) {

        archive_emit(
            ctx,
            "명령어에 필요한 인자가 없습니다."
        );
    }

    else {
        return 0;
    }

    return 1;
}
