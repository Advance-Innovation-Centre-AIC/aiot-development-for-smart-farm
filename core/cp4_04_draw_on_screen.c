/*
 * cp4_04_draw_on_screen.c -- หลักการ 4.4: วาดด้วยของที่มี เส้น (Line) กับ จุด
 * (DotMatrix)
 * (Drawing with what there is: a line and a dot matrix)
 *
 * เฟิร์มแวร์ไม่มี Canvas จึงวาดด้วยของสองอย่าง: ui.Line เส้นหักผ่านจุด
 * ไม่เกิน 16 จุด และ ui.DotMatrix ตารางจุด 16 x 8 ส่งทั้งภาพ 16 ไบต์
 * จอไฟ RGB บนบอร์ดวาดภาพเดียวกันด้วย blit 64 ไบต์ ส่งเฉพาะตอนภาพเปลี่ยน
 * หมุน VR1 (ความชื้นดิน) ขึ้นลงช้า ๆ เส้นกราฟ จุดบนจอ และจอไฟ RGB
 * เลื่อนไปพร้อมกัน ต่ำกว่าเส้นแดง (25 %) เส้นหนาขึ้นและแท่งเป็นสีแดง
 * เล่น 3 นาทีแล้วจบเอง
 *
 * There is no canvas in this firmware, so the picture is drawn with
 * two things: ui.Line, a polyline of at most 16 points, and
 * ui.DotMatrix, a 16 x 8 grid sent whole as 16 bytes. The board's RGB
 * light matrix draws the same picture with a 64-byte blit, sent only
 * when the picture changes. Turn VR1 (soil moisture) slowly: the
 * trend line, the dots and the light matrix move together. Below the
 * red line (25 %) the trend gets thicker and the bars turn red. The
 * program ends by itself after three minutes.
 *
 * Twin of cp4_04_draw_on_screen.py
 * (aiot-development-for-smart-farm/core/cp4_04_draw_on_screen.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - ui.volume has no hasattr test: a board without it answers
 *     BENTO_E_UNSUPPORTED, which is skipped as the .py skips the line.
 *   - The history is an array of POINTS values with a count, where
 *     the .py appends to a list and pops its oldest value.
 *   - Every light matrix status is ignored, as the .py's try/except
 *     OSError ignores a failed draw.
 *   - The tune table keeps only the tunes this program plays.
 *   - Stop ends the program at once, without the closing screen, as
 *     KeyboardInterrupt does in the .py.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    DRY_BELOW = 25, /* soil below this % is dry: thick line, red bars */
    POINTS = 16,    /* history = dot matrix width = points per line */
    SAMPLE_MS = 1000, /* read VR1 and redraw this often */
    LINE_THIN = 3,    /* trend line width, normal (px) */
    LINE_BOLD = 7,    /* trend line width, dry (px) */
    CH_X = 24,        /* the chart frame both lines share */
    CH_Y = 76,
    CH_W = 400,
    CH_H = 160,
    RUN_MS = 180000, /* the program ends after three minutes */
    SPEAKER = 40,    /* speaker volume, 0-100 % */
    VOLUME = 25      /* tone velocity, 0-127 (about 20 %) */
};

/* The knob and the pictures' byte formats. */
enum {
    PERCENT = 100,
    ADC_FULL = 4095, /* pots.read() at the right end */
    VR1 = 0,
    ROWS = 8,          /* dot matrix and light matrix height */
    HALF_PERCENT = 50, /* rounds a bar height to the nearest dot */
    LINE_EDGE = 4,     /* px kept free above and below the trend */
    DOT_BYTES = 16,    /* DotMatrix: 1 bit per dot, 2 bytes a row */
    DOT_ROW_BYTES = 2,
    DOT_BYTE_BITS = 8,
    LEFT_DOT = 0x80,  /* the highest bit is the leftmost dot */
    FRAME_BYTES = 64, /* light matrix: 4 bits per dot */
    FRAME_ROW_BYTES = 8,
    NIBBLE_BITS = 4
};

/* Sound. */
enum {
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    TUNE_NOTES_MAX = 3
};

/* The tunes this program plays. */
typedef enum {
    TUNE_GOOD = 0,
    TUNE_BAD
} tune_t;

