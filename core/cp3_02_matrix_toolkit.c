/*
 * cp3_02_matrix_toolkit.c -- หลักการ 3.2: จอไฟ 16x8 เป็นจอเล็ก ๆ ได้ 4 แบบ
 * (The 16x8 light matrix as four small displays)
 *
 * จอไฟ RGB 16x8 มี 128 จุด 8 สี จึงต้องเลือกแบบให้เหมาะกับข่าว กราฟเส้น
 * (blit) เห็นแนวโน้ม หลอด (bar) เห็นว่าเต็มแค่ไหน ตัวเลข (score) อ่านค่า
 * ตัววิ่ง (scroll) ข้อความสั้น ๆ หมุน VR1 ช้า ๆ กด SW6 (ปุ่มบน) เปลี่ยนแบบ
 * กด SW5 (ปุ่มล่าง) ส่งตัววิ่งใหม่ จอภาพนับว่าเขียนจอไฟไปกี่ครั้ง เทียบกับ
 * รอบที่ตัดสิน เพราะเขียนเฉพาะตอนภาพเปลี่ยน เล่น 3 นาทีแล้วจบเอง
 *
 * The 16x8 RGB light matrix has 128 dots in 8 colours, so pick the
 * display that suits the news: a sparkline (blit) shows the trend, a
 * bar shows how full, a score reads the value, a scroll carries a short
 * word. Turn VR1 slowly, press SW6 (upper button) for the next display
 * and SW5 (lower button) to send a new scroll. The screen counts the
 * matrix writes against the decision rounds, because the matrix is
 * written only when its picture changes. The program ends by itself
 * after three minutes.
 *
 * Twin of cp3_02_matrix_toolkit.py
 * (aiot-development-for-smart-farm/core/cp3_02_matrix_toolkit.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - draw() counts any status but BENTO_OK as the .py's OSError: the
 *     write is not counted and the program goes on.
 *   - The picture the .py compares (bytes or a tuple) is a struct: its
 *     display, the 64-byte frame, or the two numbers of a bar or score.
 *   - The history is a 16-value array with a count, where the .py
 *     appends to a list and drops the oldest value.
 *   - The tune table keeps only the tunes this program plays.
 *   - Stop ends the program at once, without the closing screen, as
 *     KeyboardInterrupt does in the .py.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    WARN = 60,  /* from here the colour turns yellow */
    ALARM = 85, /* from here it turns red */
    SAMPLE_MS =
        20, /* read the buttons this often (debounced per read) */
    TICK_MS = 500,   /* decide a new picture this often */
    RUN_MS = 180000, /* the program ends after three minutes */
    SPEAKER = 40,    /* speaker volume, 0-100 % */
    VOLUME = 25      /* tone velocity, 0-127 (about 20 %) */
};

/* The light matrix and the board. */
enum {
    MODE_SPARK = 0,
    MODE_BAR = 1,
    MODE_SCORE = 2,
    MODE_SCROLL = 3,
    MODE_COUNT = 4,
    MATRIX_W = 16,
    MATRIX_ROWS = 8,
    BYTES_PER_ROW = 8, /* two dots per byte */
    FRAME_BYTES = 64,  /* the whole matrix in one blit() */
    HIGH_NIBBLE = 4,   /* an odd x sits in the upper 4 bits */
    SCROLL_MS = 80,
    PERCENT = 100,
    ADC_FULL = 4095, /* pots.read() at the right end */
    VR1 = 0,
    BUTTON_COUNT = 2, /* i = 0 is SW5 = pressed(1), i = 1 is SW6 */
    SW6_INDEX = 1
};

