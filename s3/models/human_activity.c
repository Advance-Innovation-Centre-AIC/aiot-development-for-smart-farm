/*
 * human_activity.c -- Activity Timeline: เวลาที่ใช้ในแต่ละกิจกรรม
 * (โมเดลจาก Edge AI Store)
 *
 * ใช้โมเดล "HumanActivity" ที่ติดตั้งจาก Edge AI Store บน TESAIoT Dev
 * Kit อ่านการเคลื่อนไหวจาก IMU ติดบอร์ดไว้ที่หน้าอก การ์ด "Now" บอก
 * กิจกรรมที่ยืนยันแล้ว (ยืน วิ่ง เดิน นั่ง กระโดด) กิจกรรมหนึ่งนับเมื่อ
 * อยู่อันดับหนึ่งนานพอ (Min) การ์ด "Time per activity" รวมเวลาของแต่ละ
 * กิจกรรม แตะปุ่ม "Min" หรือกด SW6 (ปุ่มบน) เลือกเวลาขั้นต่ำ 0, 5 หรือ
 * 10 วินาที SW5 (ปุ่มล่าง) หยุดชั่วคราว/ทำต่อ
 *
 * Runs the model "HumanActivity" deployed from the Edge AI Store on
 * the TESAIoT Dev Kit, reading the IMU with the board worn on the
 * chest. The "Now" card shows the accepted activity (standing,
 * running, walking, sitting, jumping); an activity counts once it
 * has stayed on top for the minimum time. The "Time per activity"
 * card adds up the time spent in each one. Tap "Min" or press SW6
 * (upper) to choose the minimum time: 0, 5 or 10 s. SW5 (lower) =
 * pause/resume.
 *
 * Twin of human_activity.py (aiot-development-for-smart-farm/s3/models/human_activity.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - edge_ai.models() is read row by row (count, model_info, label);
 *     a row is built in when the firmware compiled it in
 *     (diag.compiled_models), unknown (the .py's None) when diag
 *     cannot say. A label that cannot be read is "".
 *   - The OSError texts printed are the binding's own words for the
 *     C status ("edge_ai: model link timeout", ...).
 *   - edge_ai.select() and edge_ai.stop() wait inside the binding;
 *     here they are started and polled every 20 ms, so Stop ends a
 *     wait at once.
 *   - class Stop is a text kept in the program's state and a failed
 *     status; main() shows and prints it as the except does.
 *   - `import buttons` fails on the Eva Kit: here that board has
 *     bento_buttons_count() 0. `import edge_ai` fails where there is
 *     no model link: here bento_edgeai_supported() false ends the
 *     program with BENTO_E_UNSUPPORTED.
 *   - gc.collect() and gc.mem_free(): C has no garbage collector, so
 *     the "free heap:" line is not printed.
 *   - The dict of texts, colours and values already sent is one entry
 *     per widget slot; a text is kept up to TEXT_CHARS - 1 bytes.
 *   - Not sure, the .py's index -1 of its lists, is the class number
 *     after the model's last class.
 *   - A score is the board's whole percent: int(score * 100 + 0.5)
 *     gives it back unchanged, so the percent is used as it comes.
 *   - The unused settings K, N, REFRACT_MS and HOLD_MS and the unused
 *     alarm states are left out.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_edgeai.h"
#include "bento_fmt.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- 1) Settings: edit before running ---- */
static const char MODEL_NAME[] = "HumanActivity"; /* exact name */
/* BUILTIN (false = deployed from the Store) lives in source_matches() */

enum {
    DEFAULT_CONFIRM = 65, /* Confirm at, when the board cannot say */
    POLL_MS = 100,
    DRAW_MS = 500,
    DWELL_COUNT = 3,
    DWELL_START = 1, /* Min 5 s */
    CHIP_COUNT = 5
};

/* ms on top before an activity counts */
static const int32_t DWELLS[DWELL_COUNT] = {0, 5000, 10000};
/* the classes this example needs; unlabeled = none of them */
static const char *const CHIPS[CHIP_COUNT] = {
    "standing", "running", "walking", "sitting", "jumping"};

/* Colours, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_CARD = 0x171B22,
    COLOR_BUTTON = 0x5E6878,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* Times, counts and sizes. */
enum {
    SETTLE_MS = 150, /* let the cleared screen settle */
    LOOP_MS = 20,
    OP_POLL_MS = 20,  /* ask a pending select or stop this often */
    SILENT_MS = 3000, /* result() failing this long = Stop */
    MS_PER_S = 1000,
    S_PER_MIN = 60,
    PERCENT = 100,
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    NOT_SURE = -1,    /* no class on top at Confirm at */
    NO_CHIP = -1,     /* a class that is none of the chips */
    TEXT_CHARS = 128, /* a SET_TEXT carries at most 126 bytes */
    CLOCK_CHARS = 36, /* "HH:MM:SS" of any three int32_t */
    PAUSE_BUTTON = 1, /* SW5, the lower button */
    DWELL_BUTTON = 0  /* SW6, the upper button */
};

/* Layout on the 792 x 398 canvas. */
enum {
    AUTO = -1, /* x, y, w or h the .py leaves out */
    LABEL_PX = 16,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 14,
    CARD_TITLE_DY = 8,
    CHIP_X = 28,
    CHIP_STEP_PX = 88,
    CHIP_TEXT_DX = 5,
    ROW_X = 508,
    ROW_Y = 96,
    ROW_BAR_Y = 116,
    ROW_STEP_PX = 34
};