/* Screen layout. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    HINT_Y = 38,
    CARD_Y = 66,
    CARD_W = 424,
    CARD_H = 180,
    CARD_RADIUS = 12,
    EDGE_LINE_PX = 2,
    CAPTION_Y = 254,
    VALUE_Y = 284,
    DOTS_X = 466,
    DOTS_W = 304,
    DOTS_H = 152,
    HELP_Y = 352,
    FONT_SMALL = 16,
    FONT_VALUE = 24,
    FONT_TITLE = 24,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TEXT_BYTES = 24  /* "ดิน %d %%" with any int32_t */
};

/* Colours, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

typedef struct {
    int16_t x;
    int16_t y;
} point_t;

/* The widgets the program changes after building the screen. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t trend;
    bento_ui_id_t dots;
    bento_ui_id_t value;
    bento_ui_id_t help;
} draw_screen_t;

/* Soil moisture, oldest first. */
typedef struct {
    int32_t value[POINTS];
    int32_t count;
} history_t;

typedef struct {
    draw_screen_t screen;
    history_t history;
    bool dry_known; /* false until the first reading */
    bool dry;
    bool picture_known; /* false until the first picture is sent */
    uint8_t picture[DOT_BYTES];
} drawing_t;

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

/* One screen refresh; answers how many events came (none are read). */
static uint32_t present(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    /* ui.poll() answers no events when it fails */
    if (bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                      NULL) != BENTO_OK) {
        count = 0U;
    }
    return count;
}

/* Empties the event queue: this program reads no events, but a full
 * queue drops the new ones. */
static void drain_events(bento_ui_t *ui)
{
    while (present(ui) != 0U) {
    }
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
    static const uint8_t notes[][TUNE_NOTES_MAX] = {{72U, 79U, 84U},
                                                    {84U, 76U, 0U}};
    static const uint32_t note_count[] = {3U, 2U};
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

/* VR1 stands in for a soil moisture sensor: 0 = bone dry, 100 =
 * soaked. */
static bento_status_t soil_percent(int32_t *percent)
{
    uint16_t raw = 0U;
    const bento_status_t status = bento_pots_read(VR1, &raw);

    *percent = ((int32_t)raw * PERCENT) / ADC_FULL;
    return status;
}

/* Moisture 0-100 % to a bar 0-8 dots high, rounded. */
static int32_t bar_height(int32_t percent)
{
    return ((percent * ROWS) + HALF_PERCENT) / PERCENT;
}

/* The DotMatrix picture: 2 bytes a row, the highest bit the leftmost
 * dot; each bar grows from the bottom row up. */
static void make_dots(const history_t *history, uint8_t *dots)
{
    int32_t x;
    int32_t y;

    (void)memset(dots, 0, (size_t)DOT_BYTES);
    for (x = 0; x < history->count; x++) {
        for (y = ROWS - bar_height(history->value[x]); y < ROWS; y++) {
            dots[(y * DOT_ROW_BYTES) + (x / DOT_BYTE_BITS)] |=
                (uint8_t)((uint32_t)LEFT_DOT >>
                          (uint32_t)(x % DOT_BYTE_BITS));
        }
    }
}

/* The same picture for the light matrix: 4 bits per dot hold its
 * colour, even x in the low half of the byte, odd x in the high
 * half. */
static void make_frame(const history_t *history, uint8_t *frame)
{
    int32_t x;
    int32_t y;

    (void)memset(frame, 0, (size_t)FRAME_BYTES);
    for (x = 0; x < history->count; x++) {
        const uint32_t color = (history->value[x] < DRY_BELOW)
                                   ? (uint32_t)BENTO_RGBMATRIX_RED
                                   : (uint32_t)BENTO_RGBMATRIX_GREEN;
        const uint32_t shift =
            (uint32_t)NIBBLE_BITS * ((uint32_t)x % 2U);

        for (y = ROWS - bar_height(history->value[x]); y < ROWS; y++) {
            frame[(y * FRAME_ROW_BYTES) + (x / 2)] |=
                (uint8_t)(color << shift);
        }
    }
}

/* Moisture to a y inside the chart frame: 100 % at the top, 0 % at
 * the bottom, LINE_EDGE px clear of each edge. */
static int32_t line_y(int32_t percent)
{
    const int32_t span_px = (int32_t)CH_H - ((int32_t)LINE_EDGE * 2);

    return ((int32_t)CH_H - (int32_t)LINE_EDGE) -
           ((percent * span_px) / (int32_t)PERCENT);
}

static void remember(history_t *history, int32_t percent)
{
    int32_t i;

    if (history->count == POINTS) {
        for (i = 1; i < POINTS; i++) {
            history->value[i - 1] = history->value[i];
        }
        history->count--;
    }
    history->value[history->count] = percent;
    history->count++;
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

/* A line in the chart frame; font_px carries its width. */
static bento_status_t add_line(bento_ui_t *ui, uint32_t color,
                               int32_t width_px, bento_ui_id_t *out)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = CH_X;
    args.y = CH_Y;
    args.w = CH_W;
    args.h = CH_H;
    args.color = color;
    args.font_px = width_px;
    return bento_ui_line_create(ui, &args, out);
}

/* The dry threshold, drawn once with two points. */
static bento_status_t add_edge(bento_ui_t *ui)
{
    const int32_t dry_y = line_y(DRY_BELOW);
    bento_ui_id_t edge;
    const bento_status_t status =
        add_line(ui, COL_BAD, EDGE_LINE_PX, &edge);

    if (status == BENTO_OK) {
        /* add_point() sends and waits for no answer (modui.c) */
        (void)bento_ui_add_point(ui, edge, 0, dry_y);
        (void)bento_ui_add_point(ui, edge, CH_W, dry_y);
    }
    return status;
}

static bento_status_t add_card(bento_ui_t *ui)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;

    bento_ui_widget_args_init(&args);
    args.x = MARGIN_X;
    args.y = CARD_Y;
    args.w = CARD_W;
    args.h = CARD_H;
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = 1;
    return bento_ui_panel_create(ui, &args, &unused);
}

