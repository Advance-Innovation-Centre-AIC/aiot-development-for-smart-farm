/*
 * cp2_06_ai_confidence_gate.c -- หลักการ 2.6: ให้ AI มีสิทธิ์ตอบว่า
 * "ไม่แน่ใจ" (ประตูความมั่นใจ)
 *
 * โมเดล AI ให้คะแนนทุกคลาส (0-1) คลาสที่คะแนนสูงสุดคือคำตอบ และคะแนนนั้นคือ
 * ความมั่นใจ ประตูจะลงมือเมื่อได้คำตอบเดียวกัน N_IN_ROW ครั้งติดกัน และทุก
 * ครั้งมั่นใจไม่น้อยกว่าเกณฑ์ ไม่ผ่านประตู = "ไม่แน่ใจ" นับเฉพาะคำตอบใหม่
 * (เลข seq เปลี่ยน) วางบอร์ดนิ่ง ถือบอร์ดวาดวงกลม หรือเขย่า ดู "AI ตอบดิบ"
 * เทียบกับ "ตัดสิน" หมุน VR1 ตั้งเกณฑ์ 0.50-0.99 สด ๆ แล้วดูตัวนับ
 * "ลงมือ" กับ "ไม่แน่ใจ" ใช้โมเดลในตัวที่หาด้วยชื่อ (MODEL_KEY)
 * โปรแกรมเล่น 3 นาทีแล้วจบเอง ใช้กับ TESAIoT Dev Kit และ BENTO Emulator
 *
 * Give the AI the right to answer "not sure": a confidence gate. The
 * model scores every class (0-1); the top class is its answer and
 * that score its confidence. The gate acts only after the same answer
 * N_IN_ROW times in a row, each at least as confident as the
 * threshold; anything else is "not sure". Only new answers (a new
 * seq) count. Keep the board still, draw a circle with it or shake it
 * and compare the raw answer with the gate's decision; turn VR1 to
 * set the threshold between 0.50 and 0.99 live. The built-in model is
 * found by name (MODEL_KEY). It runs for three minutes.
 *
 * Twin of cp2_06_ai_confidence_gate.py (aiot-development-for-smart-farm/core/cp2_06_ai_confidence_gate.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - edge_ai.models() builds a list of dicts; here the rows are read
 *     one by one, a built-in row being one the firmware compiled in,
 *     and any failure there means "not found", as the .py's except;
 *   - the result dict is a struct: the label is read by its class
 *     number, conf and scores are the engine's percent / 100;
 *   - None (no candidate, no decision) is the class number -1, and
 *     CONF_MIN = 0 means "use VR1", as None does in the .py;
 *   - edge_ai.start(i) and edge_ai.stop() wait inside the binding;
 *     here they are started and polled every 20 ms, so Stop ends a
 *     wait at once; a start that fails shows the same message, and
 *     the stop's failure is ignored, as the .py's except does;
 *   - a result that fails to arrive skips the round (the .py's
 *     OSError); a C program also closes the module's link at the end;
 *   - the settings MODEL_KEY and CONF_* live in the function that reads
 *     them (MISRA 8.9); a comment at the top says where;
 *   - round() rounds half to even, as Python's does;
 *   - hasattr(ui, "volume") is not asked: the volume call is always
 *     made, and a firmware without it refuses it, which is ignored;
 *   - ui.tone() and chart points are fire-and-forget in MicroPython,
 *     so their status is ignored here too;
 *   - Stop ends the program at the next sleep and main returns 0,
 *     without the closing line (MicroPython's KeyboardInterrupt);
 *   - the Edge AI link is opened after the screen is drawn, where the
 *     .py first asks edge_ai for its models.
 * In the BENTO Emulator, Edge AI from C is not available yet: the
 * program says so, then takes the .py's own "no model" path
 * ("ไม่พบโมเดล ...") and ends calmly, as the .py does. A board
 * without the model link (the reTerminal) takes the same path
 * without the Emulator note.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_edgeai.h"
#include "bento_pots.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* MODEL_KEY (the model's name, or part of it) is model_key in main(),
 * and CONF_MIN, CONF_LO and CONF_HI (the threshold) are in conf_min():
 * each lives in the one function that reads it (MISRA 8.9). */