/* Every widget the program changes later has a slot. */
enum {
    SLOT_HEARTBEAT = 0,
    SLOT_VERDICT = 1,
    SLOT_CHIP_PANELS = 2, /* CHIP_COUNT slots */
    SLOT_CHIP_TEXTS = SLOT_CHIP_PANELS + (int32_t)CHIP_COUNT,
    SLOT_BAR = SLOT_CHIP_TEXTS + (int32_t)CHIP_COUNT,
    SLOT_CANDIDATE = SLOT_BAR + 1,
    SLOT_ROWS = SLOT_CANDIDATE + 1, /* CHIP_COUNT slots */
    SLOT_ROW_BARS = SLOT_ROWS + (int32_t)CHIP_COUNT,
    SLOT_DWELL = SLOT_ROW_BARS + (int32_t)CHIP_COUNT,
    SLOT_STATUS = SLOT_DWELL + 1,
    SLOT_COUNT = SLOT_STATUS + 1,
    SLOT_NONE = SLOT_COUNT /* a widget nobody changes again */
};

/* One ui.<Widget>(...) call of the .py: the kind, the slot that
 * keeps its id, its text and the keywords x, y, w, h, color, min,
 * max and value. */
typedef struct {
    uint8_t kind;
    uint8_t slot;
    const char *text;
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    uint32_t color;
    int32_t minimum;
    int32_t maximum;
    int32_t value;
} widget_spec_t;

/* What was last sent to one widget: send only what changed, a full
 * display queue drops commands. */
typedef struct {
    bool has_text;
    bool has_color;
    bool has_value;
    uint32_t color;
    int32_t value;
    char text[TEXT_CHARS];
} sent_t;

/* The newest result read, and since when result() fails. */
typedef struct {
    bool has_sequence;
    uint32_t sequence;
    bool failing;
    uint32_t failing_since_ms;
} reader_t;

/* The program's state: too big for the 8 KB stack. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT + 1]; /* + the spare for SLOT_NONE */
    sent_t sent[SLOT_COUNT];
    bool has_buttons;
    uint32_t model_index;
    int32_t model_count; /* rows when the model was found */
    uint32_t label_count;
    char labels[MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
    int32_t chip_of[MAX_CLASSES]; /* the chip of a class, or NO_CHIP */
    reader_t reader;
    char stop_text[TEXT_CHARS]; /* "" until the program stops itself */
} app_t;

/* One board button: one event per press, holding does not repeat. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} board_button_t;

/* A class counts after the dwell on top at Confirm at; NOT_SURE is
 * no class. */
typedef struct {
    int32_t accepted;
    int32_t candidate;
    uint32_t since_ms;
    int32_t count;
} dwell_t;

/* The top class of a new result: its number, percent and Confirm
 * at. */
typedef struct {
    bool fresh; /* false: no new result since the last draw */
    uint32_t klass;
    int32_t percent;
    int32_t confirm;
} top_t;

/* The loop's state. */
typedef struct {
    bento_edgeai_thresholds_t thresholds;
    board_button_t pause_button;
    board_button_t dwell_button;
    dwell_t dwell;
    int32_t dwell_choice;
    int32_t spent_ms[CHIP_COUNT]; /* time in each accepted activity */
    bool paused;
    bool blink;
    top_t top;
    uint32_t drawn_ms;
    uint32_t polled_ms;
} run_t;

/* Too big for the 8 KB program stack: file scope, zeroed by the
 * loader on every load, and set again at the start of main()
 * (CLEAN_C.md C9). */
extern app_t g_app;
app_t g_app;

/* ---- 2) Board buttons and the AI core ---- */