/* cols and rows give the dots, w and h the whole size. */
static bento_status_t add_dots(draw_screen_t *screen)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = DOTS_X;
    args.y = CH_Y;
    args.w = DOTS_W;
    args.h = DOTS_H;
    args.min_val = POINTS;
    args.max_val = ROWS;
    return bento_ui_dotmatrix_create(screen->ui, &args, &screen->dots);
}

static bento_status_t add_headings(bento_ui_t *ui)
{
    const point_t title_at = {MARGIN_X, TITLE_Y};
    const point_t hint_at = {MARGIN_X, HINT_Y};
    bento_ui_id_t unused;
    bento_status_t status =
        add_label(ui, "วาดด้วยของที่มี: เส้น กับ จุด", title_at, COL_TEXT,
                  FONT_TITLE, &unused);

    if (status == BENTO_OK) {
        status = add_label(ui, "ไม่มี Canvas: ใช้ ui.Line กับ ui.DotMatrix",
                           hint_at, COL_DIM, FONT_SMALL, &unused);
    }
    return status;
}

static bento_status_t add_chart(draw_screen_t *screen)
{
    const point_t caption_at = {CH_X, CAPTION_Y};
    const point_t value_at = {CH_X, VALUE_Y};
    bento_ui_id_t unused;
    bento_status_t status = add_card(screen->ui);

    if (status == BENTO_OK) {
        status = add_edge(screen->ui);
    }
    if (status == BENTO_OK) {
        status =
            add_line(screen->ui, COL_INFO, LINE_THIN, &screen->trend);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "เส้นแดง = เกณฑ์ดินแห้ง", caption_at,
                           COL_DIM, FONT_SMALL, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", value_at, COL_TEXT,
                           FONT_VALUE, &screen->value);
    }
    return status;
}

static bento_status_t build_screen(draw_screen_t *screen)
{
    const point_t matrix_at = {DOTS_X, CAPTION_Y};
    const point_t help_at = {MARGIN_X, HELP_Y};
    bento_ui_id_t unused;
    bento_status_t status = bento_ui_screen_default(screen->ui);

    if (status == BENTO_OK) {
        status = sleep_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_headings(screen->ui);
    }
    if (status == BENTO_OK) {
        status = add_chart(screen);
    }
    if (status == BENTO_OK) {
        status = add_dots(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "จอไฟ RGB บนบอร์ด = ภาพเดียวกัน",
                           matrix_at, COL_DIM, FONT_SMALL, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "หมุน VR1 ช้า ๆ", help_at, COL_DIM,
                           FONT_SMALL, &screen->help);
    }
    if (status == BENTO_OK) {
        (void)present(screen->ui);
    }
    return status;
}