/* Settings a student may change. */
enum {
    N_IN_ROW = 3,    /* the same confident answer this many times */
    MAX_CLASSES = 4, /* class scores shown (built-in models: 2-3) */
    ASK_MS = 100,    /* ask the AI this often (no new answer missed) */
    TICK_MS = 500,   /* refresh the screen this often */
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
    MODEL_Y = 38,
    GAP_X = 180, /* right of the title: one 609 px line fits */
    GAP_Y = 8,
    HELP_Y = 352,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    LEFT_CARD_X = 12,
    LEFT_CARD_W = 380,
    TOP_CARD_Y = 64,
    TOP_CARD_H = 150,
    INSET_X = 24,
    RAW_Y = 98,
    RAW_BAR_Y = 138,
    RAW_BAR_W = 356,
    RAW_BAR_H = 20,
    CONF_Y = 172,
    SCORE_CARD_X = 402,
    SCORE_CARD_W = 378,
    NAME_X = 414,
    NAME_Y = 100,
    SCORE_BAR_X = 530,
    SCORE_BAR_Y = 102,
    SCORE_BAR_W = 238,
    SCORE_BAR_H = 16,
    SCORE_ROW_H = 28,
    GATE_CARD_Y = 224,
    GATE_CARD_H = 114,
    LED_Y = 262,
    LED_SIZE = 40,
    DECISION_X = 78,
    DECISION_Y = 266,
    COUNTS_Y = 308,
    CHART_X = 422,
    LEGEND_Y = 224,
    CHART_Y = 252,
    CHART_W = 358,
    CHART_H = 86,
    CHART_MAX_W = 400,
    CHART_POINTS = 400,
    PERCENT = 100
};

/* Font sizes, in pixels: Thai text is 20 px or more on the board. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20,
    FONT_LATIN = 16, /* the class names, Latin only */
    FONT_BIG = 28
};