/* Sound. */
enum {
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

/* The tunes this program plays. */
typedef enum {
    TUNE_TAP = 0,
    TUNE_START,
    TUNE_GOOD
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    HINT_Y = 38,
    CARD_Y = 64,
    CARD_H = 274,
    MODES_W = 300,
    VALUE_X = 322,
    VALUE_W = 458,
    CARD_TEXT_X = 12,
    CARD_TEXT_Y = 6,
    CARD_RADIUS = 12,
    MODE_X = 28,
    MODE_Y = 100,
    MODE_STEP = 52,
    SEG_X = 338,
    SEG_Y = 100,
    SEG_W = 150,
    SEG_H = 56,
    CALL_Y = 180,
    COUNT_Y = 220,
    HELP_Y = 352,
    FONT_DEFAULT = 0, /* no value= on the .py's label */
    FONT_TEXT = 20,   /* Thai is 20 px or larger on the board */
    FONT_TITLE = 24,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TEXT_BYTES = 80  /* the counts line with two int32_t */
};

/* Colours, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

typedef struct {
    int16_t x;
    int16_t y;
} point_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} rect_t;

/* What should be on the matrix now, in a form that compares: a
 * frame for the sparkline, a count or value and a colour for the
 * others. */
typedef struct {
    bool known; /* false: nothing drawn yet, or a forced redraw */
    int32_t mode;
    uint8_t frame[FRAME_BYTES];
    int32_t amount;
    int32_t color;
} picture_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t modes[MODE_COUNT];
    bento_ui_id_t seg;
    bento_ui_id_t call;
    bento_ui_id_t count;
    bento_ui_id_t help;
} matrix_screen_t;

typedef struct {
    matrix_screen_t screen;
    int32_t mode;
    int32_t history[MATRIX_W];
    int32_t history_count;
    picture_t last;
    int32_t writes;
    int32_t ticks;
    bool down[BUTTON_COUNT];
    uint32_t tick_ms;
} toolkit_t;

/* The one place a layout number narrows to the API's int16_t. */
static int16_t px(int32_t value)
{
    return (int16_t)value;
}

static bento_status_t check_running(bento_status_t status)
{
    bento_status_t result = status;

    if ((result == BENTO_OK) && !bento_runtime_is_running()) {
        result = BENTO_E_CANCELLED;
    }
    return result;
}

static bento_status_t sleep_ms(uint32_t ms)
{
    return check_running(bento_time_sleep_ms(ms));
}

/* A screen refresh whose events nobody reads. */
static void present(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    /* ui.poll() answers no events when it fails, and this program
     * reads none */
    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    /* a setter's refusal was ignored before too (modui.c) */
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* Plays a tune note by note, 100 ms apart. */
static bento_status_t beep(bento_ui_t *ui, tune_t tune)
{
    static const uint8_t notes[][TUNE_NOTES_MAX] = {
        {76U, 0U, 0U}, {72U, 79U, 0U}, {72U, 79U, 84U}};
    static const uint32_t note_count[] = {1U, 2U, 3U};
    const uint32_t count = note_count[tune];
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        /* ui.tone() sends and waits for no answer (modui.c) */
        (void)bento_ui_sfx_tone(ui, notes[tune][i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)NOTE_MS);
        status = sleep_ms((uint32_t)NOTE_GAP_MS);
    }
    return status;
}

/* The matrix sits on the display core's bus: a write that does not
 * get through is not counted, and the program goes on. */
static bool drawn(bento_status_t status)
{
    return status == BENTO_OK;
}

/* A value 0-100 to a matrix colour number. */
static int32_t level_color(int32_t value)
{
    int32_t color = BENTO_RGBMATRIX_GREEN;

    if (value >= ALARM) {
        color = BENTO_RGBMATRIX_RED;
    } else if (value >= WARN) {
        color = BENTO_RGBMATRIX_YELLOW;
    } else {
        /* below WARN: green */
    }
    return color;
}

/* The last 16 values to a 64-byte blit() frame: one dot per column,
 * a high value near the top row; fewer than 16 sit at the right. */
static void spark_frame(const toolkit_t *toolkit,
                        uint8_t frame[FRAME_BYTES])
{
    const int32_t first_x = MATRIX_W - toolkit->history_count;
    int32_t k;

    (void)memset(frame, 0, FRAME_BYTES);
    for (k = 0; k < toolkit->history_count; k++) {
        const int32_t x = first_x + k;
        const int32_t value = toolkit->history[k];
        const int32_t y =
            (MATRIX_ROWS - 1) - ((value * (MATRIX_ROWS - 1)) / PERCENT);
        const uint32_t color = (uint32_t)level_color(value);
        const uint32_t shift =
            ((x % 2) != 0) ? (uint32_t)HIGH_NIBBLE : 0U;

        frame[(y * BYTES_PER_ROW) + (x / 2)] |=
            (uint8_t)(color << shift);
    }
}