/* The whole line again: clear the old points, then add the new ones
 * one by one (at most POINTS). */
static void draw_trend(const draw_screen_t *screen,
                       const history_t *history)
{
    int32_t i;

    /* clear_items() and add_point() wait for no answer (modui.c) */
    (void)bento_ui_items_clear(screen->ui, screen->trend);
    for (i = 0; i < history->count; i++) {
        (void)bento_ui_add_point(screen->ui, screen->trend,
                                 (i * CH_W) / (POINTS - 1),
                                 line_y(history->value[i]));
    }
}

/* The line gets thicker only when the soil turns dry or wet again. */
static bento_status_t show_dryness(drawing_t *drawing, int32_t percent)
{
    const bool dry = percent < DRY_BELOW;
    bento_status_t status = BENTO_OK;

    if (!drawing->dry_known || (dry != drawing->dry)) {
        if (drawing->dry_known) {
            status =
                beep(drawing->screen.ui, dry ? TUNE_BAD : TUNE_GOOD);
        }
        if (status == BENTO_OK) {
            /* prop() sends and waits for no answer (modui.c) */
            (void)bento_ui_set_prop(
                drawing->screen.ui, drawing->screen.trend,
                BENTO_UI_PROP_LINE_WIDTH, dry ? LINE_BOLD : LINE_THIN);
            drawing->dry = dry;
            drawing->dry_known = true;
        }
    }
    return status;
}

/* The dot matrix and the light matrix get the picture only when it
 * changed. */
static void show_picture(drawing_t *drawing)
{
    uint8_t dots[DOT_BYTES];
    uint8_t frame[FRAME_BYTES];

    make_dots(&drawing->history, dots);
    if (!drawing->picture_known ||
        (memcmp(dots, drawing->picture, sizeof(dots)) != 0)) {
        /* set_pixels() waits for no answer (modui.c) */
        (void)bento_ui_dotmatrix_set(drawing->screen.ui,
                                     drawing->screen.dots, dots,
                                     (uint32_t)sizeof(dots));
        make_frame(&drawing->history, frame);
        /* a blit that does not get through is skipped, as the .py's
         * except OSError does */
        (void)bento_rgbmatrix_blit(frame, (uint32_t)sizeof(frame));
        (void)memcpy(drawing->picture, dots, sizeof(dots));
        drawing->picture_known = true;
    }
}

static bento_status_t run_round(drawing_t *drawing)
{
    char text[TEXT_BYTES];
    int32_t percent = 0;
    bento_status_t status = soil_percent(&percent);

    if (status == BENTO_OK) {
        remember(&drawing->history, percent);
        status = show_dryness(drawing, percent);
    }
    if (status == BENTO_OK) {
        draw_trend(&drawing->screen, &drawing->history);
        show_picture(drawing);
        (void)snprintf(text, sizeof(text), "ดิน %" PRId32 " %%",
                       percent);
        set_text(drawing->screen.ui, drawing->screen.value, text);
        drain_events(drawing->screen.ui);
        status = sleep_ms((uint32_t)SAMPLE_MS);
    }
    return status;
}

static bento_status_t run_loop(drawing_t *drawing)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_round(drawing);
    }
    return status;
}

/* Light matrix out and the closing line. */
static void finish(const draw_screen_t *screen)
{
    /* a clear that does not get through is skipped, as the .py's
     * except OSError does */
    (void)bento_rgbmatrix_clear();
    (void)bento_ui_set_color(screen->ui, screen->help, COL_BAD);
    set_text(screen->ui, screen->help,
             "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
    (void)present(screen->ui);
}

int main(void)
{
    drawing_t drawing;
    bento_status_t status;

    (void)memset(&drawing, 0, sizeof(drawing));
    drawing.screen.ui = bento_runtime_get_ui();
    /* a board without ui.volume answers BENTO_E_UNSUPPORTED and is
     * skipped as the .py skips it; elsewhere it waits for no answer */
    (void)bento_ui_sfx_volume_set(drawing.screen.ui, SPEAKER);
    status = build_screen(&drawing.screen);
    if (status == BENTO_OK) {
        status = run_loop(&drawing);
    }
    if (status == BENTO_OK) {
        finish(&drawing.screen);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
