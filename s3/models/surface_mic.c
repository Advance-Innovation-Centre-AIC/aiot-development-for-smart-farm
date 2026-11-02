/*
 * surface_mic.c -- Robot Vacuum Brain: สมองหุ่นยนต์ดูดฝุ่น
 * (ต้องใช้ SurfaceMic จาก Edge AI Store)
 *
 * ใช้โมเดล "SurfaceMic" จาก Edge AI Store แยกชนิดพื้นผิวจากเสียงเครื่อง
 * ดูดฝุ่น: ลอยในอากาศ (Air) พื้นแข็ง (Floor) หรือพรม (Carpet) เมื่อชนิด
 * ใดอยู่อันดับหนึ่งนาน 1 วินาที หุ่นยนต์เปลี่ยนสิ่งที่ทำ: ปิดมอเตอร์
 * ทำงานปกติ หรือเพิ่มแรงดูด (BOOST) เปิดเครื่องดูดฝุ่นใกล้บอร์ด แตะ
 * ปุ่ม Mop หรือกด SW6 (ปุ่มบน) เพื่อเปิด/ปิดผ้าถู SW5 (ปุ่มล่าง)
 * หยุดชั่วคราว/ทำต่อ
 *
 * Runs "SurfaceMic" from the Edge AI Store: it tells the surface
 * from a vacuum's sound (Air, Floor, Carpet). Once a surface has
 * been on top for 1 s the robot changes what it does: motor off,
 * Normal, or BOOST. Run a vacuum near the board. Tap Mop or press
 * SW6 (upper) to turn the mop on or off; SW5 (lower) =
 * pause/resume.
 *
 * Twin of surface_mic.py (aiot-development-for-smart-farm/s3/models/surface_mic.py). Style: CLEAN_C.md.
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
 *   - The dict of texts and colours already sent is one entry per
 *     widget slot; a text is kept up to TEXT_CHARS - 1 bytes.
 *   - A score is the board's whole percent: int(score * 100 + 0.5)
 *     gives it back unchanged, so the percent is used as it comes.
 *   - Python's index -1 ("Not sure", no chip) is the row after the
 *     last label here: NOT_SURE in the dwell, -1 nowhere else.
 *   - Dwell.step() answers whether the state changed; nobody reads
 *     it, so here it answers nothing.
 *   - In the BENTO Emulator, Edge AI from C is not available yet:
 *     the program says so, then goes on as the .py does when it
 *     cannot read the model list (the "not on this board" card), and
 *     ends.
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
static const char MODEL_NAME[] = "SurfaceMic"; /* exact name */
/* BUILTIN (false = deployed from the Store): in source_matches() */
/* CHIPS = NEED: the classes this example needs */
static const char *const CHIPS[] = {"Air", "Floor", "Carpet"};

enum {
    VOTE_NEED = 3,        /* (not used by T4) */
    VOTE_WINDOW = 5,      /* (not used by T4) */
    REFRACT_MS = 1500,    /* (not used by T4) */
    HOLD_MS = 2000,       /* (not used by T4) */
    DEFAULT_HEARING = 40, /* Hearing at, when the board cannot say */
    DEFAULT_CONFIRM = 65, /* Confirm at */
    DEFAULT_CLEAR = 30,   /* clear below */
    POLL_MS = 100,
    DRAW_MS = 500,
    DWELL_MS =
        1000 /* ms on top before the robot changes what it does */
};

/* Colours, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_CARD = 0x171B22,
    COLOR_BUTTON = 0x5E6878,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF,
    COLOR_ALARM_BACK = 0x3A1416
};

/* What the robot does on a surface, and why. */
typedef struct {
    const char *surface;
    const char *act;
    const char *why;
    uint32_t color;
} act_t;

/* ACTS, what the robot does on each surface: in show_robot() */

/* Times, counts and sizes. */
enum {
    SETTLE_MS = 150, /* let the cleared screen settle */
    LOOP_MS = 20,
    OP_POLL_MS = 20,  /* ask a pending select or stop this often */
    SILENT_MS = 3000, /* result() failing this long = Stop */
    MS_PER_S = 1000,
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    NOT_SURE = MAX_CLASSES, /* the dwell's row after the labels */
    NEED_COUNT = 3,
    ACT_COUNT = 4,
    TEXT_CHARS = 128, /* a SET_TEXT carries at most 126 bytes */
    CLOCK_CHARS = 36, /* "HH:MM:SS" of any three int32_t */
    PAUSE_BUTTON = 1, /* SW5, the lower button */
    MOP_BUTTON = 0    /* SW6, the upper button: Mop */
};