/* Sound, timing and sizes that are not settings. */
enum {
    SETTLE_MS = 200,   /* let the cleared screen settle */
    OP_POLL_MS = 20,   /* ask whether a start or stop is done */
    TONE_MS = 90,      /* each note of a tune */
    NOTE_GAP_MS = 100, /* one note to the next */
    TUNE_NOTES = 3,    /* the longest tune */
    TEXT_BYTES = 128,  /* a SET_TEXT carries at most 126 bytes */
    POT_FULL = 4095,   /* the knob's top raw value */
    NO_CLASS = -1      /* Python's None for a class */
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

/* The built-in model that was found. */
typedef struct {
    bool found;
    uint32_t index;
    uint32_t class_count;
    char name[BENTO_EDGEAI_NAME_LEN];
} model_t;

/* The gate: the candidate class, how many times in a row, the
 * decision (NO_CLASS = not sure), and the two counters. */
typedef struct {
    int32_t candidate;
    int32_t in_row;
    int32_t decision;
    int32_t acted;
    int32_t unsure;
} gate_t;

/* The widgets that change after the screen is drawn. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t model;
    bento_ui_id_t raw;
    bento_ui_id_t raw_bar;
    bento_ui_id_t conf;
    bento_ui_id_t names[MAX_CLASSES];
    bento_ui_id_t bars[MAX_CLASSES];
    bento_ui_id_t led;
    bento_ui_id_t decision;
    bento_ui_id_t counts;
    bento_ui_id_t chart;
    uint8_t min_series;
    bento_ui_id_t help;
} screen_t;

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

/* Python's round(): the nearest whole number, a half to the even
 * one. */
static int32_t py_round(float value)
{
    const float half = 0.5F;
    const float below = floorf(value);
    const float rest = value - below;
    int32_t whole = (int32_t)below;

    if ((rest > half) || ((rest == half) && ((whole % 2) != 0))) {
        whole++;
    }
    return whole;
}

/* 0-1 as a whole percent (the screen's widgets take only whole
 * numbers). */
static int32_t pct(float share)
{
    return py_round(share * (float)PERCENT);
}

/* The engine's percent as the 0-1 score the .py reads. */
static float score_of(uint8_t percent)
{
    return (float)percent / (float)PERCENT;
}

/* A capital letter's small one, by table (no arithmetic on
 * characters). */
static char lower_ascii(char letter)
{
    static const char capitals[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char smalls[] = "abcdefghijklmnopqrstuvwxyz";
    char lower = letter;
    size_t i;

    for (i = 0U; i < (sizeof(capitals) - 1U); i++) {
        if (capitals[i] == letter) {
            lower = smalls[i];
        }
    }
    return lower;
}

/* key.lower() in name.lower(), for the ASCII names models have. */
static bool contains_ignoring_case(const char *name, const char *key)
{
    const size_t name_length = strlen(name);
    const size_t key_length = strlen(key);
    bool found = false;
    size_t start;

    for (start = 0U; ((start + key_length) <= name_length) && !found;
         start++) {
        size_t i = 0U;

        while ((i < key_length) &&
               (lower_ascii(name[start + i]) == lower_ascii(key[i]))) {
            i++;
        }
        found = (i == key_length);
    }
    return found;
}

/* How many rows the firmware compiled in: those are built in. -1 when
 * the board cannot say (the .py's missing "builtin" key). */
static int32_t compiled_count(void)
{
    bento_edgeai_diag_t diag;
    int32_t count = -1;

    bento_edgeai_diag_init(&diag);
    if (bento_edgeai_diag(&diag) == BENTO_OK) {
        count = (int32_t)diag.compiled_models;
    }
    return count;
}

/* The first built-in model whose name holds the key. Any failure
 * leaves it not found: the .py's except counts an AI core that does
 * not answer as "no such model". */
static void find_model(const char *key, model_t *model)
{
    const int32_t compiled = compiled_count();
    bento_edgeai_model_info_t info;
    int32_t count = 0;
    int32_t i;

    model->found = false;
    if (bento_edgeai_count(&count) != BENTO_OK) {
        count = 0;
    }
    for (i = 0; (i < count) && !model->found; i++) {
        const bool built_in = (compiled < 0) || (i < compiled);

        bento_edgeai_model_info_init(&info);
        if ((bento_edgeai_model_info((uint32_t)i, &info) == BENTO_OK) &&
            built_in && contains_ignoring_case(info.name, key)) {
            model->found = true;
            model->index = (uint32_t)i;
            model->class_count = info.class_count;
            (void)memcpy(model->name, info.name, sizeof(model->name));
        }
    }
}

/* The threshold now: the fixed one, or VR1's raw 0-4095 turned to
 * CONF_LO..CONF_HI. */
static bento_status_t conf_min(float *threshold)
{
    /* CONF_MIN: 0 = set the threshold live with VR1; a number such as
     * 0.95 fixes it (VR1 then does nothing) */
    static const float conf_fixed = 0.0F;
    /* CONF_LO, CONF_HI: the range VR1 turns the threshold through */
    static const float conf_lo = 0.50F;
    static const float conf_hi = 0.99F;
    uint16_t raw = 0U;
    bento_status_t status = BENTO_OK;

    if (conf_fixed != 0.0F) {
        *threshold = conf_fixed;
    } else {
        status = bento_pots_read(0U, &raw);
        *threshold = conf_lo + (((float)raw * (conf_hi - conf_lo)) /
                                (float)POT_FULL);
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

/* Feeds one new answer: the decision is the class once the same class
 * was confident enough N_IN_ROW times in a row, else NO_CLASS. */
static void feed_gate(gate_t *gate, int32_t label, float conf,
                      float threshold)
{
    if ((label == NO_CLASS) || (conf < threshold)) {
        gate->candidate = NO_CLASS; /* not confident enough: restart */
        gate->in_row = 0;
    } else if (label == gate->candidate) {
        gate->in_row =
            (gate->in_row < N_IN_ROW) ? (gate->in_row + 1) : N_IN_ROW;
    } else {
        gate->candidate = label; /* a new answer starts its own count */
        gate->in_row = 1;
    }
    gate->decision =
        (gate->in_row >= N_IN_ROW) ? gate->candidate : NO_CLASS;
}

/* Waits for a started start or stop; Stop cancels it. */
static bento_status_t finish_op(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = pause_ms((uint32_t)OP_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_edgeai_op_poll(NULL);
        } else {
            (void)bento_edgeai_op_cancel(); /* Stop wins */
        }
    }
    return status;
}

/* Select the row and run it; loading may take up to 15 s. */
static bento_status_t start_model(uint32_t index)
{
    bento_status_t status = bento_edgeai_select_start(index);

    if (status == BENTO_OK) {
        status = finish_op();
    }
    return status;
}

/* Always free the AI core, even after a Stop; a stop that fails is
 * ignored, as the .py's except does. */
static void stop_model(void)
{
    if (bento_edgeai_stop_start() == BENTO_OK) {
        (void)finish_op(); /* its outcome changes nothing here */
    }
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
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

/* A 0-100 bar that starts at 0. */
static bento_status_t add_bar(bento_ui_t *ui, const rect_t *box,
                              bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = px(box->x);
    args.y = px(box->y);
    args.w = px(box->w);
    args.h = px(box->h);
    args.min_val = 0;
    args.max_val = PERCENT;
    args.font_px = 0;
    return bento_ui_bar_create(ui, &args, out_id);
}

/* The raw answer card: the label, its bar and the confidence line. */
static bento_status_t add_raw_card(screen_t *screen)
{
    const rect_t box = {LEFT_CARD_X, TOP_CARD_Y, LEFT_CARD_W,
                        TOP_CARD_H};
    const rect_t bar = {INSET_X, RAW_BAR_Y, RAW_BAR_W, RAW_BAR_H};
    const label_spec_t raw = {INSET_X, RAW_Y, FONT_BIG, COL_TEXT};
    const label_spec_t conf = {INSET_X, CONF_Y, FONT_TEXT, COL_DIM};
    bento_status_t status = add_card(screen->ui, &box, "AI ตอบดิบ");

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "-", &raw, &screen->raw);
    }
    if (status == BENTO_OK) {
        status = add_bar(screen->ui, &bar, &screen->raw_bar);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &conf, &screen->conf);
    }
    return status;
}

/* The score card: one name and one bar per class shown. */
static bento_status_t add_score_card(screen_t *screen)
{
    const rect_t box = {SCORE_CARD_X, TOP_CARD_Y, SCORE_CARD_W,
                        TOP_CARD_H};
    bento_status_t status = add_card(screen->ui, &box, "คะแนน");
    int32_t i;

    for (i = 0; (i < MAX_CLASSES) && (status == BENTO_OK); i++) {
        const label_spec_t name = {NAME_X, NAME_Y + (i * SCORE_ROW_H),
                                   FONT_LATIN, COL_TEXT};
        const rect_t bar = {SCORE_BAR_X,
                            SCORE_BAR_Y + (i * SCORE_ROW_H),
                            SCORE_BAR_W, SCORE_BAR_H};

        status = add_label(screen->ui, " ", &name, &screen->names[i]);
        if (status == BENTO_OK) {
            status = add_bar(screen->ui, &bar, &screen->bars[i]);
        }
    }
    return status;
}

static bento_status_t add_led(screen_t *screen)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.x = INSET_X;
    args.y = LED_Y;
    args.w = LED_SIZE;
    args.h = LED_SIZE;
    args.color = COL_OK;
    args.font_px = 0;
    return bento_ui_led_create(screen->ui, &args, &screen->led);
}