/* The picture that should be on the matrix now; the scroll has none
 * (it is sent on entering the mode and on SW5 only). */
static void make_picture(const toolkit_t *toolkit, int32_t value,
                         picture_t *picture)
{
    (void)memset(picture, 0, sizeof(*picture));
    picture->known = toolkit->mode != MODE_SCROLL;
    picture->mode = toolkit->mode;
    picture->color = level_color(value);
    if (toolkit->mode == MODE_SPARK) {
        spark_frame(toolkit, picture->frame);
    } else if (toolkit->mode == MODE_BAR) {
        picture->amount = (value * MATRIX_W) / PERCENT;
    } else {
        picture->amount = value;
    }
}

static bool same_picture(const picture_t *left, const picture_t *right)
{
    bool same =
        left->known && right->known && (left->mode == right->mode);

    if (same && (left->mode == MODE_SPARK)) {
        same = memcmp(left->frame, right->frame, FRAME_BYTES) == 0;
    } else if (same) {
        same = (left->amount == right->amount) &&
               (left->color == right->color);
    } else {
        /* one of them is not known: they differ */
    }
    return same;
}

static bento_status_t read_percent(int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(VR1, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                point_t at, uint32_t color,
                                int32_t font_px, bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = at.x;
    args.y = at.y;
    args.color = color;
    args.font_px = font_px;
    return bento_ui_label_create(ui, &args, out);
}

/* A card: a dark box with a grey edge and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, rect_t box,
                               const char *title)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    const point_t at = {px(box.x + CARD_TEXT_X),
                        px(box.y + CARD_TEXT_Y)};
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = box.x;
    args.y = box.y;
    args.w = box.w;
    args.h = box.h;
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_label(ui, title, at, COL_INFO, FONT_TEXT, &unused);
    }
    return status;
}

static bento_status_t add_modes_card(matrix_screen_t *screen)
{
    static const char *const modes[MODE_COUNT] = {
        "กราฟเส้น (blit)", "หลอด (bar)", "ตัวเลข (score)",
        "ตัววิ่ง (scroll)"};
    const rect_t box = {MARGIN_X, CARD_Y, MODES_W, CARD_H};
    bento_status_t status = add_card(screen->ui, box, "แบบ");
    int32_t i;

    for (i = 0; (i < MODE_COUNT) && (status == BENTO_OK); i++) {
        const point_t at = {MODE_X, px(MODE_Y + (i * MODE_STEP))};

        status = add_label(screen->ui, modes[i], at, COL_DIM,
                           FONT_DEFAULT, &screen->modes[i]);
    }
    return status;
}

static bento_status_t add_value_card(matrix_screen_t *screen)
{
    static const char dashes[] = "--";
    const rect_t box = {VALUE_X, CARD_Y, VALUE_W, CARD_H};
    const point_t call_at = {SEG_X, CALL_Y};
    const point_t count_at = {SEG_X, COUNT_Y};
    bento_ui_widget_args_t args;
    bento_status_t status =
        add_card(screen->ui, box, "ค่า VR1 และคำสั่งล่าสุด");

    bento_ui_widget_args_init(&args);
    args.text = dashes;
    args.text_len = (uint32_t)strlen(dashes);
    args.x = SEG_X;
    args.y = SEG_Y;
    args.w = SEG_W;
    args.h = SEG_H;
    args.color = COL_OK;
    if (status == BENTO_OK) {
        status = bento_ui_seg7_create(screen->ui, &args, &screen->seg);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", call_at, COL_TEXT,
                           FONT_TEXT, &screen->call);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", count_at, COL_DIM,
                           FONT_TEXT, &screen->count);
    }
    return status;
}

static bento_status_t build_screen(matrix_screen_t *screen)
{
    bento_ui_id_t unused;
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t hint_at = {MARGIN_X, HINT_Y};
    const point_t help_at = {MARGIN_X, HELP_Y};
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "จอไฟ 16x8 เป็นจอเล็ก 4 แบบ",
                           title_at, COL_TEXT, FONT_TITLE, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui,
                           "VR1 = ค่า  SW6 = เปลี่ยนแบบ  SW5 = ส่งตัววิ่งใหม่",
                           hint_at, COL_DIM, FONT_TEXT, &unused);
    }
    if (status == BENTO_OK) {
        status = add_modes_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_value_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "เขียนจอไฟเฉพาะตอนภาพเปลี่ยน",
                           help_at, COL_DIM, FONT_TEXT, &screen->help);
    }
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

static void show_mode(const matrix_screen_t *screen, int32_t mode)
{
    int32_t i;

    for (i = 0; i < MODE_COUNT; i++) {
        /* the widget setters ignore a refusal, as modui.c does */
        (void)bento_ui_set_color(screen->ui, screen->modes[i],
                                 (i == mode) ? COL_WARN : COL_DIM);
    }
}

