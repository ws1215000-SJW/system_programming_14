/*
 * ui.c - ncursesw 화면 (발전소 직원 / 관제사 공용).
 * - 창을 따로 만들지 않고 프레임마다 stdscr 를 지우고 터미널 크기에 맞춰 다시 그린다.
 * - 한글은 한 글자가 두 칸이다. 자르기와 정렬은 모두 표시 폭(mbrtowc + wcwidth)으로 센다.
 * - 직원 화면에는 지금 있는 시설의 계기판과 설비 프로세스만, 관제사 화면에는 세 시설의
 *   수치와 복구율만 나온다. 두 사람이 가진 정보가 달라야 하기 때문이다.
 */
#define _GNU_SOURCE
#define _XOPEN_SOURCE_EXTENDED 1
#include "ui.h"

#include <curses.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "timeutil.h"

#define OUT_MAX      400     /* 명령 출력 줄 수 */
#define OUT_TEXT     512
#define CHAT_MAX     100
#define CHAT_TEXT    400
#define INPUT_BYTES  256
#define HIST_MAX     50
#define SEG_MAX      128     /* 한 줄을 줄바꿈한 조각 수 한도 */
#define KIND_ECHO    100     /* 입력한 명령을 따라 적은 줄 */
#define ALERT_MS     3000    /* 이벤트 발생 직후 머리줄이 깜박이는 시간 */

enum { PAIR_GOOD = 1, PAIR_WARN, PAIR_BAD, PAIR_ACCENT, PAIR_HEAD_BAD, PAIR_HEAD_GOOD };

static struct {
    int       inited;
    role_t    role;
    int       colors;
    ui_view_t view;
    int       has_view;
    int       last_state;
    int       last_event;
    int64_t   alert_since;

    struct { unsigned char kind; char text[OUT_TEXT]; } out[OUT_MAX];
    int out_head, out_n;       /* 원형 버퍼: 가장 오래된 줄이 out_head */
    int scroll;                /* 위로 넘긴 화면 줄 수 */

    struct { unsigned char from; char text[CHAT_TEXT]; } chat[CHAT_MAX];
    int chat_head, chat_n;

    char in[INPUT_BYTES];
    int  in_len, in_cur;
    char hist[HIST_MAX][INPUT_BYTES];
    int  hist_n, hist_pos;     /* hist_pos == -1 이면 새로 쓰는 중 */
    char draft[INPUT_BYTES];
} U;

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ---------- 속성 ---------- */
static attr_t attr_pair(int pair, attr_t fallback)
{
    return U.colors ? (attr_t)COLOR_PAIR(pair) : fallback;
}

/* 위험도 0/1/2 */
static attr_t attr_level(int level)
{
    if (level >= 2)
        return attr_pair(PAIR_BAD, A_REVERSE) | A_BOLD;
    if (level == 1)
        return attr_pair(PAIR_WARN, A_NORMAL) | A_BOLD;
    return A_NORMAL;
}

/* ---------- 표시 폭 ---------- */
/*
 * s[i..] 의 글자 하나. 쓴 바이트 수를 돌려주고 *width 에 칸 수를 준다.
 * 제어 문자는 폭 0 (그리지 않는다), 깨진 바이트는 폭 1 ('?' 로 그린다).
 */
static size_t next_char(const char *s, size_t len, size_t i, mbstate_t *st, int *width, int *bad)
{
    wchar_t wc;
    size_t r = mbrtowc(&wc, s + i, len - i, st);
    int w;

    *bad = 0;
    if (r == (size_t)-1 || r == (size_t)-2) {
        memset(st, 0, sizeof *st);
        *width = 1;
        *bad = 1;
        return 1;
    }
    if (r == 0) {
        *width = 0;
        return 1;
    }
    w = wcwidth(wc);
    *width = w < 0 ? 0 : w;
    return r;
}

static int text_width(const char *s, size_t len)
{
    mbstate_t st;
    size_t i = 0;
    int total = 0;

    memset(&st, 0, sizeof st);
    while (i < len) {
        int w, bad;
        i += next_char(s, len, i, &st, &w, &bad);
        total += w;
    }
    return total;
}