/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t wait_ms(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* The program stops itself with a message on screen. */
static bento_status_t raise_stop(app_t *app, const char *text)
{
    (void)snprintf(app->stop_text, sizeof(app->stop_text), "%s", text);
    return BENTO_E_FAILED;
}

/* Reads a board button and remembers a new press (a rising edge).
 * A board without buttons reads nothing. */
static bento_status_t sample_button(const app_t *app,
                                    board_button_t *button)
{
    bool pressed = false;
    bento_status_t status = BENTO_OK;

    if (app->has_buttons) {
        status = bento_buttons_pressed(button->index, &pressed);
        if ((status == BENTO_OK) && pressed && !button->down) {
            button->clicked = true;
        }
        button->down = pressed;
    }
    return status;
}

/* Was it pressed since the last question? Asking clears it. */
static bool pressed_now(board_button_t *button)
{
    const bool fired = button->clicked;

    button->clicked = false;
    return fired;
}

/* str() of the OSError the binding raises for a failed query. */
static const char *link_error_text(bento_status_t status)
{
    const char *text = "edge_ai: model link init failed";

    if (status == BENTO_E_TIMEOUT) {
        text = "edge_ai: model link timeout";
    } else if (status == BENTO_E_NOT_READY) {
        text = "edge_ai: engine unavailable";
    } else if (status == BENTO_E_UNSUPPORTED) {
        text = "edge_ai: this firmware does not answer that query";
    } else {
        /* the link did not come up */
    }
    return text;
}

/* How many rows the firmware compiled in: those are built in.
 * -1 when the board cannot say (the binding's None). */
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

/* Built in or Store as builtin asks; a row the board cannot place
 * (None on the emulator) counts as built in. */
static bool source_matches(int32_t row, int32_t compiled)
{
    static const bool builtin =
        false; /* false = deployed from the Store */
    bool matches;

    if (builtin) {
        matches = (compiled < 0) || (row < compiled);
    } else {
        matches = (compiled >= 0) && (row >= compiled);
    }
    return matches;
}

/* The chip a class name stands for, or NO_CHIP. */
static int32_t chip_index(const char *name)
{
    int32_t chip = NO_CHIP;
    int32_t j;

    for (j = CHIP_COUNT - 1; j >= 0; j--) {
        if (strcmp(name, CHIPS[j]) == 0) {
            chip = j;
        }
    }
    return chip;
}

/* The model's class names, "" for one that cannot be read. */
static void read_labels(app_t *app,
                        const bento_edgeai_model_info_t *info)
{
    uint32_t length = 0U;
    uint32_t i;

    app->label_count = info->class_count;
    if (app->label_count > (uint32_t)MAX_CLASSES) {
        app->label_count = MAX_CLASSES;
    }
    for (i = 0U; i < app->label_count; i++) {
        if (bento_edgeai_label(app->model_index, i, app->labels[i],
                               BENTO_EDGEAI_LABEL_LEN,
                               &length) != BENTO_OK) {
            app->labels[i][0] = '\0';
        }
        app->chip_of[i] = chip_index(app->labels[i]);
    }
}

/* Every row, as edge_ai.models() reads them; the first one with the
 * exact name and the right source is the model. */
static bento_status_t read_rows(app_t *app, bool *found)
{
    bento_edgeai_model_info_t info;
    int32_t count = 0;
    int32_t row;
    bento_status_t status = bento_edgeai_count(&count);
    const int32_t compiled =
        (status == BENTO_OK) ? compiled_count() : -1;

    for (row = 0; (row < count) && (status == BENTO_OK); row++) {
        bento_edgeai_model_info_init(&info);
        status = bento_edgeai_model_info((uint32_t)row, &info);
        if ((status == BENTO_OK) && !*found &&
            (strcmp(info.name, MODEL_NAME) == 0) &&
            source_matches(row, compiled)) {
            *found = true;
            app->model_index = (uint32_t)row;
            read_labels(app, &info);
        }
    }
    app->model_count = count;
    return status;
}

/* find_model(): a link that fails prints why, and then there is no
 * model. */
static bool find_model(app_t *app)
{
    bool found = false;
    const bento_status_t status = read_rows(app, &found);

    if (status != BENTO_OK) {
        (void)printf("cannot read models: %s\n",
                     link_error_text(status));
        found = false;
    }
    return found;
}

/* Waits for a started select or stop; Stop cancels it. */
static bento_status_t finish_op(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = wait_ms(OP_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_edgeai_op_poll(NULL);
        } else {
            (void)bento_edgeai_op_cancel(); /* Stop wins */
        }
    }
    return status;
}

/* stop_ai(): an OSError is passed over; only Stop goes on. */
static bento_status_t stop_ai(void)
{
    bento_status_t status = bento_edgeai_stop_start();

    if (status == BENTO_OK) {
        status = finish_op();
    }
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

/* Per class: Confirm at; the default for every class when the board
 * cannot say. */
static void read_thresholds(uint32_t index,
                            bento_edgeai_thresholds_t *thresholds)
{
    uint32_t i;

    bento_edgeai_thresholds_init(thresholds);
    if (bento_edgeai_thresholds(index, thresholds) != BENTO_OK) {
        for (i = 0U; i < (uint32_t)MAX_CLASSES; i++) {
            thresholds->confirm[i] = DEFAULT_CONFIRM;
        }
    }
}

/* Only NEW results of this model; Stop after SILENT_MS of errors.
 * *fresh: the result is new. */
static bento_status_t
read_result(app_t *app, bento_edgeai_result_t *result, bool *fresh)
{
    reader_t *reader = &app->reader;
    bento_status_t status = BENTO_OK;

    *fresh = false;
    bento_edgeai_result_init(result);
    if (bento_edgeai_result(result) != BENTO_OK) {
        const uint32_t now_ms = bento_time_ticks_ms();

        if (!reader->failing) {
            reader->failing = true;
            reader->failing_since_ms = now_ms;
        }
        if (bento_time_ticks_diff(now_ms, reader->failing_since_ms) >=
            SILENT_MS) {
            status =
                raise_stop(app, "AI core not answering - run again");
        }
    } else {
        reader->failing = false;
        *fresh = result->has_verdict &&
                 (!reader->has_sequence ||
                  (result->seq != reader->sequence)) &&
                 (result->index == app->model_index);
        if (*fresh) {
            reader->has_sequence = true;
            reader->sequence = result->seq;
        }
    }
    return status;
}

/* ---- 3) Screen helpers ---- */

/* The time now as HH:MM:SS. A refused read leaves 00:00:00. */
static void clock_text(char *text, size_t cap)
{
    bento_time_tm_t now;

    bento_time_tm_init(&now);
    if (bento_time_localtime_now(&now) != BENTO_OK) {
        bento_time_tm_init(&now);
    }
    (void)snprintf(text, cap, "%02" PRId32 ":%02" PRId32 ":%02" PRId32,
                   now.hour, now.min, now.sec);
}

/* The widget setters ignore a refusal in MicroPython (modui.c), so
 * they do here too. */
static void set_text(const app_t *app, uint32_t slot, const char *text)
{
    (void)bento_ui_set_text(app->ui, app->ids[slot], text,
                            (uint32_t)strlen(text));
}

/* Sends a text only when it changed. */
static void put_text(app_t *app, uint32_t slot, const char *text)
{
    sent_t *sent = &app->sent[slot];

    if (!sent->has_text || (strcmp(sent->text, text) != 0)) {
        sent->has_text = true;
        (void)snprintf(sent->text, sizeof(sent->text), "%s", text);
        set_text(app, slot, text);
    }
}

/* Sends a colour only when it changed. */
static void put_color(app_t *app, uint32_t slot, uint32_t color)
{
    sent_t *sent = &app->sent[slot];

    if (!sent->has_color || (sent->color != color)) {
        sent->has_color = true;
        sent->color = color;
        (void)bento_ui_set_color(app->ui, app->ids[slot], color);
    }
}

static void put(app_t *app, uint32_t slot, const char *text,
                uint32_t color)
{
    put_text(app, slot, text);
    put_color(app, slot, color);
}

/* setv(): a value only when it changed. */
static void put_value(app_t *app, uint32_t slot, int32_t value)
{
    sent_t *sent = &app->sent[slot];

    if (!sent->has_value || (sent->value != value)) {
        sent->has_value = true;
        sent->value = value;
        (void)bento_ui_set_value(app->ui, app->ids[slot], value);
    }
}

/* ui.poll() whose events nobody reads; a failed poll answers none. */
static void drain_events(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    if (bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                      NULL) != BENTO_OK) {
        count = 0U;
    }
}

