/*
 * cp1_02_practise.c -- แบบฝึกเติมโค้ด (Code Quest ระดับ 3 โบนัส):
 * แปลงช่วงและจำกัดขอบ
 *
 * ไฟล์นี้เหมือน cp1_02_knob_scaling ทุกอย่าง ยกเว้น map_range() และ
 * clamp() ในส่วน "3) สมอง" ที่เว้นช่องไว้ 4 ช่อง: A, B (ใน map_range)
 * และ C, D (ใน clamp) แต่ละช่องเขียนว่า BLANK เติมคำตอบแทน BLANK ให้ครบ
 * แล้วรัน โปรแกรมตรวจ 6 กรณีก่อนเปิดจอ (self_test) ผ่านครบ = Console
 * ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง ยังไม่ถูก = Console บอกว่า
 * กรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ) โจทย์โบนัส ไม่มีเฉลยในโฟลเดอร์นี้
 * ใช้กับ TESAIoT Dev Kit และ BENTO Emulator
 *
 * Fill-in practice (Code Quest level 3, bonus): map a range and clamp
 * it. The program is cp1_02_knob_scaling except map_range() and
 * clamp(), which leave four blanks: A and B in map_range, C and D in
 * clamp, each written BLANK. Fill them in and run: self_test checks 6
 * cases before the screen opens. All pass = "ผ่าน!" on the console
 * and the knob cards run as in the example; a wrong case is printed
 * and the program stops before the screen.
 *
 * Twin of cp1_02_practise.py (aiot-development-for-smart-farm/core/practise/cp1_02_practise.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - C cannot leave a name undefined and still build: each blank
 *     ____ is BLANK (0) here, so an unfilled file builds, self_test
 *     names the first wrong case and stops before the screen, as the
 *     .py stops at its NameError;
 *   - map_range() answers false where the .py raises
 *     ZeroDivisionError (an input range with no width): self_test
 *     shows it as None, as the .py does, and in the knob loop it
 *     ends the program with BENTO_E_INVALID_ARGUMENT;
 *   - a case prints its numbers as the .py's tuple does: a whole
 *     wanted value without ".0", and a clamp result (always one of
 *     its whole-number arguments there) as a whole number;
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
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_pots.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    /* raw ต้องขยับเกินกี่ count ตัวเลขบนจอจึงยอมเปลี่ยน */
    DEADBAND = 20,
    RAW_LO = 0,      /* raw ที่ปลายซ้ายสุดของลูกบิด */
    RAW_HI = 4095,   /* raw ที่ปลายขวาสุดของลูกบิด */
    SAMPLE_MS = 20,  /* อ่านปุ่มทุกกี่ ms (เฟิร์มแวร์กรองสั่น 50 ms) */
    TICK_MS = 500,   /* อ่านลูกบิดและอัปเดตจอทุกกี่ ms */
    RUN_MS = 180000, /* เล่นนาน 3 นาทีแล้วจบเอง */
    SPEAKER = 40,    /* ความดังลำโพงรวม 0-100% (firmware 2.4.2+) */
    VOLUME = 25      /* ความดังเสียง 0-127 (ราว 20%) */
};
/* .py: BTN_NAMES. ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(1),
 * SW6 = ปุ่มบน = pressed(0) */
static const char *const BUTTON_NAMES[2] = {"SW5", "SW6"};
/* ---- end of settings ---- */

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
    TENTHS_PER_UNIT = 10,
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
        {76U, 0U, 0U},   {72U, 79U, 0U}, {79U, 72U, 0U},
        {72U, 79U, 84U}, {84U, 76U, 0U}, {84U, 76U, 69U},
        {88U, 0U, 0U}};
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

/* ---- 3) The brain: decides, never touches the hardware ---- */

/* Each blank ____ of the .py: write your answer in place of BLANK. */
enum {
    BLANK = 0
};

/* Straight-line map: in_low -> out_low, in_high -> out_high, the
 * values between in the same proportion. Answers false where the .py
 * raises ZeroDivisionError: the input range has no width. */