static int str_width(const char *s)
{
    return text_width(s, strlen(s));
}

/* (y, x) 부터 최대 maxcols 칸만 그린다. 그린 칸 수를 돌려준다. */
static int put_clipn(int y, int x, const char *s, size_t len, int maxcols)
{
    char buf[1024];
    size_t n = 0, i = 0;
    int used = 0;
    mbstate_t st;

    if (maxcols <= 0 || y < 0 || y >= LINES || x < 0)
        return 0;
    maxcols = imin(maxcols, COLS - x);
    memset(&st, 0, sizeof st);
    while (i < len && n + MB_LEN_MAX + 1 < sizeof buf) {
        int cw, bad;
        size_t adv = next_char(s, len, i, &st, &cw, &bad);

        if (cw > 0 && used + cw > maxcols)
            break;
        if (bad) {
            buf[n++] = '?';
        } else if (cw > 0 || adv > 1) {     /* 한 바이트짜리 제어 문자만 버린다 */
            memcpy(buf + n, s + i, adv);
            n += adv;
        }
        used += cw;
        i += adv;
    }
    buf[n] = '\0';
    if (n > 0)
        mvaddstr(y, x, buf);
    return used;
}

static int put_clip(int y, int x, const char *s, int maxcols)
{
    return put_clipn(y, x, s, strlen(s), maxcols);
}

/* x..x+cols 의 오른쪽 끝에 맞춰 그린다 */
static void put_right(int y, int x, int cols, const char *s)
{
    int wd = imin(str_width(s), cols);
    put_clip(y, x + cols - wd, s, wd);
}

static void put_center(int y, int x, int cols, const char *s)
{
    int wd = imin(str_width(s), cols);
    put_clip(y, x + (cols - wd) / 2, s, wd);
}

static void fill_row(int y, int x, int cols)
{
    if (cols > 0 && y >= 0 && y < LINES)
        mvhline(y, x, ' ', imin(cols, COLS - x));
}

/*
 * 폭 cols 에 맞춰 줄바꿈한 조각들의 시작 바이트를 starts 에 쓴다. 조각 수(최소 1)를 돌려준다.
 * 되도록 공백 뒤에서 끊고, 공백 없이 긴 낱말은 글자 단위로 끊는다.
 */
static int wrap_segments(const char *s, size_t len, int cols, size_t *starts, int maxn)
{
    mbstate_t st;
    size_t i = 0, brk = 0;
    int n = 1, used = 0, brk_used = 0;

    if (cols < 1)
        cols = 1;
    starts[0] = 0;
    memset(&st, 0, sizeof st);
    while (i < len) {
        int w, bad;
        size_t adv = next_char(s, len, i, &st, &w, &bad);

        if (w > 0 && used > 0 && used + w > cols && n < maxn) {
            if (brk > starts[n - 1]) {
                starts[n++] = brk;
                used -= brk_used;
            } else {
                starts[n++] = i;
                used = 0;
            }
            brk = 0;
            brk_used = 0;
        }
        used += w;
        i += adv;
        if (s[i - adv] == ' ') {
            brk = i;
            brk_used = used;
        }
    }
    return n;
}

/* ---------- 초기화 / 종료 ---------- */
int ui_init(role_t role)
{
    if (U.inited)
        return 0;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return -1;

    setlocale(LC_ALL, "");
    if (strcmp(nl_langinfo(CODESET), "UTF-8") != 0) {
        if (setlocale(LC_ALL, "C.UTF-8") == NULL)
            setlocale(LC_ALL, "en_US.UTF-8");
    }
    set_escdelay(25);
    if (initscr() == NULL)
        return -1;
    cbreak();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    U.colors = 0;
    if (has_colors() && start_color() == OK) {
        use_default_colors();
        init_pair(PAIR_GOOD, COLOR_GREEN, -1);
        init_pair(PAIR_WARN, COLOR_YELLOW, -1);
        init_pair(PAIR_BAD, COLOR_RED, -1);
        init_pair(PAIR_ACCENT, COLOR_CYAN, -1);
        init_pair(PAIR_HEAD_BAD, COLOR_WHITE, COLOR_RED);
        init_pair(PAIR_HEAD_GOOD, COLOR_BLACK, COLOR_GREEN);
        U.colors = 1;
    }
    U.role = role;
    U.hist_pos = -1;
    if (!U.has_view) {
        memset(&U.view, 0, sizeof U.view);
        U.view.event = EV_NONE;
        U.view.location = -1;
    }
    U.last_state = U.view.game_state;
    U.last_event = U.view.event;
    U.inited = 1;
    return 0;
}