static bento_status_t add_widget(app_t *app, const widget_spec_t *spec)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    if (spec->text != NULL) {
        args.text = spec->text;
        args.text_len = (uint32_t)strlen(spec->text);
    }
    args.x = spec->x;
    args.y = spec->y;
    args.w = spec->width;
    args.h = spec->height;
    args.color = spec->color;
    args.min_val = spec->minimum;
    args.max_val = spec->maximum;
    args.font_px = spec->value;
    return bento_ui_create(app->ui, spec->kind, &args,
                           &app->ids[spec->slot]);
}

static bento_status_t
add_widgets(app_t *app, const widget_spec_t *specs, uint32_t count)
{
    uint32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_widget(app, &specs[i]);
    }
    return status;
}

/* A fresh screen, settled before the first widget. */
static bento_status_t clear_screen(const app_t *app)
{
    bento_status_t status = bento_ui_screen_default(app->ui);

    if (status == BENTO_OK) {
        status = wait_ms(SETTLE_MS);
    }
    return status;
}

/* The card shown when the model is missing or has other classes. */
static bento_status_t show_missing(app_t *app, const char *title,
                                   const char *text)
{
    static const widget_spec_t panel = {
        UI_WIDGET_PANEL, SLOT_NONE, NULL,        96, 110, 600, 180,
        COLOR_CARD,      COLOR_BAD, CARD_RADIUS, 1};
    static const widget_spec_t body = {
        UI_WIDGET_LABEL, SLOT_STATUS, " ", 120, 170, AUTO, AUTO,
        COLOR_TEXT,      0,           0,   20};
    static const widget_spec_t heading_spec = {
        UI_WIDGET_LABEL, SLOT_NONE, NULL, 120, 126, AUTO, AUTO,
        COLOR_BAD,       0,         0,    24};
    widget_spec_t heading = heading_spec;
    bento_status_t status;

    heading.text = title;
    (void)printf("%s\n", text);
    status = clear_screen(app);
    if (status == BENTO_OK) {
        status = add_widget(app, &panel);
    }
    if (status == BENTO_OK) {
        status = add_widget(app, &heading);
    }
    if (status == BENTO_OK) {
        status = add_widget(app, &body);
    }
    if (status == BENTO_OK) {
        set_text(app, SLOT_STATUS, text); /* .text() fits 126 bytes */
        drain_events(app->ui);
    }
    return status;
}

static bento_status_t show_not_on_board(app_t *app)
{
    char text[TEXT_CHARS];

    (void)snprintf(text, sizeof(text),
                   "Deploy %s\nfrom edgeai-store.tesaiot.dev\n"
                   "(sign in with GitHub) on a Dev Kit.",
                   MODEL_NAME);
    return show_missing(app, "Model not on this board", text);
}

/* Does the model have every chip class? *odd: it does not, and the
 * card says which ones it lacks; never guess. */
static bento_status_t check_classes(app_t *app, bool *odd)
{
    char text[TEXT_CHARS];
    size_t used;
    int32_t j;
    uint32_t i;

    used = (size_t)snprintf(text, sizeof(text), "%s has no class\n",
                            MODEL_NAME);
    *odd = false;
    for (j = 0; j < CHIP_COUNT; j++) {
        bool found = false;

        for (i = 0U; i < app->label_count; i++) {
            found = found || (app->chip_of[i] == j);
        }
        if (!found && (used < sizeof(text))) {
            used +=
                (size_t)snprintf(&text[used], sizeof(text) - used,
                                 "%s%s", *odd ? ", " : "", CHIPS[j]);
        }
        *odd = *odd || !found;
    }
    return *odd ? show_missing(app,
                               "Model classes differ from this example",
                               text)
                : BENTO_OK;
}

