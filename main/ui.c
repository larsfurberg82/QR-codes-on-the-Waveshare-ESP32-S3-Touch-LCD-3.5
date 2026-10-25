#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "font8x16.h"
#include "qrcode.h"

/* ------------------------------------------------------------ layout */
#define FIELD_X   4
#define FIELD_Y   4
#define FIELD_W   (UI_W - 2 * FIELD_X)
#define FIELD_H   56
#define FIELD_PAD 6
#define FIELD_CPL ((FIELD_W - 2 * FIELD_PAD) / FONT_W)   /* chars per line */
#define FIELD_LINES 3

#define INFO_Y    62                    /* "QR v5-H" ......... "12/44" */
#define QR_AREA_Y 80
#define QR_AREA_H 216
#define KB_Y      (QR_AREA_Y + QR_AREA_H)
#define KB_ROWS   4
#define KB_ROW_H  ((UI_H - KB_Y) / KB_ROWS)

#define QR_SCALE  ((QR_AREA_H) / (QR_SIZE + 4))   /* keeps >= 2 modules of margin */
_Static_assert(QR_SCALE >= 1, "QR_VERSION too large for the QR area");
_Static_assert(KB_ROW_H * KB_ROWS + KB_Y == UI_H, "keyboard must fill the screen bottom");

/* ------------------------------------------------------------ colours */
/* Panel wants RGB565 big-endian, so pre-swap the bytes. */
#define RGB(r, g, b) ((uint16_t)__builtin_bswap16((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))))
#define COL_BG      RGB(16, 18, 24)
#define COL_FIELD   RGB(34, 38, 50)
#define COL_TEXT    RGB(240, 240, 240)
#define COL_HINT    RGB(120, 126, 140)
#define COL_FULL    RGB(255, 96, 96)
#define COL_QRBG    RGB(255, 255, 255)
#define COL_QRFG    RGB(0, 0, 0)
#define COL_QRHINT  RGB(110, 110, 120)
#define COL_KEY     RGB(62, 66, 82)
#define COL_KEYSPEC RGB(38, 42, 56)
#define COL_KEYDOWN RGB(0, 120, 215)
#define COL_SHIFT   RGB(0, 150, 110)
#define COL_CAPS    RGB(210, 140, 0)

/* -------------------------------------------------------------- keys */
enum { K_NONE, K_CHAR, K_SHIFT, K_BKSP, K_CLR, K_SYM1, K_SYM2, K_ABC };
typedef struct { uint8_t act; char ch; uint8_t w; const char *label; } keydef_t;
typedef struct { const keydef_t *k; uint8_t n; } rowdef_t;

/* Widths are in tenths of a normal key; every row adds up to 100. */
#define CH(c)     { K_CHAR, (c), 10, NULL }
#define CHW(c, w) { K_CHAR, (c), (w), NULL }
#define SP(w)     { K_NONE, 0, (w), NULL }
#define BKSP      { K_BKSP, 0, 15, "DEL" }
#define CLR       { K_CLR, 0, 15, "CLR" }
#define SPACE     { K_CHAR, ' ', 50, NULL }

static const keydef_t L0R0[] = { CH('q'),CH('w'),CH('e'),CH('r'),CH('t'),CH('y'),CH('u'),CH('i'),CH('o'),CH('p') };
static const keydef_t L0R1[] = { SP(5),CH('a'),CH('s'),CH('d'),CH('f'),CH('g'),CH('h'),CH('j'),CH('k'),CH('l'),SP(5) };
static const keydef_t L0R2[] = { { K_SHIFT, 0, 15, "SHIFT" },CH('z'),CH('x'),CH('c'),CH('v'),CH('b'),CH('n'),CH('m'),BKSP };
static const keydef_t L0R3[] = { { K_SYM1, 0, 15, "?123" },CH(','),SPACE,CH('.'),CLR };