void ui_shutdown(void)
{
    if (!U.inited)
        return;
    U.inited = 0;
    curs_set(1);
    endwin();
}

/* ---------- 내용 넣기 ---------- */
void ui_update(const ui_view_t *view)
{
    if (view == NULL)
        return;
    U.view = *view;
    U.view.prompt[sizeof U.view.prompt - 1] = '\0';
    U.view.notice[sizeof U.view.notice - 1] = '\0';
    U.has_view = 1;

    /* 새 이벤트가 시작되면 한 번 울리고 머리줄을 잠깐 깜박인다 */
    if (view->game_state == GS_EVENT_ACTIVE &&
        (U.last_state != GS_EVENT_ACTIVE || U.last_event != view->event)) {
        U.alert_since = now_ms();
        if (U.inited)
            beep();
    }
    U.last_state = view->game_state;
    U.last_event = view->event;
}

static void out_push(int kind, const char *text, size_t len)
{
    int idx;
    size_t i, n = 0;
    char *dst;

    if (U.out_n < OUT_MAX) {
        idx = (U.out_head + U.out_n) % OUT_MAX;
        U.out_n++;
    } else {
        idx = U.out_head;
        U.out_head = (U.out_head + 1) % OUT_MAX;
    }
    U.out[idx].kind = (unsigned char)kind;
    dst = U.out[idx].text;
    for (i = 0; i < len && n + 5 < OUT_TEXT; i++) {
        if (text[i] == '\t') {                  /* 탭은 칸 수가 정해지지 않으므로 공백으로 */
            memcpy(dst + n, "    ", 4);
            n += 4;
        } else if (text[i] != '\r') {
            dst[n++] = text[i];
        }
    }
    dst[n] = '\0';
}

void ui_print(ui_kind_t kind, const char *fmt, ...)
{
    char buf[4096];
    const char *p, *nl;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    for (p = buf;; p = nl + 1) {
        nl = strchr(p, '\n');
        if (nl == NULL) {
            if (*p != '\0' || p == buf)
                out_push(kind, p, strlen(p));
            break;
        }
        out_push(kind, p, (size_t)(nl - p));
    }
}

void ui_chat(role_t from, const char *text)
{
    int idx;

    if (text == NULL)
        return;
    if (U.chat_n < CHAT_MAX) {
        idx = (U.chat_head + U.chat_n) % CHAT_MAX;
        U.chat_n++;
    } else {
        idx = U.chat_head;
        U.chat_head = (U.chat_head + 1) % CHAT_MAX;
    }
    U.chat[idx].from = (unsigned char)from;
    snprintf(U.chat[idx].text, CHAT_TEXT, "%s", text);
}

void ui_clear(void)
{
    U.out_head = U.out_n = 0;
    U.scroll = 0;
}

/* ---------- 그리기: 부품 ---------- */
static void draw_bar(int y, int x, int cols, int pct)
{
    int fill, i;

    pct = imax(0, imin(pct, 100));
    fill = cols * pct / 100;
    for (i = 0; i < cols; i++) {
        if (i < fill) {
            attrset(attr_pair(PAIR_GOOD, A_NORMAL) | A_REVERSE);
            mvaddch(y, x + i, ' ');
        } else {
            attrset(A_DIM);
            mvaddch(y, x + i, '-');
        }
    }
    attrset(A_NORMAL);
}

static void draw_metric(int y, int x, int cols, int fac, int idx, float v)
{
    const char *unit = metric_unit(fac, idx);
    char val[32];

    put_clip(y, x, metric_label(fac, idx), cols);
    if (isnan(v)) {
        snprintf(val, sizeof val, "--");            /* 수신 불가 */
        attrset(attr_level(2));
    } else {
        snprintf(val, sizeof val, "%.*f%s%s", metric_decimals(fac, idx), (double)v,
                 strcmp(unit, "%") == 0 ? "" : " ", unit);
        attrset(attr_level(metric_level(fac, idx, v)));
    }
    put_right(y, x, cols, val);
    attrset(A_NORMAL);
}