/* ---- 4) This model's screen (T4 state + dwell) ---- */

/* The title, the "Now" card, the heartbeat and the verdict. */
static bento_status_t add_now_card(app_t *app)
{
    static const widget_spec_t top[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Activity Timeline", 12, 4, AUTO,
         AUTO, COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Detects 5 activities when worn. Cannot: work lying on a desk "
         "(reads sitting).",
         12, 38, AUTO, AUTO, COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 12, 64, 470, 272, COLOR_CARD,
         COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Now", 12 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_LED, SLOT_HEARTBEAT, NULL, 756, 6, 26, 26, COLOR_OK,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_VERDICT, "Not sure", 28, 96, AUTO, AUTO,
         COLOR_DIM, 0, 0, 28},
    };

    return add_widgets(app, top, sizeof(top) / sizeof(top[0]));
}

/* One chip per activity, then the dwell bar and the candidate
 * line. */
static bento_status_t add_chips(app_t *app)
{
    static const widget_spec_t panel = {
        UI_WIDGET_PANEL, SLOT_CHIP_PANELS, NULL, CHIP_X, 170, 80, 56,
        COLOR_CARD,      COLOR_DIM,        8,    1};
    static const widget_spec_t text = {UI_WIDGET_LABEL,
                                       SLOT_CHIP_TEXTS,
                                       NULL,
                                       CHIP_X,
                                       188,
                                       AUTO,
                                       AUTO,
                                       COLOR_DIM,
                                       0,
                                       0,
                                       LABEL_PX};
    static const widget_spec_t rest[] = {
        {UI_WIDGET_BAR, SLOT_BAR, NULL, 28, 236, 436, 10, 0, 0, 100, 0},
        {UI_WIDGET_LABEL, SLOT_CANDIDATE, " ", 28, 250, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
    };
    widget_spec_t chip = panel;
    widget_spec_t name = text;
    int32_t j;
    bento_status_t status = BENTO_OK;

    for (j = 0; (j < CHIP_COUNT) && (status == BENTO_OK); j++) {
        chip.slot = (uint8_t)(SLOT_CHIP_PANELS + j);
        chip.x = (int16_t)(CHIP_X + (CHIP_STEP_PX * j));
        name.slot = (uint8_t)(SLOT_CHIP_TEXTS + j);
        name.x = (int16_t)(chip.x + CHIP_TEXT_DX);
        name.text = CHIPS[j];
        status = add_widget(app, &chip);
        if (status == BENTO_OK) {
            status = add_widget(app, &name);
        }
    }
    if (status == BENTO_OK) {
        status = add_widget(app, &rest[0]);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(app->ui, app->ids[SLOT_BAR],
                                 COLOR_TEXT);
        status = add_widget(app, &rest[1]);
    }
    return status;
}

/* One row of the time card: the activity's m:ss and its share. */
static bento_status_t add_row(app_t *app, int32_t j)
{
    static const widget_spec_t row = {
        UI_WIDGET_LABEL, SLOT_ROWS, NULL, ROW_X,   ROW_Y, AUTO, AUTO,
        COLOR_DIM,       0,         0,    LABEL_PX};
    static const widget_spec_t bar = {UI_WIDGET_BAR,
                                      SLOT_ROW_BARS,
                                      NULL,
                                      ROW_X,
                                      ROW_BAR_Y,
                                      266,
                                      8,
                                      0,
                                      0,
                                      100,
                                      0};
    widget_spec_t spec = row;
    char text[TEXT_CHARS];
    bento_status_t status;

    (void)snprintf(text, sizeof(text), "%s 0:00", CHIPS[j]);
    spec.slot = (uint8_t)(SLOT_ROWS + j);
    spec.y = (int16_t)(ROW_Y + (ROW_STEP_PX * j));
    spec.text = text;
    status = add_widget(app, &spec);
    if (status == BENTO_OK) {
        spec = bar;
        spec.slot = (uint8_t)(SLOT_ROW_BARS + j);
        spec.y = (int16_t)(ROW_BAR_Y + (ROW_STEP_PX * j));
        status = add_widget(app, &spec);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(app->ui, app->ids[spec.slot],
                                 COLOR_TEXT);
    }
    return status;
}

/* The "Time per activity" card: one row per activity. */
static bento_status_t add_time_card(app_t *app)
{
    static const widget_spec_t card[] = {
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 494, 64, 294, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Time per activity",
         494 + CARD_TITLE_DX, 64 + CARD_TITLE_DY, AUTO, AUTO,
         COLOR_INFO, 0, 0, LABEL_PX},
    };
    int32_t j;
    bento_status_t status =
        add_widgets(app, card, sizeof(card) / sizeof(card[0]));

    for (j = 0; (j < CHIP_COUNT) && (status == BENTO_OK); j++) {
        status = add_row(app, j);
    }
    return status;
}