/* Layout on the 792 x 398 canvas. */
enum {
    AUTO = -1, /* x, y, w or h the .py leaves out */
    LABEL_PX = 16,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 14,
    CARD_TITLE_DY = 8,
    CHIP_X = 28,
    CHIP_STEP_PX = 148,
    CHIP_TEXT_DX = 10
};

/* Every widget the program changes later has a slot. */
enum {
    CHIP_COUNT = 3,
    SLOT_HEARTBEAT = 0,
    SLOT_VERDICT = 1,
    SLOT_CHIPS = 2, /* CHIP_COUNT pairs: panel, then label */
    SLOT_CANDIDATE = SLOT_CHIPS + (2 * CHIP_COUNT),
    SLOT_ACT = SLOT_CANDIDATE + 1,
    SLOT_WHY = SLOT_ACT + 1,
    SLOT_MOP = SLOT_WHY + 1,
    SLOT_MOP_BUTTON = SLOT_MOP + 1,
    SLOT_STATUS = SLOT_MOP_BUTTON + 1,
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
    uint32_t color;
    char text[TEXT_CHARS];
} sent_t;

/* The newest result read, and since when result() fails. */
typedef struct {
    bool has_sequence;
    uint32_t sequence;
    bool failing;
    uint32_t failing_since_ms;
} reader_t;

/* One board button: one event per press, holding does not repeat. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} board_button_t;

/* A class counts after DWELL_MS on top at Confirm at; NOT_SURE when
 * none does. */
typedef struct {
    int32_t chip_of[NOT_SURE + 1]; /* the chip of a class, -1 = none */
    uint32_t accepted;
    uint32_t candidate;
    uint32_t since_ms; /* the candidate is on top since */
    int32_t count;
} dwell_t;

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
    dwell_t dwell;
    reader_t reader;
    char stop_text[TEXT_CHARS]; /* "" until the program stops itself */
} app_t;