static void draw_fac_name(int y, int x, int cols, int fac, int fault)
{
    attrset(A_BOLD | (fault ? attr_level(2) : 0));
    put_clip(y, x, facility_name(fac), cols);
    attrset(A_NORMAL);
}

/* 직원 화면: 지금 있는 시설의 계기판 (8줄). 복구율은 보여 주지 않는다 */
static void draw_field_panel(int y, int x, int cols)
{
    int fac = U.view.location, i, level;
    const ui_fac_t *f;
    char buf[48];

    if (fac < 0 || fac >= FAC_COUNT)
        return;
    f = &U.view.fac[fac];
    draw_fac_name(y, x, cols, fac, f->fault);
    for (i = 0; i < METRIC_COUNT; i++)
        draw_metric(y + 2 + i, x, cols, fac, i, f->metrics[i]);

    put_clip(y + 7, x, equip_name(fac), cols);
    if (f->equip_pid > 0) {
        level = f->equip_state == EQ_DEAD ? 2 : (f->equip_state == EQ_STOPPED ? 1 : 0);
        snprintf(buf, sizeof buf, "%d  %s", f->equip_pid, equip_state_name(f->equip_state));
        attrset(level ? attr_level(level) : attr_pair(PAIR_GOOD, A_NORMAL));
        put_right(y + 7, x, cols, buf);
        attrset(A_NORMAL);
    }
}

/* 관제사 화면: 시설 하나의 복구율과 수치 (6줄). 설비 프로세스는 보여 주지 않는다 */
static void draw_control_panel(int y, int x, int cols, int fac)
{
    const ui_fac_t *f = &U.view.fac[fac];
    char pct[8];
    int i;

    draw_fac_name(y, x, cols, fac, f->fault);
    snprintf(pct, sizeof pct, "%d%%", imax(0, imin(f->progress, 100)));
    attrset(f->progress >= 100 ? attr_pair(PAIR_GOOD, A_BOLD) : A_NORMAL);
    put_right(y, x, cols, pct);
    attrset(A_NORMAL);
    draw_bar(y + 1, x, cols, f->progress);
    for (i = 0; i < METRIC_COUNT; i++)
        draw_metric(y + 2 + i, x, cols, fac, i, f->metrics[i]);
}

static void draw_header(int cols)
{
    const ui_view_t *v = &U.view;
    int64_t now = now_ms();
    attr_t bar = A_REVERSE;
    char mid[128], peer[48], tm[16];
    int x, right;

    switch (v->game_state) {
    case GS_EVENT_ACTIVE:
        bar = attr_pair(PAIR_HEAD_BAD, A_REVERSE) | A_BOLD;
        if (now - U.alert_since < ALERT_MS && ((now - U.alert_since) / 400) % 2 == 1)
            bar = A_REVERSE | A_BOLD;
        break;
    case GS_GAME_OVER:
        bar = attr_pair(PAIR_HEAD_BAD, A_REVERSE) | A_BOLD;
        break;
    case GS_REPAIR_SUCCESS:
    case GS_GAME_CLEAR:
        bar = attr_pair(PAIR_HEAD_GOOD, A_REVERSE);
        break;
    default:
        break;
    }

    attrset(bar);
    fill_row(0, 0, cols);
    x = 1 + put_clip(0, 1, role_name(U.role), cols - 2) + 3;

    if (v->game_state == GS_EVENT_ACTIVE && v->event != EV_NONE)
        snprintf(mid, sizeof mid, "%s %s", event_code(v->event), event_title(v->event));
    else
        snprintf(mid, sizeof mid, "%s", game_state_name(v->game_state));

    snprintf(peer, sizeof peer, "%s %s", U.role == ROLE_FIELD ? "관제사" : "직원",
             v->peer_connected ? "연결" : "없음");
    right = cols - 1 - str_width(peer);
    if (right > x)
        put_clip(0, right, peer, cols - right);

    if (v->game_state == GS_EVENT_ACTIVE) {
        format_mmss(v->limit_left_ms, tm, sizeof tm);
        right -= str_width(tm) + 3;
        if (right > x) {
            /* 30초 아래로 내려가면 남은 시간이 깜박인다 */
            if (v->limit_left_ms <= 30000 && (now / 500) % 2 == 0)
                attrset(A_REVERSE | A_BOLD);
            put_clip(0, right, tm, cols - right);
            attrset(bar);
        }
    }
    if (right - 2 > x)
        put_clip(0, x, mid, right - 2 - x);
    attrset(A_NORMAL);
}