/* The gate card: the LED, the decision and the counters. */
static bento_status_t add_gate_card(screen_t *screen)
{
    const rect_t box = {LEFT_CARD_X, GATE_CARD_Y, LEFT_CARD_W,
                        GATE_CARD_H};
    const label_spec_t decision = {DECISION_X, DECISION_Y, FONT_BIG,
                                   COL_WARN};
    const label_spec_t counts = {INSET_X, COUNTS_Y, FONT_TEXT,
                                 COL_TEXT};
    bento_status_t status = add_card(screen->ui, &box, "ประตูตัดสิน");

    if (status == BENTO_OK) {
        status = add_led(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ไม่แน่ใจ", &decision,
                           &screen->decision);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &counts, &screen->counts);
    }
    return status;
}

/* A smooth line chart with no dots: LVGL draws no dots once the
 * points reach the chart's width, so it is at most 400 wide and has
 * 400 points (the firmware takes 10-400). */
static bento_status_t add_chart(screen_t *screen)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = CHART_X;
    args.y = CHART_Y;
    args.w = (CHART_W < CHART_MAX_W) ? CHART_W : CHART_MAX_W;
    args.h = CHART_H;
    args.color = COL_INFO;
    args.min_val = 0;
    args.max_val = PERCENT;
    status = bento_ui_chart_create(screen->ui, &args, &screen->chart);
    if (status == BENTO_OK) {
        /* a setter: MicroPython ignores its refusal too */
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = bento_ui_chart_add_series(
            screen->ui, screen->chart, COL_BAD, &screen->min_series);
    }
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