/* A press edge of one button: SW6 picks the next display, SW5 asks
 * for a new scroll (in the scroll display only). */
static bento_status_t press(toolkit_t *toolkit, int32_t index,
                            bool *send_scroll)
{
    if (index == SW6_INDEX) {
        toolkit->mode = (toolkit->mode + 1) % MODE_COUNT;
        toolkit->last.known = false; /* forces a redraw */
        show_mode(&toolkit->screen, toolkit->mode);
        /* the bar draws the bottom row only: the old picture must go */
        (void)drawn(bento_rgbmatrix_clear());
    }
    *send_scroll = toolkit->mode == MODE_SCROLL;
    return beep(toolkit->screen.ui, TUNE_TAP);
}

/* Both buttons, every SAMPLE_MS (each read is debounced). */
static bento_status_t read_buttons(toolkit_t *toolkit,
                                   bool *send_scroll)
{
    bento_status_t status = BENTO_OK;
    int32_t i;

    for (i = 0; (i < BUTTON_COUNT) && (status == BENTO_OK); i++) {
        bool down = false;

        status = bento_buttons_pressed(1U - (uint32_t)i, &down);
        if ((status == BENTO_OK) && down && !toolkit->down[i]) {
            status = press(toolkit, i, send_scroll);
        }
        toolkit->down[i] = down;
    }
    return status;
}

/* The scroll is sent once; the display core keeps it running. */
static void send_scroll_text(toolkit_t *toolkit, int32_t value)
{
    char word[TEXT_BYTES];
    char call[TEXT_BYTES];
    const int32_t color = level_color(value);

    (void)snprintf(word, sizeof(word), "VR1 %" PRId32, value);
    if (drawn(bento_rgbmatrix_scroll(word, (uint32_t)strlen(word),
                                     color, SCROLL_MS))) {
        toolkit->writes++;
    }
    (void)snprintf(call, sizeof(call),
                   "scroll(\"%s\", %" PRId32 ", 80)", word, color);
    set_text(toolkit->screen.ui, toolkit->screen.call, call);
}

/* Draws a picture that differs from the last one. */
static void draw_picture(toolkit_t *toolkit, const picture_t *picture,
                         int32_t value)
{
    char call[TEXT_BYTES];
    bool ok;

    if (picture->mode == MODE_SPARK) {
        ok = drawn(bento_rgbmatrix_blit(picture->frame, FRAME_BYTES));
        (void)snprintf(call, sizeof(call), "blit(64 ไบต์)");
    } else if (picture->mode == MODE_BAR) {
        ok = drawn(bento_rgbmatrix_bar(value, PERCENT, picture->color));
        (void)snprintf(call, sizeof(call),
                       "bar(%" PRId32 ", 100, %" PRId32 ")", value,
                       picture->color);
    } else {
        ok = drawn(bento_rgbmatrix_score(value, picture->color));
        (void)snprintf(call, sizeof(call),
                       "score(%" PRId32 ", %" PRId32 ")", value,
                       picture->color);
    }
    if (ok) {
        toolkit->writes++;
    }
    set_text(toolkit->screen.ui, toolkit->screen.call, call);
}