/* The loop's state. */
typedef struct {
    bento_edgeai_thresholds_t thresholds;
    board_button_t pause_button;
    board_button_t mop_button;
    bool mop;
    bool paused;
    bool blink;
    bool has_top;        /* a new result since the last draw: ... */
    uint32_t top;        /* ... its top class ... */
    int32_t top_percent; /* ... and its score */
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

/* Built in or Store as BUILTIN asks; a row the board cannot place
 * (None on the emulator) counts as built in. */
static bool source_matches(int32_t row, int32_t compiled)
{
    static const bool builtin = false; /* BUILTIN */
    bool matches;

    if (builtin) {
        matches = (compiled < 0) || (row < compiled);
    } else {
        matches = (compiled >= 0) && (row >= compiled);
    }
    return matches;
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

/* Per class: Hearing at, Confirm at, clear below; the defaults for
 * every class when the board cannot say. */
static void read_thresholds(uint32_t index,
                            bento_edgeai_thresholds_t *thresholds)
{
    uint32_t i;

    bento_edgeai_thresholds_init(thresholds);
    if (bento_edgeai_thresholds(index, thresholds) != BENTO_OK) {
        for (i = 0U; i < (uint32_t)MAX_CLASSES; i++) {
            thresholds->enter[i] = DEFAULT_HEARING;
            thresholds->confirm[i] = DEFAULT_CONFIRM;
            thresholds->exit[i] = DEFAULT_CLEAR;
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

/* ---- 3) Screen helpers ---- */

/* Compares an event's kind through a uint8_t, so both sides have one
 * type (CLEAN_C.md C5). */
static bool is_kind(const bento_ui_event_t *event, uint8_t kind)
{
    return event->type == kind;
}

/* The widget setters ignore a refusal in MicroPython (modui.c), so
 * they do here too. */
static void set_text(const app_t *app, uint32_t slot, const char *text)
{
    (void)bento_ui_set_text(app->ui, app->ids[slot], text,
                            (uint32_t)strlen(text));
}

static void set_value(const app_t *app, uint32_t slot, int32_t value)
{
    (void)bento_ui_set_value(app->ui, app->ids[slot], value);
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

/* The card shown when the model is not on this board, or has other
 * classes. */
static bento_status_t show_missing(app_t *app, const char *title,
                                   const char *text)
{
    static const widget_spec_t card[] = {
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 96, 110, 600, 180,
         COLOR_CARD, COLOR_BAD, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, NULL, 120, 126, AUTO, AUTO,
         COLOR_BAD, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 120, 170, AUTO, AUTO,
         COLOR_TEXT, 0, 0, 20},
    };
    widget_spec_t heading = card[1];
    bento_status_t status;

    (void)printf("%s\n", text);
    status = clear_screen(app);
    if (status == BENTO_OK) {
        status = add_widget(app, &card[0]);
    }
    if (status == BENTO_OK) {
        heading.text = title;
        status = add_widget(app, &heading);
    }
    if (status == BENTO_OK) {
        status = add_widget(app, &card[2]);
    }
    if (status == BENTO_OK) {
        set_text(app, SLOT_STATUS, text); /* .text() fits 126 bytes */
        drain_events(app->ui);
    }
    return status;
}

/* The store deploy card: the model is not on this board. */
static bento_status_t show_not_deployed(app_t *app)
{
    char text[TEXT_CHARS];

    (void)snprintf(text, sizeof(text),
                   "Deploy %s\nfrom edgeai-store.tesaiot.dev\n(sign in "
                   "with GitHub) on a Dev Kit.",
                   MODEL_NAME);
    return show_missing(app, "Model not on this board", text);
}

/* Is this class one of the labels? */
static bool has_label(const app_t *app, const char *name)
{
    bool found = false;
    uint32_t klass;

    for (klass = 0U; klass < app->label_count; klass++) {
        if (strcmp(app->labels[klass], name) == 0) {
            found = true;
        }
    }
    return found;
}

/* odd(): a model whose classes are not the ones this example needs:
 * say so on the card, never guess. *odd: some are missing. */
static bento_status_t check_classes(app_t *app, bool *odd)
{
    char text[TEXT_CHARS];
    size_t used;
    uint32_t i;
    bento_status_t status = BENTO_OK;

    *odd = false;
    (void)snprintf(text, sizeof(text), "%s has no class\n", MODEL_NAME);
    for (i = 0U; i < (uint32_t)NEED_COUNT; i++) {
        if (!has_label(app, CHIPS[i])) { /* NEED = CHIPS */
            used = strlen(text);
            (void)snprintf(&text[used], sizeof(text) - used, "%s%s",
                           *odd ? ", " : "", CHIPS[i]);
            *odd = true;
        }
    }
    if (*odd) {
        status = show_missing(
            app, "Model classes differ from this example", text);
    }
    return status;
}

/* ---- 4) This model's screen (T4 state + dwell) ---- */

/* The chip of a label: its place among CHIPS, -1 none. */
static int32_t chip_of(const char *label)
{
    int32_t chip = -1;
    uint32_t i;

    for (i = 0U; i < (uint32_t)CHIP_COUNT; i++) {
        if (strcmp(CHIPS[i], label) == 0) {
            chip = (int32_t)i;
        }
    }
    return chip;
}

static void dwell_init(app_t *app)
{
    dwell_t *dwell = &app->dwell;
    uint32_t klass;

    for (klass = 0U; klass <= (uint32_t)NOT_SURE; klass++) {
        dwell->chip_of[klass] = (klass < app->label_count)
                                    ? chip_of(app->labels[klass])
                                    : -1;
    }
    dwell->accepted = NOT_SURE;
    dwell->candidate = NOT_SURE;
    dwell->count = 0;
}

/* The name of a dwell row. */
static const char *dwell_name(const app_t *app, uint32_t klass)
{
    return (klass < app->label_count) ? app->labels[klass] : "Not sure";
}

/* One console line when the accepted state changes. */
static void dwell_step(app_t *app, uint32_t klass, int32_t percent,
                       uint32_t now_ms)
{
    dwell_t *dwell = &app->dwell;
    char stamp[CLOCK_CHARS];

    if (klass != dwell->candidate) {
        dwell->candidate = klass;
        dwell->since_ms = now_ms;
    }
    if ((klass != dwell->accepted) &&
        (bento_time_ticks_diff(now_ms, dwell->since_ms) >= DWELL_MS)) {
        dwell->accepted = klass;
        clock_text(stamp, sizeof(stamp));
        (void)printf("%s %s %" PRId32 "%%", stamp,
                     dwell_name(app, klass), percent);
        if (klass != (uint32_t)NOT_SURE) {
            dwell->count++;
            (void)printf(" #%" PRId32, dwell->count);
        }
        (void)printf("\n");
    }
}

/* The title, the "Now" card, the heartbeat and the verdict. */
static bento_status_t add_now_card(app_t *app)
{
    static const widget_spec_t top[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Robot Vacuum Brain", 12, 4, AUTO,
         AUTO, COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Detects: vacuum sound over air, floor, carpet. Cannot: work "
         "without a vacuum.",
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

/* One chip per material: a panel and its name. */
static bento_status_t add_chips(app_t *app)
{
    static const widget_spec_t chip_panel = {
        UI_WIDGET_PANEL, SLOT_CHIPS, NULL, CHIP_X, 170, 140, 56,
        COLOR_CARD,      COLOR_DIM,  8,    1};
    static const widget_spec_t chip_text = {UI_WIDGET_LABEL,
                                            SLOT_CHIPS,
                                            NULL,
                                            CHIP_X + CHIP_TEXT_DX,
                                            186,
                                            AUTO,
                                            AUTO,
                                            COLOR_DIM,
                                            0,
                                            0,
                                            20};
    widget_spec_t spec;
    int32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0; (i < CHIP_COUNT) && (status == BENTO_OK); i++) {
        const int32_t panel_slot = SLOT_CHIPS + (2 * i);
        const int32_t text_slot = panel_slot + 1;

        spec = chip_panel;
        spec.slot = (uint8_t)panel_slot;
        spec.x = (int16_t)(CHIP_X + (CHIP_STEP_PX * i));
        status = add_widget(app, &spec);
        if (status == BENTO_OK) {
            spec = chip_text;
            spec.slot = (uint8_t)text_slot;
            spec.text = CHIPS[i];
            spec.x =
                (int16_t)(CHIP_X + CHIP_TEXT_DX + (CHIP_STEP_PX * i));
            status = add_widget(app, &spec);
        }
    }
    return status;
}

static bento_status_t build_screen(app_t *app)
{
    static const widget_spec_t robot[] = {
        {UI_WIDGET_LABEL, SLOT_CANDIDATE, " ", 28, 240, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 494, 64, 294, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Robot does", 494 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_ACT, "Normal", 508, 100, AUTO, AUTO,
         COLOR_DIM, 0, 0, 28},
        {UI_WIDGET_LABEL, SLOT_WHY, " ", 508, 140, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_MOP, " ", 508, 170, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_BUTTON, SLOT_MOP_BUTTON, "Mop: ON", 508, 266, 266,
         60, COLOR_BUTTON, 0, 0, 20},
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
        status =
            add_widgets(app, robot, sizeof(robot) / sizeof(robot[0]));
    }
    if (status == BENTO_OK) {
        drain_events(app->ui);
    }
    return status;
}

/* Accepted white, candidate grey-blue. */
static void draw_chips(app_t *app)
{
    static const uint32_t panel_colors[] = {COLOR_CARD, COLOR_TEXT,
                                            COLOR_BUTTON};
    static const uint32_t text_colors[] = {COLOR_DIM, COLOR_CARD,
                                           COLOR_TEXT};
    const dwell_t *dwell = &app->dwell;
    int32_t chip;
    uint32_t look;

    for (chip = 0; chip < CHIP_COUNT; chip++) {
        look = 0U;
        if (chip == dwell->chip_of[dwell->accepted]) {
            look = 1U;
        } else if (chip == dwell->chip_of[dwell->candidate]) {
            look = 2U;
        } else {
            /* neither */
        }
        put_color(app, (uint32_t)SLOT_CHIPS + (2U * (uint32_t)chip),
                  panel_colors[look]);
        put_color(app,
                  (uint32_t)SLOT_CHIPS + (2U * (uint32_t)chip) + 1U,
                  text_colors[look]);
    }
}

/* A real number as '%.1f' writes it on this board. */
static void tenths_text(bento_real_t value, char *text)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value, 1,
                                 &length) != BENTO_OK) {
        text[0] = '\0';
    }
}

/* The candidate's time on top so far, or the newest top class. */
static void show_candidate(app_t *app, const run_t *run,
                           uint32_t now_ms)
{
    const dwell_t *dwell = &app->dwell;
    const int32_t elapsed_ms =
        (dwell->candidate != dwell->accepted)
            ? bento_time_ticks_diff(now_ms, dwell->since_ms)
            : 0;
    char elapsed[BENTO_FMT_BUF_SIZE];
    char needed[BENTO_FMT_BUF_SIZE];
    char text[TEXT_CHARS];

    if (elapsed_ms != 0) {
        tenths_text((bento_real_t)elapsed_ms / (bento_real_t)MS_PER_S,
                    elapsed);
        tenths_text((bento_real_t)DWELL_MS / (bento_real_t)MS_PER_S,
                    needed);
        (void)snprintf(text, sizeof(text), "%s %s/%s s",
                       dwell_name(app, dwell->candidate), elapsed,
                       needed);
        put_text(app, SLOT_CANDIDATE, text);
    } else if (run->has_top) {
        (void)snprintf(text, sizeof(text),
                       "top %s %" PRId32 "%% | Confirm at %d%%",
                       app->labels[run->top], run->top_percent,
                       (int)run->thresholds.confirm[run->top]);
        put_text(app, SLOT_CANDIDATE, text);
    } else {
        /* nothing new to say */
    }
}

static void draw(app_t *app, const run_t *run, uint32_t now_ms)
{
    const uint32_t accepted = app->dwell.accepted;

    set_value(app, SLOT_HEARTBEAT,
              (run->blink && run->has_top) ? 1 : 0);
    put(app, SLOT_VERDICT, dwell_name(app, accepted),
        (accepted != (uint32_t)NOT_SURE) ? COLOR_TEXT : COLOR_DIM);
    draw_chips(app);
    show_candidate(app, run, now_ms);
}

/* What the robot does on the accepted surface. */
static void show_robot(app_t *app, const run_t *run)
{
    static const act_t acts[ACT_COUNT] = {
        {"Air", "Motor off", "lifted - nozzle in the air", COLOR_TEXT},
        {"Floor", "Normal", "hard floor", COLOR_DIM},
        {"Carpet", "BOOST", "carpet - more suction", COLOR_TEXT},
        {"unlabeled", "Normal", "no vacuum heard", COLOR_DIM},
    };
    static const act_t unsure = {"", "Normal", "not sure - stay Normal",
                                 COLOR_DIM};
    const char *surface = dwell_name(app, app->dwell.accepted);
    const bool carpet = strcmp(surface, "Carpet") == 0;
    const act_t *act = &unsure;
    const char *mop = "Mop off";
    uint32_t i;

    for (i = 0U; i < (uint32_t)ACT_COUNT; i++) {
        if (strcmp(acts[i].surface, surface) == 0) {
            act = &acts[i];
        }
    }
    put(app, SLOT_ACT, act->act, act->color);
    put_text(app, SLOT_WHY, act->why);
    if (run->mop) {
        mop = carpet ? "Mop lifted (carpet)" : "Mop down";
    }
    put(app, SLOT_MOP, mop,
        (run->mop && carpet) ? COLOR_TEXT : COLOR_DIM);
}

/* ---- 5) Main program ---- */

static const char *help_text(const app_t *app)
{
    return app->has_buttons
               ? "Run a vacuum near it - SW6 = Mop, SW5 = pause"
               : "Run a vacuum near it";
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

/* sc.index(max(sc)): the first class with the top score. */
static uint32_t top_class(const bento_edgeai_result_t *result,
                          uint32_t count)
{
    uint32_t top = 0U;
    uint32_t klass;

    for (klass = 1U; klass < count; klass++) {
        if (result->score_pct[klass] > result->score_pct[top]) {
            top = klass;
        }
    }
    return top;
}

/* One new result: the top class and the surface's dwell. */
static void take_result(app_t *app, run_t *run,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    const uint32_t top = top_class(result, app->label_count);
    const int32_t percent = result->score_pct[top];
    const int32_t confirm = run->thresholds.confirm[top];

    run->has_top = true;
    run->top = top;
    run->top_percent = percent;
    dwell_step(app, (percent >= confirm) ? top : (uint32_t)NOT_SURE,
               percent, now_ms);
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

/* A tap on Mop (a click). A failed poll answers no events. */
static bool mop_tapped(const app_t *app)
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
        if ((is_kind(&events[i], BENTO_UI_EVENT_CLICKED)) &&
            (events[i].id == app->ids[SLOT_MOP_BUTTON])) {
            tapped = true;
        }
    }
    return tapped;
}

/* SW5: pause stops the AI core; resume starts it again. */
static bento_status_t toggle_pause(app_t *app, run_t *run)
{
    bento_status_t status;

    run->paused = !run->paused;
    app->dwell.accepted = NOT_SURE; /* start again from Not sure */
    app->dwell.candidate = NOT_SURE;
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

/* SW6 or a tap: the mop goes on or off. */
static bento_status_t read_controls(app_t *app, run_t *run)
{
    bool tapped;
    bento_status_t status = sample_button(app, &run->pause_button);

    if (status == BENTO_OK) {
        status = sample_button(app, &run->mop_button);
    }
    if (status == BENTO_OK) {
        tapped = pressed_now(&run->mop_button);
        if (mop_tapped(app)) {
            tapped = true;
        }
        if (tapped) {
            run->mop = !run->mop;
            put_text(app, SLOT_MOP_BUTTON,
                     run->mop ? "Mop: ON" : "Mop: OFF");
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
    bento_status_t status = read_controls(app, run);
    const uint32_t now_ms = bento_time_ticks_ms();

    if ((status == BENTO_OK) && !run->paused &&
        (bento_time_ticks_diff(now_ms, run->polled_ms) >= POLL_MS)) {
        run->polled_ms = now_ms;
        status = poll_model(app, run, now_ms);
    }
    if ((status == BENTO_OK) &&
        (bento_time_ticks_diff(now_ms, run->drawn_ms) >= DRAW_MS)) {
        run->drawn_ms = now_ms;
        run->blink = !run->blink;
        draw(app, run, now_ms);
        show_robot(app, run);
        run->has_top = false;
    }
    return status;
}

/* start(), then the loop until Stop or an error. */
static bento_status_t listen(app_t *app, run_t *run)
{
    bento_status_t status = start_model(app, false);

    run->drawn_ms = bento_time_ticks_ms();
    run->polled_ms = run->drawn_ms;
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
        dwell_init(app);
        run.pause_button.index = PAUSE_BUTTON;
        run.mop_button.index = MOP_BUTTON;
        run.mop = true;
        status = listen(app, &run);
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

/* The BENTO Emulator's C runtime has no Edge AI yet: every
 * bento_edgeai_* call answers BENTO_E_UNSUPPORTED. */
static const char EMULATOR_GAP[] =
    "Edge AI จาก C ยังไม่มีใน BENTO Emulator ให้รันบน Dev Kit หรือ "
    "Eva Kit";

static void say_emulator_gap(void)
{
    (void)printf(
        "%s / Edge AI from C is not in the BENTO Emulator yet: "
        "run it on a Dev Kit or Eva Kit.\n",
        EMULATOR_GAP);
}

/* The label, under the "not on this board" card. */
static bento_status_t show_emulator_gap(app_t *app)
{
    static const widget_spec_t gap = {UI_WIDGET_LABEL,
                                      SLOT_NONE,
                                      EMULATOR_GAP,
                                      12,
                                      300,
                                      AUTO,
                                      AUTO,
                                      COLOR_WARN,
                                      0,
                                      0,
                                      20};

    return add_widget(app, &gap);
}

int main(void)
{
    app_t *app = &g_app;
    bool supported = false;
    bool emulator = false;
    bool found = false;
    bento_status_t status;

    (void)memset(app, 0, sizeof(*app));
    app->ui = bento_runtime_get_ui();
    app->has_buttons = bento_buttons_count() > 0U;
    status = bento_edgeai_supported(&supported);
    if (status == BENTO_E_UNSUPPORTED) {
        say_emulator_gap();
        emulator = true; /* go on as the .py: no model list to read */
        status = BENTO_OK;
    } else if ((status == BENTO_OK) && !supported) {
        status = BENTO_E_UNSUPPORTED; /* import edge_ai fails */
    } else {
        /* a board with the model link, or a link error that ends it */
    }
    if (status == BENTO_OK) {
        found = find_model(app);
        status = found ? watch_model(app) : show_not_deployed(app);
    }
    if ((status == BENTO_OK) && emulator && !found) {
        status = show_emulator_gap(app);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