static bento_status_t build_screen(screen_t *screen)
{
    const label_spec_t title = {MARGIN_X, TITLE_Y, FONT_TITLE,
                                COL_TEXT};
    const label_spec_t model = {MARGIN_X, MODEL_Y, FONT_TEXT, COL_DIM};
    const label_spec_t legend = {CHART_X, LEGEND_Y, FONT_TEXT, COL_DIM};
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    bento_ui_t *ui = screen->ui;
    bento_ui_id_t unused;
    bento_status_t status =
        bento_ui_screen(ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "AI แน่ใจก่อนทำ", &title, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "โหลดโมเดล...", &model, &screen->model);
    }
    if (status == BENTO_OK) {
        status = add_raw_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_score_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_gate_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "ฟ้า มั่นใจ แดง เกณฑ์", &legend, &unused);
    }
    if (status == BENTO_OK) {
        status = add_chart(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(ui, "นิ่ง วงกลม เขย่า  VR1 = เกณฑ์", &help,
                           &screen->help);
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

/* The label of a class, or "" when the engine cannot name it. */
static void read_label(uint32_t index, int32_t class_number,
                       char label[BENTO_EDGEAI_LABEL_LEN])
{
    uint32_t length = 0U;

    if (bento_edgeai_label(index, (uint32_t)class_number, label,
                           BENTO_EDGEAI_LABEL_LEN,
                           &length) != BENTO_OK) {
        label[0] = '\0';
    }
}

/* The model's class names beside the score bars. */
static void show_names(const screen_t *screen, const model_t *model)
{
    char label[BENTO_EDGEAI_LABEL_LEN];
    int32_t i;

    for (i = 0; (i < (int32_t)model->class_count) && (i < MAX_CLASSES);
         i++) {
        read_label(model->index, i, label);
        set_text(screen->ui, screen->names[i], label);
    }
}

/* Top: the latest raw answer and every class's score. */
static void show_raw(const screen_t *screen,
                     const bento_edgeai_result_t *result, int32_t shown,
                     int32_t threshold)
{
    char label[BENTO_EDGEAI_LABEL_LEN];
    char text[TEXT_BYTES];
    uint32_t i;

    read_label(result->index, (int32_t)result->top, label);
    set_text(screen->ui, screen->raw, (label[0] != '\0') ? label : "-");
    (void)bento_ui_set_value(screen->ui, screen->raw_bar, shown);
    (void)snprintf(text, sizeof(text), "มั่นใจ %ld %%  เกณฑ์ %ld %%",
                   (long)shown, (long)threshold);
    set_text(screen->ui, screen->conf, text);
    for (i = 0U;
         (i < result->class_count) && (i < (uint32_t)MAX_CLASSES);
         i++) {
        (void)bento_ui_set_value(screen->ui, screen->bars[i],
                                 pct(score_of(result->score_pct[i])));
    }
    /* chart points are fire-and-forget in MicroPython too */
    (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U, shown);
    (void)bento_ui_chart_set_next(screen->ui, screen->chart,
                                  screen->min_series, threshold);
}

/* Bottom: the gate's decision and its counters. */
static void show_gate(const screen_t *screen, const model_t *model,
                      const gate_t *gate)
{
    const bool decided = gate->decision != NO_CLASS;
    char label[BENTO_EDGEAI_LABEL_LEN];
    char text[TEXT_BYTES];

    label[0] = '\0';
    if (decided) {
        read_label(model->index, gate->decision, label);
    }
    (void)bento_ui_set_value(screen->ui, screen->led, decided ? 1 : 0);
    set_text(screen->ui, screen->decision,
             (label[0] != '\0') ? label : "ไม่แน่ใจ");
    (void)bento_ui_set_color(screen->ui, screen->decision,
                             decided ? COL_OK : COL_WARN);
    (void)snprintf(text, sizeof(text),
                   "ติด %ld/%ld  ลงมือ %ld  ไม่แน่ใจ %ld",
                   (long)gate->in_row, (long)N_IN_ROW,
                   (long)gate->acted, (long)gate->unsure);
    set_text(screen->ui, screen->counts, text);
}

/* The state of one watch: the last answer, its seq and the gate. */
typedef struct {
    bento_edgeai_result_t result;
    bool has_result;
    gate_t gate;
    float threshold;
} watch_t;

/* A new answer: through the gate, counted, and a tune only when the
 * decision turns into an action. */
static bento_status_t take_answer(const screen_t *screen,
                                  watch_t *watch,
                                  const bento_edgeai_result_t *got)
{
    const int32_t was = watch->gate.decision;
    const int32_t label = (int32_t)got->top;
    bento_status_t status = BENTO_OK;

    watch->result = *got;
    watch->has_result = true;
    feed_gate(&watch->gate, label, score_of(got->score_pct[got->top]),
              watch->threshold);
    if (watch->gate.decision != NO_CLASS) {
        watch->gate.acted++;
    } else {
        watch->gate.unsure++;
    }
    if ((watch->gate.decision != NO_CLASS) &&
        (watch->gate.decision != was)) {
        status = beep(screen->ui, TUNE_START);
    }
    return status;
}

/* Asks for the latest answer; only a new seq goes through the gate.
 * A result that fails to arrive skips this round. */
static bento_status_t ask(const screen_t *screen, watch_t *watch)
{
    bento_edgeai_result_t got;
    bento_status_t status = BENTO_OK;

    bento_edgeai_result_init(&got);
    if ((bento_edgeai_result(&got) == BENTO_OK) && got.has_verdict &&
        (!watch->has_result || (got.seq != watch->result.seq))) {
        status = take_answer(screen, watch, &got);
    }
    return status;
}

/* Every TICK_MS: the threshold again, then the screen. */
static bento_status_t refresh(const screen_t *screen,
                              const model_t *model, watch_t *watch)
{
    bento_status_t status = conf_min(&watch->threshold);

    if ((status == BENTO_OK) && watch->has_result) {
        const bento_edgeai_result_t *result = &watch->result;

        show_raw(screen, result,
                 pct(score_of(result->score_pct[result->top])),
                 pct(watch->threshold));
        show_gate(screen, model, &watch->gate);
    }
    if (status == BENTO_OK) {
        poll_screen(screen->ui);
    }
    return status;
}

/* Asks every ASK_MS until RUN_MS, the screen every TICK_MS. */
static bento_status_t watch_model(const screen_t *screen,
                                  const model_t *model)
{
    watch_t watch;
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t shown_ms = start_ms;
    bento_status_t status;

    (void)memset(&watch, 0, sizeof(watch));
    watch.gate.candidate = NO_CLASS;
    watch.gate.decision = NO_CLASS;
    status = conf_min(&watch.threshold);
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        uint32_t now_ms;

        status = ask(screen, &watch);
        now_ms = bento_time_ticks_ms();
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, shown_ms) >= TICK_MS)) {
            shown_ms = now_ms;
            status = refresh(screen, model, &watch);
        }
        if (status == BENTO_OK) {
            status = pause_ms((uint32_t)ASK_MS);
        }
    }
    return status;
}