static bool map_range(int32_t x, int32_t in_low, int32_t in_high,
                      int32_t out_low, int32_t out_high,
                      bento_real_t *result)
{
    /* ช่อง A: x เดินมาจาก "จุดไหน" ของช่วงเข้า? */
    const int32_t walked = x - BLANK;
    /* ช่อง B: ความกว้างช่วงเข้า = อะไร - in_low ? */
    const int32_t width = BLANK - in_low;
    const bool has_width = width != 0;
    const int32_t scaled = walked * (out_high - out_low);

    /* in_high is read once blank B is filled */
    (void)in_high;
    *result = (bento_real_t)0;
    if (has_width) {
        *result = (bento_real_t)out_low +
                  ((bento_real_t)scaled / (bento_real_t)width);
    }
    return has_width;
}

/* A value outside low..high is cut to the nearer end. */
static bento_real_t clamp(bento_real_t value, int32_t low, int32_t high)
{
    bento_real_t result = (bento_real_t)low;

    if (value < (bento_real_t)low) {
        result = (bento_real_t)low;
    } else if (value > (bento_real_t)BLANK) {
        /* ช่อง C: value มากกว่า "อะไร" จึงต้องตัด? */
        result = (bento_real_t)high;
    } else {
        /* ช่อง D: อยู่ในช่วงแล้ว ควรคืนค่าอะไร? */
        result = (bento_real_t)BLANK;
    }
    return result;
}

typedef enum {
    CHECK_MAP_RANGE,
    CHECK_CLAMP
} check_kind_t;

/* A case's arguments, in map_range's order; clamp takes the first
 * three as value, low and high. */
enum {
    ARG_VALUE,
    ARG_IN_LOW,
    ARG_IN_HIGH,
    ARG_OUT_LOW,
    ARG_OUT_HIGH,
    MAP_ARGS
};

/* One case: the function, its arguments and the wanted value in
 * tenths. */
typedef struct {
    check_kind_t kind;
    int32_t args[MAP_ARGS];
    int32_t want_tenths;
} range_case_t;

/* A number as the .py's print shows it: 30, 27.5. */
static void format_tenths(char *text, uint32_t size, int32_t tenths)
{
    if ((tenths % TENTHS_PER_UNIT) == 0) {
        (void)snprintf(text, size, "%" PRId32,
                       tenths / TENTHS_PER_UNIT);
    } else {
        (void)snprintf(text, size, "%" PRId32 ".%" PRId32,
                       tenths / TENTHS_PER_UNIT,
                       tenths % TENTHS_PER_UNIT);
    }
}

/* Runs one case; *known is false where the .py's got is None. */
static bento_real_t run_case(const range_case_t *test, bool *known)
{
    const int32_t *arg = test->args;
    bento_real_t got = (bento_real_t)0;

    if (test->kind == CHECK_MAP_RANGE) {
        *known = map_range(arg[0], arg[1], arg[2], arg[ARG_OUT_LOW],
                           arg[ARG_OUT_HIGH], &got);
    } else {
        *known = true;
        got = clamp((bento_real_t)arg[0], arg[1], arg[2]);
    }
    return got;
}

/* What the case got, as the .py prints it. */
static void format_got(char *text, const range_case_t *test, bool known,
                       bento_real_t got)
{
    uint32_t length = 0U;

    if (!known) {
        (void)snprintf(text, BENTO_FMT_BUF_SIZE, "None");
    } else if (test->kind == CHECK_MAP_RANGE) {
        if (bento_fmt_real(text, BENTO_FMT_BUF_SIZE, got, &length) !=
            BENTO_OK) {
            text[0] = '\0';
        }
    } else {
        /* clamp gave back one of its whole-number arguments */
        (void)snprintf(text, BENTO_FMT_BUF_SIZE, "%" PRId32,
                       round_half_even(got));
    }
}

static void print_case(const range_case_t *test, bool known,
                       bento_real_t got)
{
    const int32_t *arg = test->args;
    const bool is_map = test->kind == CHECK_MAP_RANGE;
    char args[TEXT_BYTES];
    char want[BENTO_FMT_BUF_SIZE];
    char got_text[BENTO_FMT_BUF_SIZE];

    if (is_map) {
        (void)snprintf(args, sizeof(args),
                       "(%" PRId32 ", %" PRId32 ", %" PRId32
                       ", %" PRId32 ", %" PRId32 ")",
                       arg[0], arg[1], arg[2], arg[ARG_OUT_LOW],
                       arg[ARG_OUT_HIGH]);
    } else {
        (void)snprintf(args, sizeof(args),
                       "(%" PRId32 ", %" PRId32 ", %" PRId32 ")",
                       arg[0], arg[1], arg[2]);
    }
    format_tenths(want, sizeof(want), test->want_tenths);
    format_got(got_text, test, known, got);
    (void)printf("ยังไม่ถูก: %s %s ควรได้ %s แต่ได้ %s\n",
                 is_map ? "map_range" : "clamp", args, want, got_text);
}