static const keydef_t L1R0[] = { CH('1'),CH('2'),CH('3'),CH('4'),CH('5'),CH('6'),CH('7'),CH('8'),CH('9'),CH('0') };
static const keydef_t L1R1[] = { CH('-'),CH('/'),CH(':'),CH(';'),CH('('),CH(')'),CH('$'),CH('&'),CH('@'),CH('"') };
static const keydef_t L1R2[] = { { K_SYM2, 0, 15, "#+=" },CHW('.',14),CHW(',',14),CHW('?',14),CHW('!',14),CHW('\'',14),BKSP };
static const keydef_t L1R3[] = { { K_ABC, 0, 15, "abc" },CH(','),SPACE,CH('.'),CLR };

static const keydef_t L2R0[] = { CH('['),CH(']'),CH('{'),CH('}'),CH('#'),CH('%'),CH('^'),CH('*'),CH('+'),CH('=') };
static const keydef_t L2R1[] = { SP(15),CH('_'),CH('\\'),CH('|'),CH('~'),CH('<'),CH('>'),CH('`'),SP(15) };
static const keydef_t L2R2[] = { { K_SYM1, 0, 15, "123" },CHW('.',14),CHW(',',14),CHW('?',14),CHW('!',14),CHW('\'',14),BKSP };
static const keydef_t L2R3[] = { { K_ABC, 0, 15, "abc" },CH(','),SPACE,CH('.'),CLR };

#define ROW(a) { (a), (uint8_t)(sizeof(a) / sizeof((a)[0])) }
static const rowdef_t LAYERS[3][KB_ROWS] = {
    { ROW(L0R0), ROW(L0R1), ROW(L0R2), ROW(L0R3) },
    { ROW(L1R0), ROW(L1R1), ROW(L1R2), ROW(L1R3) },
    { ROW(L2R0), ROW(L2R1), ROW(L2R2), ROW(L2R3) },
};

/* ------------------------------------------------------------- state */
static uint16_t *s_buf;
static ui_flush_cb_t s_flush;

static char   s_text[UI_TEXT_CAP + 1];
static size_t s_len, s_max;
static qr_code_t s_qr;
static bool   s_qr_ok;

static int      s_layer;                /* 0 letters, 1 symbols, 2 more symbols */
static int      s_shift;                /* 0 off, 1 once, 2 caps lock */
static uint32_t s_shift_ms;
static bool     s_down;
static int      s_prow = -1, s_pidx = -1;
static uint32_t s_press_ms, s_repeat_ms;
static bool     s_cursor_on = true;
static uint32_t s_blink_ms;
static uint32_t s_dirty;                /* one bit per 16 screen rows */

typedef struct { uint16_t *px; int y0, rows; } band_t;

/* ---------------------------------------------------------- drawing */
static void fill(band_t *b, int x, int y, int w, int h, uint16_t c)
{
    int x0 = x < 0 ? 0 : x, x1 = x + w > UI_W ? UI_W : x + w;
    int y0 = y < b->y0 ? b->y0 : y, y1 = y + h > b->y0 + b->rows ? b->y0 + b->rows : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *p = b->px + (yy - b->y0) * UI_W;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}

static void glyph(band_t *b, int x, int y, char ch, int sc, uint16_t fg)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) return;
    if (y >= b->y0 + b->rows || y + FONT_H * sc <= b->y0) return;
    const uint8_t *g = font8x16[ch - FONT_FIRST];
    for (int r = 0; r < FONT_H; r++) {
        if (!g[r]) continue;
        for (int c = 0; c < FONT_W; c++) {
            if (g[r] & (0x80 >> c)) fill(b, x + c * sc, y + r * sc, sc, sc, fg);
        }
    }
}

static void text(band_t *b, int x, int y, const char *s, int sc, uint16_t fg)
{
    for (; *s; s++, x += FONT_W * sc) glyph(b, x, y, *s, sc, fg);
}

static int text_w(const char *s, int sc) { return (int)strlen(s) * FONT_W * sc; }