static bento_status_t build_screen(app_t *app)
{
    static const widget_spec_t bottom[] = {
        {UI_WIDGET_BUTTON, SLOT_DWELL, "Min 5 s", 508, 266, 266, 60,
         COLOR_BUTTON, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 12, 346, AUTO, AUTO,
         COLOR_WARN, 0, 0, LABEL_PX},
    };
    bento_status_t status = clear_screen(app);

    if (status == BENTO_OK) {
        status = add_now_card(app);
    }
    if (status == BENTO_OK) {
        status = add_chips(app);
    }
    if (status == BENTO_OK) {
        status = add_time_card(app);
    }
    if (status == BENTO_OK) {
        status = add_widgets(app, bottom,
                             sizeof(bottom) / sizeof(bottom[0]));
    }
    if (status == BENTO_OK) {
        drain_events(app->ui);
    }
    return status;
}

/* The name of a class; Not sure after the model's last class. */
static const char *class_name(const app_t *app, int32_t klass)
{
    return (klass == NOT_SURE) ? "Not sure" : app->labels[klass];
}

static int32_t chip_for(const app_t *app, int32_t klass)
{
    return (klass == NOT_SURE) ? NO_CHIP : app->chip_of[klass];
}

/* True when the accepted state changes; one console line each. */
static bool dwell_step(const app_t *app, dwell_t *dwell, int32_t klass,
                       int32_t percent, uint32_t now_ms,
                       int32_t dwell_ms)
{
    char stamp[CLOCK_CHARS];
    bool changed = false;

    if (klass != dwell->candidate) {
        dwell->candidate = klass;
        dwell->since_ms = now_ms;
    }
    if ((klass != dwell->accepted) &&
        (bento_time_ticks_diff(now_ms, dwell->since_ms) >= dwell_ms)) {
        /* an activity; unlabeled and Not sure get no number */
        const bool counted = chip_for(app, klass) >= 0;

        dwell->accepted = klass;
        dwell->count += counted ? 1 : 0;
        clock_text(stamp, sizeof(stamp));
        (void)printf("%s %s %" PRId32 "%%", stamp,
                     class_name(app, klass), percent);
        if (counted) {
            (void)printf(" #%" PRId32, dwell->count);
        }
        (void)printf("\n");
        changed = true;
    }
    return changed;
}

/* A real number as '%.1f' writes it on this board. */
static void tenths_text(int32_t ms, char *text)
{
    uint32_t length = 0U;
    const bento_real_t seconds =
        (bento_real_t)ms / (bento_real_t)MS_PER_S;

    if (bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, seconds, 1,
                                 &length) != BENTO_OK) {
        text[0] = '\0';
    }
}

/* Accepted = white, candidate = grey-blue. */
static void draw_chips(app_t *app, const dwell_t *dwell)
{
    static const uint32_t panel_colors[] = {COLOR_CARD, COLOR_TEXT,
                                            COLOR_BUTTON};
    static const uint32_t text_colors[] = {COLOR_DIM, COLOR_CARD,
                                           COLOR_TEXT};
    const int32_t accepted = chip_for(app, dwell->accepted);
    const int32_t candidate = chip_for(app, dwell->candidate);
    int32_t j;

    for (j = 0; j < CHIP_COUNT; j++) {
        uint32_t look = 0U;

        if (j == accepted) {
            look = 1U;
        } else if (j == candidate) {
            look = 2U;
        } else {
            /* neither: the plain look */
        }

        put_color(app, (uint32_t)SLOT_CHIP_PANELS + (uint32_t)j,
                  panel_colors[look]);
        put_color(app, (uint32_t)SLOT_CHIP_TEXTS + (uint32_t)j,
                  text_colors[look]);
    }
}

/* The dwell bar and the line under it. */
static void draw_candidate(app_t *app, const run_t *run,
                           int32_t dwell_ms, uint32_t now_ms)
{
    const dwell_t *dwell = &run->dwell;
    const int32_t elapsed_ms =
        ((dwell->candidate != dwell->accepted) && (dwell_ms != 0))
            ? bento_time_ticks_diff(now_ms, dwell->since_ms)
            : 0;
    const int32_t share =
        (elapsed_ms * PERCENT) / ((dwell_ms != 0) ? dwell_ms : 1);
    char elapsed[BENTO_FMT_BUF_SIZE];
    char needed[BENTO_FMT_BUF_SIZE];
    char text[TEXT_CHARS];

    put_value(app, SLOT_BAR, (share < PERCENT) ? share : PERCENT);
    if (elapsed_ms != 0) {
        tenths_text(elapsed_ms, elapsed);
        tenths_text(dwell_ms, needed);
        (void)snprintf(text, sizeof(text), "%s %s/%s s",
                       class_name(app, dwell->candidate), elapsed,
                       needed);
        put_text(app, SLOT_CANDIDATE, text);
    } else if (run->top.fresh) {
        (void)snprintf(text, sizeof(text),
                       "top %s %" PRId32 "%% | Confirm at %" PRId32
                       "%%",
                       app->labels[run->top.klass], run->top.percent,
                       run->top.confirm);
        put_text(app, SLOT_CANDIDATE, text);
    } else {
        /* nothing new to say */
    }
}

static void draw(app_t *app, const run_t *run, int32_t dwell_ms,
                 uint32_t now_ms)
{
    const int32_t accepted = run->dwell.accepted;

    /* the heartbeat; a setter's refusal is ignored (modui.c) */
    (void)bento_ui_set_value(app->ui, app->ids[SLOT_HEARTBEAT],
                             (run->blink && run->top.fresh) ? 1 : 0);
    /* unlabeled = no activity: dim */
    put(app, SLOT_VERDICT, class_name(app, accepted),
        (chip_for(app, accepted) >= 0) ? COLOR_TEXT : COLOR_DIM);
    draw_chips(app, &run->dwell);
    draw_candidate(app, run, dwell_ms, now_ms);
}