/* Checks map_range() and clamp() on 6 cases before the screen. */
static bool self_test(void)
{
    static const range_case_t cases[] = {
        {CHECK_MAP_RANGE, {0, 0, 4095, 0, 100}, 0},
        {CHECK_MAP_RANGE, {4095, 0, 4095, 0, 500}, 5000},
        /* the input range does not start at 0 */
        {CHECK_MAP_RANGE, {1500, 1000, 2000, 0, 60}, 300},
        /* the output range does not start at 0 */
        {CHECK_MAP_RANGE, {2000, 0, 4000, 15, 40}, 275},
        {CHECK_CLAMP, {120, 0, 100, 0, 0}, 1000},
        {CHECK_CLAMP, {42, 0, 100, 0, 0}, 420}};
    const bento_real_t tolerance = (bento_real_t)0.01;
    const uint32_t count = (uint32_t)(sizeof(cases) / sizeof(cases[0]));
    bool passed = true;
    uint32_t i;

    for (i = 0U; passed && (i < count); i++) {
        bool known = false;
        const bento_real_t got = run_case(&cases[i], &known);
        const bento_real_t miss =
            got - ((bento_real_t)cases[i].want_tenths /
                   (bento_real_t)TENTHS_PER_UNIT);

        if (!known || (miss > tolerance) || (miss < -tolerance)) {
            print_case(&cases[i], known, got);
            passed = false;
        }
    }
    if (passed) {
        (void)printf("ผ่าน! map_range และ clamp ถูกทั้ง 6 กรณี\n");
    }
    return passed;
}

/* The knob's value with its unit. An input range with no width ends
 * the program, as the .py's ZeroDivisionError does. */
static bento_status_t to_unit(uint16_t raw, const knob_t *knob,
                              bento_real_t *value)
{
    bento_real_t mapped = (bento_real_t)0;
    const bento_status_t status =
        map_range((int32_t)raw, RAW_LO, RAW_HI, knob->low, knob->high,
                  &mapped)
            ? BENTO_OK
            : BENTO_E_INVALID_ARGUMENT;

    *value = clamp(mapped, knob->low, knob->high);
    return status;
}

/* How far the value moves for one raw count. */
static bento_real_t step_size(const knob_t *knob)
{
    const int32_t span = knob->high - knob->low;
    const int32_t raw_span = RAW_HI - RAW_LO;

    return (bento_real_t)span / (bento_real_t)raw_span;
}

/* The dead-band: change the number only when raw is more than band
 * away from the raw count the number comes from. */
static bool moved(uint16_t raw, const knob_state_t *state, int32_t band)
{
    const int32_t distance = (int32_t)raw - (int32_t)state->shown;

    return !state->shown_known ||
           (((distance < 0) ? -distance : distance) > band);
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

/* Converts the raw count and shows it on the card. */
static bento_status_t show_unit(bento_ui_t *ui, const knob_card_t *card,
                                const knob_t *knob, uint16_t raw)
{
    bento_real_t value = (bento_real_t)0;
    const bento_status_t status = to_unit(raw, knob, &value);

    if (status == BENTO_OK) {
        show_value(ui, card, knob, value);
    }
    return status;
}

/* Read, decide whether it moved enough, then show, for one knob. */
static bento_status_t update_knob(app_t *app, uint32_t i)
{
    knob_state_t *state = &app->knobs[i];
    knob_card_t *card = &app->cards[i];
    uint16_t raw = 0U;
    bento_status_t status = bento_pots_read(i, &raw);

    if ((status == BENTO_OK) && moved(raw, state, app->band)) {
        state->shown = raw;
        state->shown_known = true;
        status = show_unit(app->ui, card, &KNOBS[i], raw);
    } else if (status == BENTO_OK) {
        if (!state->last_known || (raw != state->last)) {
            state->skips++; /* raw moved, the number did not */
        } else {
            /* the same raw as last round */
        }
    } else {
        /* a failed read ends the program */
    }
    if (status == BENTO_OK) {
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
    /* map_range() and clamp() are checked first; a failed case ends
     * the program before the screen opens */
    status = self_test() ? build_screen(&app) : BENTO_E_CANCELLED;
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