/* ------------------------------------------------------------ widgets */
static void paint_field(band_t *b)
{
    if (b->y0 >= FIELD_Y + FIELD_H) return;
    fill(b, FIELD_X, FIELD_Y, FIELD_W, FIELD_H, COL_FIELD);

    int cur_line = (int)(s_len / FIELD_CPL), cur_col = (int)(s_len % FIELD_CPL);
    int first = cur_line >= FIELD_LINES ? cur_line - (FIELD_LINES - 1) : 0;
    char tmp[FIELD_CPL + 1];

    for (int ln = first; ln <= cur_line; ln++) {
        size_t off = (size_t)ln * FIELD_CPL;
        size_t n = s_len > off ? s_len - off : 0;
        if (n > FIELD_CPL) n = FIELD_CPL;
        memcpy(tmp, s_text + off, n);
        tmp[n] = 0;
        int y = FIELD_Y + 4 + (ln - first) * FONT_H;
        text(b, FIELD_X + FIELD_PAD, y, tmp, 1, COL_TEXT);
        if (ln == cur_line) {
            if (s_len == 0) text(b, FIELD_X + FIELD_PAD + FONT_W, y, "Type your text...", 1, COL_HINT);
            if (s_cursor_on && s_len < s_max) {
                fill(b, FIELD_X + FIELD_PAD + cur_col * FONT_W, y + 1, 2, FONT_H - 2, COL_KEYDOWN);
            }
        }
    }
}

static void paint_info(band_t *b)
{
    if (b->y0 >= INFO_Y + FONT_H || b->y0 + b->rows <= INFO_Y) return;
    char buf[24];
    snprintf(buf, sizeof buf, "QR v%d-%c", QR_VERSION, "LMQH"[QR_ECC_LEVEL]);
    text(b, FIELD_X, INFO_Y, buf, 1, COL_HINT);
    snprintf(buf, sizeof buf, "%u/%u", (unsigned)s_len, (unsigned)s_max);
    text(b, UI_W - FIELD_X - text_w(buf, 1), INFO_Y, buf, 1, s_len >= s_max ? COL_FULL : COL_TEXT);
}

static void paint_qr(band_t *b)
{
    if (b->y0 >= KB_Y || b->y0 + b->rows <= QR_AREA_Y) return;
    fill(b, FIELD_X, QR_AREA_Y, FIELD_W, QR_AREA_H, COL_QRBG);

    if (!s_qr_ok) {
        const char *l1 = s_len ? "Cannot encode" : "QR code appears here";
        int y = QR_AREA_Y + QR_AREA_H / 2 - FONT_H / 2;
        text(b, (UI_W - text_w(l1, 1)) / 2, y, l1, 1, COL_QRHINT);
        return;
    }
    const int size = QR_SIZE * QR_SCALE;
    const int ox = (UI_W - size) / 2, oy = QR_AREA_Y + (QR_AREA_H - size) / 2;
    int ya = oy > b->y0 ? oy : b->y0;
    int yb = oy + size < b->y0 + b->rows ? oy + size : b->y0 + b->rows;
    for (int y = ya; y < yb; y++) {
        uint8_t my = (uint8_t)((y - oy) / QR_SCALE);
        uint16_t *p = b->px + (y - b->y0) * UI_W + ox;
        for (int mx = 0; mx < QR_SIZE; mx++) {
            if (qr_get_module(&s_qr, (uint8_t)mx, my)) {
                for (int i = 0; i < QR_SCALE; i++) p[mx * QR_SCALE + i] = COL_QRFG;
            }
        }
    }
}

static void key_rect(int row, int cum, int w, int *x0, int *x1, int *y0)
{
    *x0 = cum * UI_W / 100;
    *x1 = (cum + w) * UI_W / 100;
    *y0 = KB_Y + row * KB_ROW_H;
}