/* The round's end: the done tune and the closing line. */
static bento_status_t close_round(const screen_t *screen)
{
    const bento_status_t status = beep(screen->ui, TUNE_GOOD);

    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen->ui, screen->help, COL_WARN);
        set_text(screen->ui, screen->help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
    }
    return status;
}

/* Start the model, watch it, and close the round; a start that fails
 * (the AI core not ready) shows its message instead. */
static bento_status_t run_model(const screen_t *screen,
                                const model_t *model)
{
    char text[TEXT_BYTES];
    bento_status_t status = start_model(model->index);

    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "โมเดล: %s", model->name);
        set_text(screen->ui, screen->model, text);
        status = watch_model(screen, model);
        if (status == BENTO_OK) {
            status = close_round(screen);
        }
    } else if (status != BENTO_E_CANCELLED) {
        (void)bento_ui_set_color(screen->ui, screen->model, COL_BAD);
        set_text(screen->ui, screen->model, "เริ่มโมเดลไม่ได้ กด RESET");
        status = BENTO_OK;
    } else {
        /* Stop while the model loads */
    }
    return status;
}

/* The class names, the run, and the AI core freed whatever ended
 * it. */
static bento_status_t run_found(const screen_t *screen,
                                const model_t *model)
{
    bento_status_t status;

    show_names(screen, model);
    poll_screen(screen->ui);
    status = run_model(screen, model);
    stop_model();
    if (status == BENTO_OK) {
        poll_screen(screen->ui);
    }
    return status;
}

