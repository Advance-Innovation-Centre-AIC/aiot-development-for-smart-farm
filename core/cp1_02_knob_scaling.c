/*
 * cp1_02_knob_scaling.c -- หลักการ 1.2: เลขดิบ 0-4095 -> ค่าที่มีหน่วย
 * (แปลงช่วง จำกัดขอบ กันกะพริบ)
 *
 * ลูกบิดส่ง "เลขดิบ" 0-4095 (ADC 12 บิต) ซึ่งยังไม่มีหน่วย map_range()
 * แปลงช่วงแบบเส้นตรง แล้ว clamp() ตัดส่วนที่หลุดขอบ จึงได้ % องศา ลิตร นาที
 * ช่องกันกะพริบ (dead-band) เปลี่ยนตัวเลขบนจอเฉพาะตอน raw ขยับเกิน DEADBAND
 * หมุน VR1-VR4 ช้า ๆ ดู raw เทียบกับตัวเลขใหญ่ "ข้าม" = จำนวนครั้งที่ raw
 * ขยับแต่ตัวเลขใหญ่ไม่เปลี่ยน กด SW5 (ปุ่มล่าง) เปิด/ปิดช่องกันกะพริบ
 * โปรแกรมเล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT Dev Kit และ BENTO Emulator
 *
 * Raw knob counts 0-4095 become values with a unit: a straight-line
 * map, a clamp at both ends, and a dead-band so the big number changes
 * only when the raw count moves more than DEADBAND. Turn VR1-VR4 slowly
 * and compare the raw line with the big number; "skipped" counts the
 * raw moves the dead-band held back. SW5 (lower button) switches the
 * dead-band on and off. It runs for three minutes.
 *
 * Twin of cp1_02_knob_scaling.py (aiot-development-for-smart-farm/core/cp1_02_knob_scaling.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - the KNOBS tuples are a table of structs; "%.0f" / "%.1f" is a
 *     digit count, and the number text is MicroPython's own
 *     (bento_fmt_real_precision);
 *   - real numbers are bento_real_t, the board MicroPython's float;
 *   - round() is a small helper that rounds a half to even, as
 *     MicroPython's round() of a float does;
 *   - None (nothing shown yet) is a bool beside each raw count;
 *   - the TUNES dict is a table indexed by an enum;
 *   - hasattr(ui, "volume") is not asked: the volume call is always
 *     made, and a firmware without it refuses it, which is ignored;
 *   - ui.tone() is fire-and-forget in MicroPython, so its status is
 *     ignored here too;
 *   - Stop ends the program at the next sleep and main returns 0,
 *     without the closing line (MicroPython's KeyboardInterrupt).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_pots.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* Settings a student may change. */
enum {
    DEADBAND =
        20,     /* raw must move more than this to change the number */
    RAW_LO = 0, /* raw count at the knob's left end */
    RAW_HI = 4095,   /* raw count at the knob's right end */
    SAMPLE_MS = 20,  /* read the button this often (debounce) */
    TICK_MS = 500,   /* read the knobs and refresh this often */
    RUN_MS = 180000, /* play for three minutes, then end */
    SPEAKER = 40,    /* speaker level 0-100 % (firmware 2.4.2+) */
    VOLUME = 25      /* tone loudness 0-127 (about 20 %) */
};

/* The course palette, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

/* Screen layout, in pixels. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    SUBTITLE_Y = 36,
    HELP_Y = 352,
    CARD_LEFT_X = 12,
    CARD_RIGHT_X = 402,
    CARD_TOP_Y = 62,
    CARD_BOTTOM_Y = 204,
    CARD_W = 378,
    CARD_H = 134,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    VALUE_DX = 12,
    VALUE_DY = 32,
    RAW_DX = 170,
    RAW_DY = 32,
    STEP_DY = 58,
    BAR_DY = 84,
    BAR_W = 354,
    BAR_H = 14,
    RULER_DY = 100,
    RULER_H = 32
};

/* Font sizes, in pixels. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20, /* Thai text is 20 px or larger on the board */
    FONT_VALUE = 28
};