static attr_t out_attr(int kind)
{
    switch (kind) {
    case UI_ERR:    return attr_pair(PAIR_BAD, A_BOLD);
    case UI_OK:     return attr_pair(PAIR_GOOD, A_NORMAL);
    case UI_SYS:    return attr_pair(PAIR_ACCENT, A_BOLD);
    case KIND_ECHO: return A_BOLD;
    default:        return A_NORMAL;
    }
}

/* 명령 출력: 아래쪽(입력줄 쪽)에 붙여 그린다 */
static void draw_output(int top, int h, int x, int cols)
{
    size_t starts[SEG_MAX];
    int i, total = 0, row, skip;

    if (h <= 0 || cols <= 0)
        return;
    for (i = 0; i < U.out_n; i++) {
        const char *s = U.out[(U.out_head + i) % OUT_MAX].text;
        total += wrap_segments(s, strlen(s), cols, starts, SEG_MAX);
    }
    U.scroll = imax(0, imin(U.scroll, total - h));

    row = top + h - 1;
    skip = U.scroll;
    for (i = U.out_n - 1; i >= 0 && row >= top; i--) {
        int idx = (U.out_head + i) % OUT_MAX;
        const char *s = U.out[idx].text;
        size_t len = strlen(s);
        int n = wrap_segments(s, len, cols, starts, SEG_MAX), k;

        attrset(out_attr(U.out[idx].kind));
        for (k = n - 1; k >= 0 && row >= top; k--) {
            size_t end = (k + 1 < n) ? starts[k + 1] : len;

            if (skip > 0) {
                skip--;
                continue;
            }
            put_clipn(row, x, s + starts[k], end - starts[k], cols);
            row--;
        }
    }
    attrset(A_NORMAL);
}

static const char *chat_name(int from)
{
    return from == ROLE_CONTROL ? "관제사" : "직원";
}

/* 통신: 말한 사람 이름만 색을 준다. 내 말과 상대 말의 색이 다르다 */
static void draw_chat(int top, int h, int x, int cols)
{
    size_t starts[SEG_MAX];
    char line[CHAT_TEXT + 32];
    int i, row = top + h - 1;

    if (h <= 0 || cols <= 0)
        return;
    for (i = U.chat_n - 1; i >= 0 && row >= top; i--) {
        int idx = (U.chat_head + i) % CHAT_MAX;
        int from = U.chat[idx].from;
        const char *name = chat_name(from);
        size_t len;
        int n, k;

        snprintf(line, sizeof line, "%s  %s", name, U.chat[idx].text);
        len = strlen(line);
        n = wrap_segments(line, len, cols, starts, SEG_MAX);
        for (k = n - 1; k >= 0 && row >= top; k--) {
            size_t end = (k + 1 < n) ? starts[k + 1] : len;

            put_clipn(row, x, line + starts[k], end - starts[k], cols);
            if (k == 0) {
                attrset((from == (int)U.role ? attr_pair(PAIR_ACCENT, A_NORMAL)
                                             : attr_pair(PAIR_WARN, A_NORMAL)) | A_BOLD);
                put_clip(row, x, name, cols);
                attrset(A_NORMAL);
            }
            row--;
        }
    }
}

/* 테두리가 있는 빈 상자 */
static void draw_box(int y, int x, int h, int w)
{
    int i;

    for (i = 0; i < h; i++)
        fill_row(y + i, x, w);
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
}