/* m:ss and the share of the counted time, per activity. */
static void draw_times(app_t *app, const run_t *run, int32_t current)
{
    char text[TEXT_CHARS];
    int32_t total = 0;
    int32_t j;

    for (j = 0; j < CHIP_COUNT; j++) {
        total += run->spent_ms[j];
    }
    for (j = 0; j < CHIP_COUNT; j++) {
        const int32_t seconds = run->spent_ms[j] / MS_PER_S;

        (void)snprintf(text, sizeof(text), "%s %" PRId32 ":%02" PRId32,
                       CHIPS[j], seconds / S_PER_MIN,
                       seconds % S_PER_MIN);
        put(app, (uint32_t)SLOT_ROWS + (uint32_t)j, text,
            (j == current) ? COLOR_TEXT : COLOR_DIM);
        put_value(app, (uint32_t)SLOT_ROW_BARS + (uint32_t)j,
                  (run->spent_ms[j] * PERCENT) /
                      ((total != 0) ? total : 1));
    }
}

/* ---- 5) Main program ---- */

static const char *help_text(const app_t *app)
{
    return app->has_buttons
               ? "Wear it on your chest - SW6 = Min, SW5 = pause"
               : "Wear it on your chest";
}

/* The model list must not have changed while paused. */
static bento_status_t check_model_list(app_t *app)
{
    char text[TEXT_CHARS];
    int32_t count = 0;
    bento_status_t status = bento_edgeai_count(&count);

    if (status != BENTO_OK) {
        (void)printf("select: %s\n", link_error_text(status));
        (void)snprintf(text, sizeof(text), "Cannot start %s",
                       MODEL_NAME);
        status = raise_stop(app, text);
    } else if (count != app->model_count) {
        status = raise_stop(app, "Model list changed - run again");
    } else {
        /* the same list */
    }
    return status;
}

/* Loads and starts the model: up to 15 s. */
static bento_status_t select_model(app_t *app)
{
    char text[TEXT_CHARS];
    bento_status_t status = bento_edgeai_select_start(app->model_index);
    const bool sent = status == BENTO_OK;

    if (sent) {
        status = finish_op();
    }
    if ((status != BENTO_OK) && (status != BENTO_E_CANCELLED)) {
        (void)printf("select: %s\n",
                     sent ? "edge_ai: select not confirmed"
                          : "edge_ai: select send failed");
        (void)snprintf(text, sizeof(text), "Cannot start %s",
                       MODEL_NAME);
        status = raise_stop(app, text);
    }
    return status;
}

/* start(): on resume, Stop if a model was added or removed
 * meanwhile. */
static bento_status_t start_model(app_t *app, bool resuming)
{
    char text[TEXT_CHARS];
    bento_status_t status = BENTO_OK;

    (void)snprintf(text, sizeof(text), "Loading %s ...", MODEL_NAME);
    put(app, SLOT_STATUS, text, COLOR_WARN);
    drain_events(app->ui);
    app->reader.failing = false;
    if (resuming) {
        status = check_model_list(app);
    }
    if (status == BENTO_OK) {
        status = select_model(app);
    }
    if (status == BENTO_OK) {
        put(app, SLOT_STATUS, help_text(app), COLOR_TEXT);
    }
    return status;
}

/* One new result: the top class steps the dwell when it is at
 * Confirm at, else Not sure does. */
static void take_result(app_t *app, run_t *run,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    const uint32_t count = (result->class_count < (uint8_t)MAX_CLASSES)
                               ? result->class_count
                               : (uint32_t)MAX_CLASSES;
    uint32_t top = 0U;
    uint32_t klass;

    for (klass = 1U; klass < count; klass++) {
        if (result->score_pct[klass] > result->score_pct[top]) {
            top = klass;
        }
    }
    run->top.fresh = true;
    run->top.klass = top;
    run->top.percent = result->score_pct[top];
    run->top.confirm = run->thresholds.confirm[top];
    (void)dwell_step(
        app, &run->dwell,
        (run->top.percent >= run->top.confirm) ? (int32_t)top
                                               : NOT_SURE,
        run->top.percent, now_ms, DWELLS[run->dwell_choice]);
}

/* Every POLL_MS while not paused. */
static bento_status_t poll_model(app_t *app, run_t *run,
                                 uint32_t now_ms)
{
    bento_edgeai_result_t result;
    bool fresh = false;
    const bento_status_t status = read_result(app, &result, &fresh);

    if ((status == BENTO_OK) && fresh) {
        take_result(app, run, &result, now_ms);
    }
    return status;
}

/* Every DRAW_MS: the blink, the time in the accepted activity (none
 * while paused) and the screen. */
static void redraw(app_t *app, run_t *run, uint32_t now_ms,
                   int32_t elapsed_ms)
{
    const int32_t current = chip_for(app, run->dwell.accepted);

    run->drawn_ms = now_ms;
    run->blink = !run->blink;
    if (current >= 0) {
        run->spent_ms[current] += elapsed_ms;
    }
    draw(app, run, DWELLS[run->dwell_choice], now_ms);
    draw_times(app, run, current);
    run->top.fresh = false;
}