/* Sound, timing and sizes that are not settings. */
enum {
    SETTLE_MS = 200,     /* let the cleared screen settle */
    TONE_MS = 90,        /* each note of a tune */
    NOTE_GAP_MS = 100,   /* one note to the next */
    TUNE_NOTES = 3,      /* the longest tune */
    TEXT_BYTES = 128,    /* a SET_TEXT carries at most 126 bytes */
    RAW_TEXT_BYTES = 40, /* "raw ", a uint16_t, "  ข้าม ", any int32_t */
    STEP_DIGITS = 4,     /* "%.4f" of one ADC step */
    MAJOR_EVERY = 2,     /* a long tick every 2 ticks on the ruler */
    KNOB_COUNT = 4,
    SW5 = 1 /* SW5, the lower button, is pressed(1) */
};

/* The tunes every course file shares. */
enum {
    TUNE_TAP = 0,
    TUNE_START = 1,
    TUNE_STOP = 2,
    TUNE_GOOD = 3,
    TUNE_BAD = 4,
    TUNE_EMPTY = 5,
    TUNE_HIT = 6,
    TUNE_KINDS = 7
};

/* One knob: its card name, unit, range, digits shown and ruler ticks. */
typedef struct {
    const char *name;
    const char *unit;
    int32_t low;
    int32_t high;
    int32_t digits;
    int32_t ticks;
} knob_t;

static const knob_t KNOBS[KNOB_COUNT] = {
    {"VR1 ความชื้นดิน", "%", 0, 100, 0, 11},
    {"VR2 อุณหภูมิเป้า", "C", 15, 40, 1, 11},
    {"VR3 น้ำในถัง", "ลิตร", 0, 500, 0, 11},
    {"VR4 ตั้งเวลารดน้ำ", "นาที", 0, 60, 0, 13}};

/* Names on the panel: SW5 = lower button = pressed(1), SW6 = upper
 * button = pressed(0). */
static const char *const BUTTON_NAMES[2] = {"SW5", "SW6"};

typedef struct {
    int32_t x;
    int32_t y;
    int32_t font_px;
    uint32_t color;
} label_spec_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} rect_t;

/* The widgets of one knob's card that change later, and the raw text
 * it shows now. */
typedef struct {
    bento_ui_id_t value;
    bento_ui_id_t raw;
    bento_ui_id_t bar;
    char raw_text[RAW_TEXT_BYTES];
} knob_card_t;

/* What the program remembers of one knob between rounds. */
typedef struct {
    bool shown_known; /* has the big number been shown yet */
    uint16_t shown;   /* the raw count the big number comes from */
    bool last_known;
    uint16_t last; /* the raw count of the round before */
    int32_t skips;
} knob_state_t;

typedef struct {
    bento_ui_t *ui;
    knob_card_t cards[KNOB_COUNT];
    knob_state_t knobs[KNOB_COUNT];
    bento_ui_id_t help;
    int32_t band;
    bool was_down;
} app_t;

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

