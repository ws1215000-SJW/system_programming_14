
#ifndef UI_ADAPTER_H
#define UI_ADAPTER_H

#include "archive_browser.h"
#include "game_state.h"
#include "ui.h"

// GameState를 ncurses 화면용 ui_view_t로 변환
void ui_adapter_build(
    const GameState *state,
    const ArchiveContext *archive,
    int peer_connected,
    ui_view_t *view
);

#endif