/* 접속 대기 / 준비 / 게임 끝: 명령 출력과 통신 영역 가운데에 상자를 띄운다 */
static void draw_overlay(int top, int body_h, int cols)
{   /* cols: 명령 출력 영역의 폭. 계기판과 통신은 가리지 않는다 */
    const ui_view_t *v = &U.view;
    int st = v->game_state;
    int w = imin(cols - 4, 56), h, y, x, i;

    if (st == GS_WAITING || st == GS_READY) {
        h = 7;
        y = top + imax(0, (body_h - h) / 2);
        x = (cols - w) / 2;
        draw_box(y, x, h, w);
        attrset(A_BOLD);
        put_center(y + 2, x + 2, w - 4, role_name(U.role));
        attrset(st == GS_READY ? attr_pair(PAIR_GOOD, A_BOLD) : attr_pair(PAIR_ACCENT, A_NORMAL));
        put_center(y + 4, x + 2, w - 4, v->notice[0] != '\0' ? v->notice : game_state_name(st));
        attrset(A_NORMAL);
    } else if (st == GS_GAME_OVER || st == GS_GAME_CLEAR) {
        int clear = st == GS_GAME_CLEAR;
        int bw = w - 4 - 8 - 6;      /* 이름 8칸, 퍼센트 6칸 */

        h = 9;
        y = top + imax(0, (body_h - h) / 2);
        x = (cols - w) / 2;
        draw_box(y, x, h, w);
        attrset((clear ? attr_pair(PAIR_GOOD, A_NORMAL) : attr_pair(PAIR_BAD, A_REVERSE)) | A_BOLD);
        put_center(y + 2, x + 2, w - 4, game_state_name(st));
        attrset(A_NORMAL);
        for (i = 0; i < FAC_COUNT; i++) {
            char pct[8];

            put_clip(y + 4 + i, x + 2, facility_name(i), 8);
            draw_bar(y + 4 + i, x + 10, bw, v->fac[i].progress);
            snprintf(pct, sizeof pct, "%d%%", imax(0, imin(v->fac[i].progress, 100)));
            put_right(y + 4 + i, x + 10 + bw, 6, pct);
        }
    }
}

/* 입력줄. 커서 위치를 돌려준다 */
static void draw_input(int y, int cols, int *cur_x)
{
    char prompt[96];
    int pw, avail, cursor_col, shift, start = 0, acc = 0;
    mbstate_t st;

    snprintf(prompt, sizeof prompt, "%s%s> ", U.view.prompt, U.view.prompt[0] != '\0' ? " " : "");
    attrset(A_BOLD);
    pw = put_clip(y, 1, prompt, cols / 2);
    attrset(A_NORMAL);

    /* 입력이 길면 커서가 보이도록 앞부분을 밀어낸다 */
    avail = cols - 2 - pw;
    cursor_col = text_width(U.in, (size_t)U.in_cur);
    shift = cursor_col >= avail ? cursor_col - avail + 1 : 0;
    memset(&st, 0, sizeof st);
    while (start < U.in_len && acc < shift) {
        int cw, bad;

        start += (int)next_char(U.in, (size_t)U.in_len, (size_t)start, &st, &cw, &bad);
        acc += cw;
    }
    put_clipn(y, 1 + pw, U.in + start, (size_t)(U.in_len - start), avail);
    *cur_x = 1 + pw + text_width(U.in + start, (size_t)imax(0, U.in_cur - start));
}

/* 터미널 크기가 바뀌었으면 ncurses 에 알린다 (SIGWINCH 처리에 기대지 않는다) */
static void follow_resize(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0 &&
        (ws.ws_row != LINES || ws.ws_col != COLS)) {
        resizeterm(ws.ws_row, ws.ws_col);
        clearok(curscr, TRUE);
    }
}