/* A sleep that turns Stop into BENTO_E_CANCELLED. */
static bento_status_t pause_ms(uint32_t delay_ms)
{
    bento_status_t status = bento_time_sleep_ms(delay_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* Tones go through ui.tone because its loudness can be set. */
static bento_status_t beep(bento_ui_t *ui, int32_t tune)
{
    static const uint8_t notes[TUNE_KINDS][TUNE_NOTES] = {
        {76U},      {72U, 79U},      {79U, 72U}, {72U, 79U, 84U},
        {84U, 76U}, {84U, 76U, 69U}, {88U}};
    static const uint8_t lengths[TUNE_KINDS] = {1U, 2U, 2U, 3U,
                                                2U, 3U, 1U};
    bento_status_t status = BENTO_OK;
    uint8_t i;

    for (i = 0U; (i < lengths[tune]) && (status == BENTO_OK); i++) {
        /* fire-and-forget in MicroPython too */
        (void)bento_ui_sfx_tone(ui, notes[tune][i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = pause_ms((uint32_t)NOTE_GAP_MS);
    }
    return status;
}

/* Straight-line map: in_low -> out_low, in_high -> out_high. */
static bento_real_t map_range(int32_t x, int32_t in_low,
                              int32_t in_high, int32_t out_low,
                              int32_t out_high)
{
    const int32_t scaled = (x - in_low) * (out_high - out_low);

    return (bento_real_t)out_low +
           ((bento_real_t)scaled / (bento_real_t)(in_high - in_low));
}

/* A value outside low..high is cut to the nearer end. */
static bento_real_t clamp(bento_real_t value, int32_t low, int32_t high)
{
    bento_real_t result = value;

    if (value < (bento_real_t)low) {
        result = (bento_real_t)low;
    } else if (value > (bento_real_t)high) {
        result = (bento_real_t)high;
    } else {
        /* already inside */
    }
    return result;
}

static bento_real_t to_unit(uint16_t raw, const knob_t *knob)
{
    return clamp(
        map_range((int32_t)raw, RAW_LO, RAW_HI, knob->low, knob->high),
        knob->low, knob->high);
}

/* How far the value moves for one raw count. */
static bento_real_t step_size(const knob_t *knob)
{
    return (bento_real_t)(knob->high - knob->low) /
           (bento_real_t)(RAW_HI - RAW_LO);
}

/* The dead-band: change the number only when raw is more than band
 * away from the raw count the number comes from. */
static bool moved(uint16_t raw, const knob_state_t *state, int32_t band)
{
    const int32_t distance = (int32_t)raw - (int32_t)state->shown;

    return !state->shown_known ||
           (((distance < 0) ? -distance : distance) > band);
}

/* MicroPython's round() of a float: to the nearest whole number, a
 * half to the even one. */
static int32_t round_half_even(bento_real_t value)
{
    const bento_real_t half = (bento_real_t)0.5;
    int32_t whole = (int32_t)value;
    bento_real_t rest;

    if ((bento_real_t)whole > value) {
        whole--;
    }
    rest = value - (bento_real_t)whole;
    if ((rest > half) || ((rest == half) && ((whole % 2) != 0))) {
        whole++;
    }
    return whole;
}

/* MicroPython's own '%.<digits>f' text; a BENTO_FMT_BUF_SIZE buffer
 * always holds it. */
static void format_real(char *text, bento_real_t value, int32_t digits)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value,
                                 digits, &length) != BENTO_OK) {
        text[0] = '\0';
    }
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                const label_spec_t *spec,
                                bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = px(spec->x);
    args.y = px(spec->y);
    args.color = spec->color;
    args.font_px = spec->font_px;
    return bento_ui_label_create(ui, &args, out_id);
}

/* A card: a dark box with a grey border and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, const rect_t *box,
                               const char *title)
{
    const label_spec_t spec = {box->x + CARD_TITLE_DX,
                               box->y + CARD_TITLE_DY, FONT_TEXT,
                               COL_INFO};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(box->x);
    args.y = px(box->y);
    args.w = px(box->w);
    args.h = px(box->h);
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = CARD_BORDER;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_label(ui, title, &spec, &unused);
    }
    return status;
}

/* The bar under the number; its colour must be set after it is made. */
static bento_status_t add_bar(bento_ui_t *ui, const knob_t *knob,
                              const rect_t *box, bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(box->x);
    args.y = px(box->y);
    args.w = px(box->w);
    args.h = px(box->h);
    args.min_val = knob->low;
    args.max_val = knob->high;
    args.font_px = knob->low;
    status = bento_ui_bar_create(ui, &args, out_id);
    if (status == BENTO_OK) {
        /* a setter's refusal is ignored, as in MicroPython */
        (void)bento_ui_set_color(ui, *out_id, COL_INFO);
    }
    return status;
}

/* The ruler under the bar: ticks only, a Scale has no needle here. */
static bento_status_t add_ruler(bento_ui_t *ui, const knob_t *knob,
                                const rect_t *box)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t ruler;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(box->x);
    args.y = px(box->y);
    args.w = px(box->w);
    args.h = px(box->h);
    args.color = COL_DIM;
    args.min_val = knob->low;
    args.max_val = knob->high;
    status = bento_ui_scale_create(ui, &args, &ruler);
    if (status == BENTO_OK) {
        /* a setter's refusal is ignored, as in MicroPython */
        (void)bento_ui_set_ticks(ui, ruler, knob->ticks, MAJOR_EVERY);
    }
    return status;
}