/* The BENTO Emulator's C runtime has no Edge AI yet: say so on the
 * screen and on the console. */
static bento_status_t show_emulator_gap(bento_ui_t *ui)
{
    static const char gap_text[] =
        "Edge AI จาก C ยังไม่มีใน BENTO "
        "Emulator ให้รันบน Dev Kit หรือ Eva Kit";
    const label_spec_t spec = {GAP_X, GAP_Y, FONT_TEXT, COL_WARN};
    bento_ui_id_t unused;

    (void)printf(
        "%s / Edge AI from C is not in the BENTO Emulator yet: "
        "run it on a Dev Kit or Eva Kit.\n",
        gap_text);
    return add_label(ui, gap_text, &spec, &unused);
}

/* Opens the Edge AI link and looks for the model. Any failure here
 * means "not found", as the .py's find_model catches every error;
 * only the BENTO Emulator adds its note. */
static bento_status_t look_for_model(const screen_t *screen,
                                     const char *key, model_t *model)
{
    const bento_status_t link = bento_edgeai_init();
    bool has_link = false;
    bento_status_t status = BENTO_OK;

    model->found = false;
    if (link == BENTO_OK) {
        find_model(key, model);
    } else if ((link == BENTO_E_UNSUPPORTED) &&
               (bento_edgeai_supported(&has_link) ==
                BENTO_E_UNSUPPORTED)) {
        /* a reTerminal refuses init too but answers this query; only
         * the BENTO Emulator's C runtime refuses both */
        status = show_emulator_gap(screen->ui);
    } else {
        /* no link or a failed init: the model is not found */
    }
    return status;
}

/* No built-in model of this name: say so and end calmly. */
static void show_not_found(const screen_t *screen, const char *key)
{
    char text[TEXT_BYTES];

    (void)bento_ui_set_color(screen->ui, screen->model, COL_BAD);
    (void)snprintf(text, sizeof(text), "ไม่พบโมเดล %s", key);
    set_text(screen->ui, screen->model, text);
    poll_screen(screen->ui);
}

int main(void)
{
    screen_t screen;
    model_t model;
    /* MODEL_KEY: the name (or part of it) of the built-in model; try
     * "Cough", "Alarm", "Siren" */
    static const char model_key[] = "Motion";
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&model, 0, sizeof(model));
    screen.ui = bento_runtime_get_ui();
    /* firmware 2.4.1 has no volume and refuses it; the .py skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = look_for_model(&screen, model_key, &model);
    }
    if ((status == BENTO_OK) && model.found) {
        status = run_found(&screen, &model);
    } else if (status == BENTO_OK) {
        show_not_found(&screen, model_key);
    } else {
        /* the status chain ended earlier */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