static void remember(toolkit_t *toolkit, int32_t value)
{
    int32_t i;

    if (toolkit->history_count == MATRIX_W) {
        for (i = 1; i < MATRIX_W; i++) {
            toolkit->history[i - 1] = toolkit->history[i];
        }
        toolkit->history_count--;
    }
    toolkit->history[toolkit->history_count] = value;
    toolkit->history_count++;
}

/* The value and the counts on screen. */
static void show_value(const toolkit_t *toolkit, int32_t value)
{
    /* the screen colour per matrix colour number: 1 red, 2 green,
     * 3 yellow */
    static const uint32_t on_screen[] = {COL_DIM, COL_BAD, COL_OK,
                                         COL_WARN};
    const matrix_screen_t *screen = &toolkit->screen;
    char text[TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "%" PRId32, value);
    set_text(screen->ui, screen->seg, text);
    /* the number on screen has the matrix's colour */
    (void)bento_ui_set_color(screen->ui, screen->seg,
                             on_screen[level_color(value)]);
    (void)snprintf(text, sizeof(text),
                   "เขียนจอไฟ %" PRId32 " ครั้ง / ตัดสิน %" PRId32 " รอบ",
                   toolkit->writes, toolkit->ticks);
    set_text(screen->ui, screen->count, text);
}

/* A decision round, every TICK_MS: the matrix is written only when
 * its picture changes. */
static void decide(toolkit_t *toolkit, int32_t value)
{
    picture_t picture;

    toolkit->ticks++;
    remember(toolkit, value);
    make_picture(toolkit, value, &picture);
    if (picture.known && !same_picture(&picture, &toolkit->last)) {
        toolkit->last = picture;
        draw_picture(toolkit, &picture, value);
    }
    show_value(toolkit, value);
    present(toolkit->screen.ui);
}

static bento_status_t run_round(toolkit_t *toolkit)
{
    const uint32_t now = bento_time_ticks_ms();
    bool send_scroll = false;
    int32_t value = 0;
    bento_status_t status = read_buttons(toolkit, &send_scroll);

    if (status == BENTO_OK) {
        status = read_percent(&value);
    }
    if ((status == BENTO_OK) && send_scroll) {
        send_scroll_text(toolkit, value);
    }
    if ((status == BENTO_OK) &&
        (bento_time_ticks_diff(now, toolkit->tick_ms) >= TICK_MS)) {
        toolkit->tick_ms = now;
        decide(toolkit, value);
    }
    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SAMPLE_MS);
    }
    return status;
}

static bento_status_t run_loop(toolkit_t *toolkit)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    toolkit->tick_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(toolkit);
    }
    return status;
}

static bento_status_t finish(const matrix_screen_t *screen)
{
    const char *text = "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่";
    bento_status_t status;

    (void)drawn(bento_rgbmatrix_clear());
    (void)bento_ui_set_color(screen->ui, screen->help, COL_WARN);
    set_text(screen->ui, screen->help, text);
    status = beep(screen->ui, TUNE_GOOD);
    if (status == BENTO_OK) {
        present(screen->ui);
    }
    return status;
}

int main(void)
{
    toolkit_t toolkit;
    bento_status_t status;

    (void)memset(&toolkit, 0, sizeof(toolkit));
    toolkit.screen.ui = bento_runtime_get_ui();
    toolkit.mode = MODE_SPARK;
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(toolkit.screen.ui, SPEAKER);
    status = build_screen(&toolkit.screen);
    if (status == BENTO_OK) {
        (void)drawn(bento_rgbmatrix_clear());
        show_mode(&toolkit.screen, toolkit.mode);
        status = beep(toolkit.screen.ui, TUNE_START);
    }
    if (status == BENTO_OK) {
        status = run_loop(&toolkit);
    }
    if (status == BENTO_OK) {
        status = finish(&toolkit.screen);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