/* The number, the raw line and the size of one step. */
static bento_status_t add_knob_labels(bento_ui_t *ui,
                                      const knob_t *knob,
                                      const rect_t *box,
                                      knob_card_t *card)
{
    const label_spec_t value = {box->x + VALUE_DX, box->y + VALUE_DY,
                                FONT_VALUE, COL_TEXT};
    const label_spec_t raw = {box->x + RAW_DX, box->y + RAW_DY,
                              FONT_TEXT, COL_WARN};
    const label_spec_t step = {box->x + RAW_DX, box->y + STEP_DY,
                               FONT_TEXT, COL_DIM};
    char number[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];
    bento_ui_id_t unused;
    bento_status_t status = add_label(ui, "--", &value, &card->value);

    if (status == BENTO_OK) {
        status = add_label(ui, "raw --", &raw, &card->raw);
    }
    if (status == BENTO_OK) {
        format_real(number, step_size(knob), STEP_DIGITS);
        (void)snprintf(text, sizeof(text), "1 ขั้น = %s %s", number,
                       knob->unit);
        status = add_label(ui, text, &step, &unused);
    }
    return status;
}

static bento_status_t build_knob_card(bento_ui_t *ui,
                                      const knob_t *knob,
                                      const rect_t *box,
                                      knob_card_t *card)
{
    const rect_t bar = {box->x + VALUE_DX, box->y + BAR_DY, BAR_W,
                        BAR_H};
    const rect_t ruler = {box->x + VALUE_DX, box->y + RULER_DY, BAR_W,
                          RULER_H};
    char title[TEXT_BYTES];
    bento_status_t status;

    (void)snprintf(title, sizeof(title), "%s (%s)", knob->name,
                   knob->unit);
    status = add_card(ui, box, title);
    if (status == BENTO_OK) {
        status = add_knob_labels(ui, knob, box, card);
    }
    if (status == BENTO_OK) {
        status = add_bar(ui, knob, &bar, &card->bar);
    }
    if (status == BENTO_OK) {
        status = add_ruler(ui, knob, &ruler);
    }
    card->raw_text[0] = '\0';
    return status;
}

/* ui.poll() here only keeps the screen drawn: its events are not
 * read, and a failed poll answers no events. */
static void poll_screen(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static bento_status_t build_cards(app_t *app)
{
    static const int32_t places[KNOB_COUNT][2] = {
        {CARD_LEFT_X, CARD_TOP_Y},
        {CARD_RIGHT_X, CARD_TOP_Y},
        {CARD_LEFT_X, CARD_BOTTOM_Y},
        {CARD_RIGHT_X, CARD_BOTTOM_Y}};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)KNOB_COUNT) && (status == BENTO_OK);
         i++) {
        const rect_t box = {places[i][0], places[i][1], CARD_W, CARD_H};

        status =
            build_knob_card(app->ui, &KNOBS[i], &box, &app->cards[i]);
    }
    return status;
}

static bento_status_t build_screen(app_t *app)
{
    const label_spec_t title = {MARGIN_X, TITLE_Y, FONT_TITLE,
                                COL_TEXT};
    const label_spec_t subtitle = {MARGIN_X, SUBTITLE_Y, FONT_TEXT,
                                   COL_DIM};
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    bento_ui_t *ui = app->ui;
    bento_ui_id_t unused;
    bento_status_t status =
        bento_ui_screen(ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status =
            add_label(ui, "ลูกบิด: เลขดิบ -> ค่าที่มีหน่วย", &title, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(ui,
                           "ค่า = ต่ำสุด + (raw - RAW_LO) x ช่วง / "
                           "(RAW_HI - RAW_LO) แล้ว clamp",
                           &subtitle, &unused);
    }
    if (status == BENTO_OK) {
        status = build_cards(app);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, " ", &help, &app->help);
    }
    if (status == BENTO_OK) {
        poll_screen(ui);
    }
    return status;
}

/* MicroPython's widget setters ignore a refusal (modui.c), so the
 * twin does too. */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