static void paint_keyboard(band_t *b)
{
    if (b->y0 + b->rows <= KB_Y) return;
    for (int r = 0; r < KB_ROWS; r++) {
        int ytop = KB_Y + r * KB_ROW_H;
        if (ytop >= b->y0 + b->rows || ytop + KB_ROW_H <= b->y0) continue;
        const rowdef_t *row = &LAYERS[s_layer][r];
        int cum = 0;
        for (int i = 0; i < row->n; i++) {
            const keydef_t *k = &row->k[i];
            int x0, x1, y0;
            key_rect(r, cum, k->w, &x0, &x1, &y0);
            cum += k->w;
            if (k->act == K_NONE) continue;

            uint16_t col = k->act == K_CHAR ? COL_KEY : COL_KEYSPEC;
            const char *label = k->label;
            if (k->act == K_SHIFT && s_shift) { col = s_shift == 2 ? COL_CAPS : COL_SHIFT; if (s_shift == 2) label = "CAPS"; }
            if (r == s_prow && i == s_pidx) col = COL_KEYDOWN;
            fill(b, x0 + 2, y0 + 3, x1 - x0 - 4, KB_ROW_H - 6, col);

            char one[2] = { 0, 0 };
            int sc = 1;
            if (!label) {
                one[0] = k->ch;
                if (s_layer == 0 && s_shift && k->ch >= 'a' && k->ch <= 'z') one[0] = (char)(k->ch - 32);
                label = one;
                sc = 2;
            }
            text(b, x0 + (x1 - x0 - text_w(label, sc)) / 2, y0 + (KB_ROW_H - FONT_H * sc) / 2, label, sc, COL_TEXT);
        }
    }
}

/* ------------------------------------------------------- dirty/flush */
static void mark(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > UI_H) y1 = UI_H;
    for (int s = y0 / 16; s * 16 < y1; s++) s_dirty |= 1u << s;
}

static void flush_dirty(void)
{
    for (int s = 0; s < UI_H / 16; s++) {
        if (!(s_dirty & (1u << s))) continue;
        int n = 1;
        while (n < UI_BAND_ROWS / 16 && s + n < UI_H / 16 && (s_dirty & (1u << (s + n)))) n++;
        band_t b = { s_buf, s * 16, n * 16 };
        fill(&b, 0, b.y0, UI_W, b.rows, COL_BG);
        paint_field(&b);
        paint_info(&b);
        paint_qr(&b);
        paint_keyboard(&b);
        s_flush(b.y0, b.y0 + b.rows, s_buf);
        s += n - 1;
    }
    s_dirty = 0;
}

/* ------------------------------------------------------------- logic */
static void regen(void)
{
    s_qr_ok = s_len > 0 && qr_generate(&s_qr, (const uint8_t *)s_text, (uint16_t)s_len);
    mark(0, KB_Y);
}

static size_t probe_max(void)
{
    memset(s_text, 'A', UI_TEXT_CAP);
    size_t lo = 0, hi = UI_TEXT_CAP;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (qr_generate(&s_qr, (const uint8_t *)s_text, (uint16_t)mid)) lo = mid; else hi = mid - 1;
    }
    memset(s_text, 0, sizeof s_text);
    return lo;
}

static void erase_one(void)
{
    if (s_len) { s_text[--s_len] = 0; regen(); }
}

void ui_clear(void)
{
    s_len = 0;
    s_text[0] = 0;
    regen();
    flush_dirty();
}

static const keydef_t *key_at(int x, int y, int *row_out, int *idx_out)
{
    if (y < KB_Y || y >= UI_H || x < 0 || x >= UI_W) return NULL;
    int r = (y - KB_Y) / KB_ROW_H;
    const rowdef_t *row = &LAYERS[s_layer][r];
    int cum = 0;
    for (int i = 0; i < row->n; i++) {
        int x0, x1, y0;
        key_rect(r, cum, row->k[i].w, &x0, &x1, &y0);
        cum += row->k[i].w;
        if (x >= x0 && x < x1) {
            if (row->k[i].act == K_NONE) return NULL;
            *row_out = r; *idx_out = i;
            return &row->k[i];
        }
    }
    return NULL;
}

