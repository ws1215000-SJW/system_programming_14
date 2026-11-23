
#ifndef ARCHIVE_BROWSER_H
#define ARCHIVE_BROWSER_H

#include <limits.h>

typedef struct {
    char root[PATH_MAX];
    char current_path[PATH_MAX];
    int is_unlocked;
} ArchiveContext;

int archive_init(ArchiveContext *ctx);

void archive_print_prompt(const ArchiveContext *ctx);

int handle_archive_command(
    ArchiveContext *ctx,
    const char *input
);

#endif