static void show_band(const app_t *app)
{
    char text[TEXT_BYTES];

    if (app->band != 0) {
        (void)snprintf(text, sizeof(text),
                       "%s (ล่าง) = เปิด/ปิดกันกะพริบ   ตอนนี้: %ld count",
                       BUTTON_NAMES[0], (long)app->band);
    } else {
        (void)snprintf(text, sizeof(text),
                       "%s (ล่าง) = เปิด/ปิดกันกะพริบ   ตอนนี้: ปิด",
                       BUTTON_NAMES[0]);
    }
    set_text(app->ui, app->help, text);
}

/* The raw label is written only when its text changes. */
static void show_raw(bento_ui_t *ui, knob_card_t *card, uint16_t raw,
                     int32_t skips)
{
    char text[RAW_TEXT_BYTES];

    (void)snprintf(text, sizeof(text), "raw %u  ข้าม %ld",
                   (unsigned int)raw, (long)skips);
    if (strcmp(text, card->raw_text) != 0) {
        (void)memcpy(card->raw_text, text, sizeof(text));
        set_text(ui, card->raw, text);
    }
}

static void show_value(bento_ui_t *ui, const knob_card_t *card,
                       const knob_t *knob, bento_real_t value)
{
    char number[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];

    format_real(number, value, knob->digits);
    (void)snprintf(text, sizeof(text), "%s %s", number, knob->unit);
    set_text(ui, card->value, text);
    /* the widget takes whole numbers only */
    (void)bento_ui_set_value(ui, card->bar, round_half_even(value));
}

/* Read, decide whether it moved enough, then show, for one knob. */
static bento_status_t update_knob(app_t *app, uint32_t i)
{
    knob_state_t *state = &app->knobs[i];
    knob_card_t *card = &app->cards[i];
    uint16_t raw = 0U;
    bento_status_t status = bento_pots_read(i, &raw);

    if (status == BENTO_OK) {
        if (moved(raw, state, app->band)) {
            state->shown = raw;
            state->shown_known = true;
            show_value(app->ui, card, &KNOBS[i],
                       to_unit(raw, &KNOBS[i]));
        } else if (!state->last_known || (raw != state->last)) {
            state->skips++; /* raw moved, the number did not */
        } else {
            /* the same raw as last round */
        }
        state->last = raw;
        state->last_known = true;
        show_raw(app->ui, card, raw, state->skips);
    }
    return status;
}

/* A press edge of SW5 switches the dead-band on or off. */
static bento_status_t check_button(app_t *app)
{
    bool down = false;
    bento_status_t status = bento_buttons_pressed((uint32_t)SW5, &down);

    if ((status == BENTO_OK) && down && !app->was_down) {
        app->band = (app->band != 0) ? 0 : DEADBAND;
        show_band(app);
        status =
            beep(app->ui, (app->band != 0) ? TUNE_START : TUNE_TAP);
    }
    app->was_down = down;
    return status;
}

/* Every TICK_MS: all four knobs, then the screen. */
static bento_status_t update_knobs(app_t *app)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)KNOB_COUNT) && (status == BENTO_OK);
         i++) {
        status = update_knob(app, i);
    }
    if (status == BENTO_OK) {
        poll_screen(app->ui);
    }
    return status;
}

static bento_status_t run_loop(app_t *app)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t shown_ms = start_ms;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        const uint32_t now_ms = bento_time_ticks_ms();

        status = check_button(app);
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, shown_ms) >= TICK_MS)) {
            shown_ms = now_ms;
            status = update_knobs(app);
        }
        if (status == BENTO_OK) {
            status = pause_ms((uint32_t)SAMPLE_MS);
        }
    }
    return status;
}

int main(void)
{
    app_t app;
    bento_status_t status;

    (void)memset(&app, 0, sizeof(app));
    app.ui = bento_runtime_get_ui();
    /* firmware 2.4.1 has no volume and refuses it; the .py skips it */
    (void)bento_ui_sfx_volume_set(app.ui, SPEAKER);
    status = build_screen(&app);
    if (status == BENTO_OK) {
        app.band = DEADBAND;
        show_band(&app);
        status = run_loop(&app);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(app.ui, app.help, COL_WARN);
        set_text(app.ui, app.help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        poll_screen(app.ui);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