void ui_draw(void)
{
    int rows, cols, side_w, vx, body_top, body_h, cur_x = 0;

    if (!U.inited)
        return;
    follow_resize();
    rows = LINES;
    cols = COLS;
    erase();

    if (rows < UI_MIN_ROWS || cols < UI_MIN_COLS) {
        char msg[64];

        snprintf(msg, sizeof msg, "%dx%d", UI_MIN_COLS, UI_MIN_ROWS);
        attrset(A_BOLD);
        put_center(rows / 2, 0, cols, msg);
        attrset(A_NORMAL);
        curs_set(0);
        refresh();
        return;
    }

    side_w = imax(30, imin(cols * 36 / 100, 44));
    vx = cols - side_w - 1;                 /* 출력과 오른쪽 영역 사이 세로줄 */
    draw_header(cols);

    if (U.role == ROLE_CONTROL) {
        int c1 = vx / 2;                    /* 발전실과 냉각실 사이 세로줄 */

        /* 위: 세 시설 (1..6행). 배전실 칸은 아래 통신 영역과 폭을 맞춘다 */
        draw_control_panel(1, 1, c1 - 2, FAC_GENERATOR);
        draw_control_panel(1, c1 + 2, vx - c1 - 3, FAC_COOLING);
        draw_control_panel(1, vx + 2, side_w - 2, FAC_POWER);
        mvvline(1, c1, ACS_VLINE, 6);
        mvhline(7, 0, ACS_HLINE, cols);
        mvaddch(7, c1, ACS_BTEE);
        body_top = 8;
        body_h = rows - 2 - body_top;
        mvvline(1, vx, ACS_VLINE, rows - 3);
        mvaddch(7, vx, ACS_PLUS);
        draw_chat(body_top, body_h, vx + 2, side_w - 2);
    } else {
        int gy = 10;                        /* 계기판 아래 가로줄 */

        body_top = 1;
        body_h = rows - 2 - body_top;
        mvvline(body_top, vx, ACS_VLINE, body_h);
        draw_field_panel(1, vx + 2, side_w - 2);
        mvhline(gy, vx + 1, ACS_HLINE, side_w);
        mvaddch(gy, vx, ACS_LTEE);
        draw_chat(gy + 1, rows - 2 - (gy + 1), vx + 2, side_w - 2);
    }
    draw_output(body_top, body_h, 1, vx - 2);

    mvhline(rows - 2, 0, ACS_HLINE, cols);
    mvaddch(rows - 2, vx, ACS_BTEE);

    draw_overlay(body_top, body_h, vx);
    draw_input(rows - 1, cols, &cur_x);

    curs_set(1);
    move(rows - 1, imin(cur_x, cols - 1));
    refresh();
}

/* ---------- 입력 ---------- */
static void in_insert(const char *b, int n)
{
    if (n <= 0 || U.in_len + n >= INPUT_BYTES)
        return;
    memmove(U.in + U.in_cur + n, U.in + U.in_cur, (size_t)(U.in_len - U.in_cur + 1));
    memcpy(U.in + U.in_cur, b, (size_t)n);
    U.in_cur += n;
    U.in_len += n;
    U.hist_pos = -1;
    U.scroll = 0;
}

/* UTF-8 글자 경계로 한 글자 앞/뒤 */
static int in_prev(int i)
{
    if (i > 0)
        i--;
    while (i > 0 && ((unsigned char)U.in[i] & 0xC0) == 0x80)
        i--;
    return i;
}

static int in_next(int i)
{
    if (i < U.in_len)
        i++;
    while (i < U.in_len && ((unsigned char)U.in[i] & 0xC0) == 0x80)
        i++;
    return i;
}

static void in_set(const char *s)
{
    snprintf(U.in, sizeof U.in, "%s", s);
    U.in_len = (int)strlen(U.in);
    U.in_cur = U.in_len;
}

static void hist_add(const char *line)
{
    if (U.hist_n > 0 && strcmp(U.hist[U.hist_n - 1], line) == 0)
        return;
    if (U.hist_n == HIST_MAX) {
        memmove(U.hist[0], U.hist[1], (HIST_MAX - 1) * INPUT_BYTES);
        U.hist_n--;
    }
    snprintf(U.hist[U.hist_n++], INPUT_BYTES, "%s", line);
}