static void apply(const keydef_t *k, uint32_t now)
{
    switch (k->act) {
    case K_CHAR: {
        if (s_len >= s_max) break;
        char c = k->ch;
        if (s_layer == 0 && s_shift && c >= 'a' && c <= 'z') c = (char)(c - 32);
        s_text[s_len++] = c;
        s_text[s_len] = 0;
        if (s_shift == 1) { s_shift = 0; mark(KB_Y, UI_H); }
        regen();
        break;
    }
    case K_BKSP: erase_one(); break;
    case K_CLR:  s_len = 0; s_text[0] = 0; regen(); break;
    case K_SHIFT:
        if (s_shift == 0) { s_shift = 1; s_shift_ms = now; }
        else if (s_shift == 1 && now - s_shift_ms < 400) s_shift = 2;
        else s_shift = 0;
        mark(KB_Y, UI_H);
        break;
    case K_SYM1: s_layer = 1; s_prow = -1; mark(KB_Y, UI_H); break;
    case K_SYM2: s_layer = 2; s_prow = -1; mark(KB_Y, UI_H); break;
    case K_ABC:  s_layer = 0; s_prow = -1; mark(KB_Y, UI_H); break;
    default: break;
    }
    s_cursor_on = true;
    s_blink_ms = now;
    mark(FIELD_Y, INFO_Y + FONT_H);
}

void ui_input(bool down, int x, int y, uint32_t now)
{
    if (down && !s_down) {
        s_down = true;
        int r, i;
        const keydef_t *k = key_at(x, y, &r, &i);
        if (k) {
            s_prow = r; s_pidx = i;
            s_press_ms = s_repeat_ms = now;
            apply(k, now);
            if (s_prow >= 0) mark(KB_Y + r * KB_ROW_H, KB_Y + (r + 1) * KB_ROW_H);
        }
    } else if (!down && s_down) {
        s_down = false;
        if (s_prow >= 0) mark(KB_Y + s_prow * KB_ROW_H, KB_Y + (s_prow + 1) * KB_ROW_H);
        s_prow = -1;
    }
    flush_dirty();
}

void ui_tick(uint32_t now)
{
    if (s_down && s_prow >= 0 && LAYERS[s_layer][s_prow].k[s_pidx].act == K_BKSP &&
        now - s_press_ms > 500 && now - s_repeat_ms > 70) {
        s_repeat_ms = now;
        erase_one();
        mark(FIELD_Y, INFO_Y + FONT_H);
    }
    if (now - s_blink_ms >= 500) {
        s_blink_ms = now;
        s_cursor_on = !s_cursor_on;
        mark(FIELD_Y, FIELD_Y + FIELD_H);
    }
    flush_dirty();
}

void ui_init(uint16_t *band_buf, ui_flush_cb_t flush, const char *initial)
{
    s_buf = band_buf;
    s_flush = flush;
    s_max = probe_max();
    if (initial) {
        size_t n = strlen(initial);
        if (n > s_max) n = s_max;
        memcpy(s_text, initial, n);
        s_len = n;
    }
    regen();
    mark(0, UI_H);
    flush_dirty();
}

const char *ui_text(void) { return s_text; }
size_t ui_max_chars(void) { return s_max; }

bool ui_debug_key_center(const char *s, int *x, int *y)
{
    for (int r = 0; r < KB_ROWS; r++) {
        const rowdef_t *row = &LAYERS[s_layer][r];
        int cum = 0;
        for (int i = 0; i < row->n; i++) {
            const keydef_t *k = &row->k[i];
            int x0, x1, y0;
            key_rect(r, cum, k->w, &x0, &x1, &y0);
            cum += k->w;
            if (k->act == K_NONE) continue;
            bool hit = k->label ? strcmp(k->label, s) == 0 : (s[0] == k->ch && s[1] == 0);
            if (hit) { *x = (x0 + x1) / 2; *y = y0 + KB_ROW_H / 2; return true; }
        }
    }
    return false;
}