/* A tap on the Min button. A failed poll answers no events. */
static bool read_taps(const app_t *app)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    uint32_t i;
    bool tapped = false;

    if (bento_ui_poll(app->ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                      NULL) != BENTO_OK) {
        count = 0U;
    }
    for (i = 0U; i < count; i++) {
        if ((events[i].type == (uint8_t)BENTO_UI_EVENT_CLICKED) &&
            (events[i].id == app->ids[SLOT_DWELL])) {
            tapped = true;
        }
    }
    return tapped;
}

/* SW6 or a tap: Min goes 5 -> 10 -> 0 -> 5 s. */
static void next_dwell(app_t *app, run_t *run)
{
    char text[TEXT_CHARS];

    run->dwell_choice = (run->dwell_choice + 1) % DWELL_COUNT;
    (void)snprintf(text, sizeof(text), "Min %" PRId32 " s",
                   DWELLS[run->dwell_choice] / MS_PER_S);
    put_text(app, SLOT_DWELL, text);
}

/* SW5: pause stops the AI core; resume starts it again. */
static bento_status_t toggle_pause(app_t *app, run_t *run)
{
    bento_status_t status;

    run->paused = !run->paused;
    run->dwell.accepted = NOT_SURE; /* start again from Not sure */
    run->dwell.candidate = NOT_SURE;
    if (run->paused) {
        status = stop_ai();
        if (status == BENTO_OK) {
            put(app, SLOT_STATUS, "Paused - SW5 resumes", COLOR_WARN);
        }
    } else {
        status = start_model(app, true);
    }
    return status;
}

/* Both buttons and the taps: Min and pause. */
static bento_status_t read_controls(app_t *app, run_t *run)
{
    bento_status_t status = sample_button(app, &run->pause_button);

    if (status == BENTO_OK) {
        status = sample_button(app, &run->dwell_button);
    }
    if (status == BENTO_OK) {
        bool tapped = pressed_now(&run->dwell_button);

        if (read_taps(app)) {
            tapped = true;
        }
        if (tapped) {
            next_dwell(app, run);
        }
        if (pressed_now(&run->pause_button)) {
            status = toggle_pause(app, run);
        }
    }
    return status;
}

/* One pass of the loop: controls, results, drawing. */
static bento_status_t run_once(app_t *app, run_t *run)
{
    uint32_t now_ms;
    int32_t elapsed_ms;
    bento_status_t status = read_controls(app, run);

    now_ms = bento_time_ticks_ms();
    if ((status == BENTO_OK) && !run->paused &&
        (bento_time_ticks_diff(now_ms, run->polled_ms) >= POLL_MS)) {
        run->polled_ms = now_ms;
        status = poll_model(app, run, now_ms);
    }
    elapsed_ms = bento_time_ticks_diff(now_ms, run->drawn_ms);
    if ((status == BENTO_OK) && (elapsed_ms >= DRAW_MS)) {
        redraw(app, run, now_ms, elapsed_ms);
    }
    return status;
}

/* The loop runs until Stop, or until the program stops itself. */
static bento_status_t run_loop(app_t *app, run_t *run)
{
    bento_status_t status = BENTO_OK;

    while (status == BENTO_OK) {
        status = run_once(app, run);
        if (status == BENTO_OK) {
            status = wait_ms(LOOP_MS);
        }
    }
    return status;
}

static bento_status_t run_model(app_t *app)
{
    run_t run;
    bool odd = false;
    bento_status_t status = check_classes(app, &odd);

    (void)memset(&run, 0, sizeof(run));
    if ((status == BENTO_OK) && !odd) {
        read_thresholds(app->model_index, &run.thresholds);
        run.pause_button.index = PAUSE_BUTTON;
        run.dwell_button.index = DWELL_BUTTON;
        run.dwell.accepted = NOT_SURE;
        run.dwell.candidate = NOT_SURE;
        run.dwell_choice = DWELL_START;
        status = start_model(app, false);
        run.drawn_ms = bento_time_ticks_ms();
        run.polled_ms = run.drawn_ms;
        if (status == BENTO_OK) {
            status = run_loop(app, &run);
        }
    }
    return status;
}

/* The screen, the loop, and the AI core always released after it.
 * A Stop the program raised itself is shown, then it ends. */
static bento_status_t watch_model(app_t *app)
{
    bento_status_t status = build_screen(app);

    if (status == BENTO_OK) {
        bento_status_t loop_status = run_model(app);
        bento_status_t stop_status;

        if ((loop_status == BENTO_E_FAILED) &&
            (app->stop_text[0] != '\0')) {
            put(app, SLOT_STATUS, app->stop_text, COLOR_BAD);
            (void)printf("%s\n", app->stop_text);
            loop_status = BENTO_OK;
        }
        stop_status = stop_ai();
        status = (loop_status != BENTO_OK) ? loop_status : stop_status;
    }
    return status;
}

int main(void)
{
    app_t *app = &g_app;
    bool supported = false;
    bento_status_t status;

    (void)memset(app, 0, sizeof(*app));
    app->ui = bento_runtime_get_ui();
    app->has_buttons = bento_buttons_count() > 0U;
    status = bento_edgeai_supported(&supported);
    if ((status == BENTO_OK) && !supported) {
        status = BENTO_E_UNSUPPORTED; /* import edge_ai fails */
    }
    if (status == BENTO_OK) {
        status =
            find_model(app) ? watch_model(app) : show_not_on_board(app);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