/* Enter: 앞뒤 공백을 떼고, 빈 줄이 아니면 line 에 복사한다. 줄이 완성됐으면 1 */
static int in_submit(char *line, size_t cap)
{
    char echo[INPUT_BYTES + 96];
    char *s = U.in, *e = U.in + U.in_len;
    int got = 0;

    while (s < e && *s == ' ')
        s++;
    while (e > s && e[-1] == ' ')
        e--;
    *e = '\0';
    if (*s != '\0' && cap > 0) {
        hist_add(s);
        snprintf(echo, sizeof echo, "%s%s> %s", U.view.prompt, U.view.prompt[0] != '\0' ? " " : "", s);
        out_push(KIND_ECHO, echo, strlen(echo));
        snprintf(line, cap, "%s", s);
        got = 1;
    }
    U.in[0] = '\0';
    U.in_len = U.in_cur = 0;
    U.hist_pos = -1;
    U.scroll = 0;
    return got;
}

static void key_special(int key)
{
    int page = imax(1, LINES - 6);

    switch (key) {
    case KEY_BACKSPACE:
        if (U.in_cur > 0) {
            int p = in_prev(U.in_cur);

            memmove(U.in + p, U.in + U.in_cur, (size_t)(U.in_len - U.in_cur + 1));
            U.in_len -= U.in_cur - p;
            U.in_cur = p;
            U.hist_pos = -1;
        }
        break;
    case KEY_DC:
        if (U.in_cur < U.in_len) {
            int nx = in_next(U.in_cur);

            memmove(U.in + U.in_cur, U.in + nx, (size_t)(U.in_len - nx + 1));
            U.in_len -= nx - U.in_cur;
            U.hist_pos = -1;
        }
        break;
    case KEY_LEFT:  U.in_cur = in_prev(U.in_cur); break;
    case KEY_RIGHT: U.in_cur = in_next(U.in_cur); break;
    case KEY_HOME:  U.in_cur = 0; break;
    case KEY_END:   U.in_cur = U.in_len; break;
    case KEY_UP:
        if (U.hist_n == 0)
            break;
        if (U.hist_pos == -1) {
            snprintf(U.draft, sizeof U.draft, "%s", U.in);
            U.hist_pos = U.hist_n - 1;
        } else if (U.hist_pos > 0) {
            U.hist_pos--;
        }
        in_set(U.hist[U.hist_pos]);
        break;
    case KEY_DOWN:
        if (U.hist_pos == -1)
            break;
        if (U.hist_pos < U.hist_n - 1) {
            U.hist_pos++;
            in_set(U.hist[U.hist_pos]);
        } else {
            U.hist_pos = -1;
            in_set(U.draft);
        }
        break;
    case KEY_PPAGE: U.scroll += page; break;
    case KEY_NPAGE: U.scroll = imax(0, U.scroll - page); break;
    case KEY_RESIZE:
    default:
        break;
    }
}

ui_input_t ui_handle_input(char *line, size_t cap)
{
    wint_t wc;
    int r;

    if (!U.inited)
        return UI_IN_NONE;
    while ((r = get_wch(&wc)) != ERR) {
        if (r == KEY_CODE_YES) {
            if (wc == KEY_ENTER) {
                if (in_submit(line, cap))
                    return UI_IN_LINE;
            } else {
                key_special((int)wc);
            }
            continue;
        }
        switch (wc) {
        case L'\r': case L'\n':
            if (in_submit(line, cap))
                return UI_IN_LINE;
            break;
        case 127: case 8:
            key_special(KEY_BACKSPACE);
            break;
        case 1:  U.in_cur = 0; break;                       /* Ctrl-A */
        case 5:  U.in_cur = U.in_len; break;                /* Ctrl-E */
        case 21: U.in[0] = '\0'; U.in_len = U.in_cur = 0; break;   /* Ctrl-U */
        case 12: clearok(curscr, TRUE); break;              /* Ctrl-L */
        case 4:                                             /* Ctrl-D */
            if (U.in_len == 0)
                return UI_IN_QUIT;
            break;
        default:
            if (wc >= 32 && iswprint(wc)) {
                char mb[MB_LEN_MAX];
                mbstate_t st;
                size_t n;

                memset(&st, 0, sizeof st);
                n = wcrtomb(mb, (wchar_t)wc, &st);
                if (n != (size_t)-1)
                    in_insert(mb, (int)n);
            }
            break;
        }
    }
    return UI_IN_NONE;
}
