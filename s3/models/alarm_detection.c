/*
 * alarm_detection.c -- Alarm Listener: ฟังเสียงสัญญาณเตือนภัย
 * (โมเดลในตัว Built-in model)
 *
 * ใช้โมเดลในตัว "Alarm Detection" ของ TESAIoT Dev Kit ฟังเสียงบี๊บของ
 * สัญญาณเตือนจากไมโครโฟน เป็นตัวอย่างสำหรับสอนเท่านั้น ไม่ใช่อุปกรณ์
 * ความปลอดภัย ได้ยินเสียงบี๊บเป็นชุด (2 ใน 3 ผลล่าสุดถึงเกณฑ์) สองชุด
 * ภายใน 4 วินาที = เตือน การ์ดทั้งสองเปลี่ยนเป็นสีแดงเข้มจนกว่าจะ
 * รับทราบ: แตะ ACK หรือกด SW6 (ปุ่มบน) ส่วน SW5 (ปุ่มล่าง) หยุดชั่วคราว
 * /ทำต่อ แตะ "Snooze" เพื่อพักการเตือน 60 วินาที (ยังนับต่อ)
 *
 * Listens for alarm beeps with the Dev Kit's built-in model "Alarm
 * Detection". A teaching demo, NOT a safety device. A beep burst is
 * 2 of the last 3 results at Confirm at; two bursts within 4 s are
 * an alarm, and both cards turn dark red until it is acknowledged:
 * tap ACK or press SW6 (upper). SW5 (lower) = pause/resume. Tap
 * "Snooze" to shelve the alarm for 60 s; it still counts.
 *
 * Twin of alarm_detection.py (aiot-development-for-smart-farm/s3/models/alarm_detection.py). Style: CLEAN_C.md.
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
 *   - The only episode here is the latched alarm (Episode(True)), so
 *     the info state SEEN is left out.
 *   - The list of bursts holds BURSTS_MAX (16) times: one burst
 *     takes at least 400 ms at one result per POLL_MS, so T3_MS
 *     holds 11 at most; a full list would forget its oldest.
 *   - A score is the board's whole percent: int(score * 100 + 0.5)
 *     gives it back unchanged, so the percent is used as it comes.
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
static const char MODEL_NAME[] = "Alarm Detection"; /* exact name */
/* BUILTIN (false = deployed from the Store): in source_matches() */

enum {
    VOTE_NEED = 2,        /* a beep burst = 2 of ... */
    VOTE_WINDOW = 3,      /* ... the last 3 results at Confirm at */
    REFRACT_MS = 1500,    /* no second event sooner than this */
    HOLD_MS = 2000,       /* not used by a latched alarm */
    DEFAULT_HEARING = 70, /* Hearing at, when the board cannot say */
    DEFAULT_CONFIRM = 85, /* Confirm at */
    DEFAULT_CLEAR = 55,   /* clear below */
    POLL_MS = 100,
    DRAW_MS = 500,
    GAP_MS = 300, /* quiet between bursts */
    T3_MS = 4000, /* 2 bursts in T3_MS = alarm */
    SNOOZE_S = 60 /* shelving */
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

/* The alarm's states: NORMAL > HEARD > UNACK > ACKED or ENDED. */
enum {
    STATE_NORMAL = 0,
    STATE_HEARD = 1,
    STATE_UNACK = 2,
    STATE_ACKED = 3,
    STATE_ENDED = 4,
    STATE_COUNT = 5
};

enum {
    SETTLE_MS = 150, /* let the cleared screen settle */
    LOOP_MS = 20,
    OP_POLL_MS = 20,  /* ask a pending select or stop this often */
    SILENT_MS = 3000, /* result() failing this long = Stop */
    MS_PER_S = 1000,
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    PERCENT_PER_LED = 10,
    TEXT_CHARS = 128, /* a SET_TEXT carries at most 126 bytes */
    CLOCK_CHARS = 36, /* "HH:MM:SS" of any three int32_t */
    PAUSE_BUTTON = 1, /* SW5, the lower button */
    ALARM_BUTTON = 0, /* SW6, the upper button: ACK */
    ALARM_BURSTS = 2, /* bursts within T3_MS that make the alarm */
    BURSTS_MAX = 16   /* more than T3_MS can hold at POLL_MS */
};

/* Layout on the 792 x 398 canvas. */
enum {
    AUTO = -1, /* x, y, w or h the .py leaves out */
    LABEL_PX = 16,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 14,
    CARD_TITLE_DY = 8,
    METER_X = 28,
    METER_STEP_PX = 44,
    LINES_Y = 166,
    LINE_STEP_PX = 24
};

enum {
    LOG_LINES = 4,
    METER_LEDS = 10,
    SLOT_HEARTBEAT = 0,
    SLOT_LEFT_CARD = 1,
    SLOT_DISC = 2,
    SLOT_VERDICT = 3,
    SLOT_STATE = 4,
    SLOT_METER = 5, /* METER_LEDS slots */
    SLOT_LEGEND = SLOT_METER + METER_LEDS,
    SLOT_SNOOZE = SLOT_LEGEND + 1,
    SLOT_RIGHT_CARD = SLOT_SNOOZE + 1,
    SLOT_ALARMS = SLOT_RIGHT_CARD + 1,
    SLOT_LINES = SLOT_ALARMS + 1, /* LOG_LINES slots */
    SLOT_ACKNOWLEDGE = SLOT_LINES + LOG_LINES,
    SLOT_STATUS = SLOT_ACKNOWLEDGE + 1,
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

/* The program's state: too big for the 8 KB stack. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT + 1]; /* + the spare for SLOT_NONE */
    sent_t sent[SLOT_COUNT];
    int32_t lit; /* LEDs of the meter that are on */
    bool has_buttons;
    uint32_t model_index;
    int32_t model_count; /* rows when the model was found */
    uint32_t label_count;
    char labels[MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
    char log[LOG_LINES][TEXT_CHARS];
    uint32_t log_count;
    reader_t reader;
    char stop_text[TEXT_CHARS]; /* "" until the program stops itself */
} app_t;

/* One board button: one event per press, holding does not repeat. */
typedef struct {
    uint32_t index;
    bool down;
    bool clicked;
} board_button_t;

/* The last VOTE_WINDOW scores. */
typedef struct {
    int32_t scores[VOTE_WINDOW];
    uint32_t count;
} vote_t;

/* The alarm: NORMAL > HEARD > UNACK > ACKED or ENDED, needs ACK. */
typedef struct {
    uint32_t state;
    int32_t count;
    bool has_time;
    uint32_t time_ms;
    char end[CLOCK_CHARS];
} episode_t;

/* The strongest event class and its thresholds. */
typedef struct {
    uint32_t klass;
    int32_t percent;
    int32_t hearing;
    int32_t confirm;
    int32_t clear;
} event_t;

/* The loop's state. */
typedef struct {
    bento_edgeai_thresholds_t thresholds;
    board_button_t pause_button;
    board_button_t alarm_button;
    vote_t vote;
    episode_t alarm;
    uint32_t bursts[BURSTS_MAX]; /* when each burst began */
    uint32_t burst_count;
    bool was;            /* in a burst */
    bool has_off;        /* a burst ended ... */
    uint32_t off_ms;     /* ... at this time */
    bool shelved;        /* Snooze was tapped ... */
    uint32_t shelved_ms; /* ... at this time */
    bool paused;
    bool blink;
    int32_t peak; /* top score since the last draw, -1 = none */
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
    static const bool builtin = true; /* BUILTIN */
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

/* The last N scores; K of them at or above a level = confirmed. */
static void vote_add(vote_t *vote, int32_t percent)
{
    uint32_t i;

    if (vote->count == (uint32_t)VOTE_WINDOW) {
        for (i = 1U; i < (uint32_t)VOTE_WINDOW; i++) {
            vote->scores[i - 1U] = vote->scores[i];
        }
        vote->count--;
    }
    vote->scores[vote->count] = percent;
    vote->count++;
}

static bool vote_hits(const vote_t *vote, int32_t level)
{
    uint32_t hits = 0U;
    uint32_t i;

    for (i = 0U; i < vote->count; i++) {
        hits += (vote->scores[i] >= level) ? 1U : 0U;
    }
    return hits >= (uint32_t)VOTE_NEED;
}

static int32_t vote_max(const vote_t *vote)
{
    int32_t top = 0;
    uint32_t i;

    for (i = 0U; i < vote->count; i++) {
        if ((i == 0U) || (vote->scores[i] > top)) {
            top = vote->scores[i];
        }
    }
    return top;
}

/* A full window, all of it below the level. */
static bool vote_clear(const vote_t *vote, int32_t level)
{
    return (vote->count == (uint32_t)VOTE_WINDOW) &&
           (vote_max(vote) < level);
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

/* Where the alarm goes once the event is no longer heard. */
static uint32_t settled_state(episode_t *episode, bool heard,
                              bool clear)
{
    uint32_t state = episode->state;

    if (clear) {
        if (state == STATE_UNACK) {
            state = STATE_ENDED;
            clock_text(episode->end, sizeof(episode->end));
        } else if (state != STATE_ENDED) {
            state = STATE_NORMAL;
        } else {
            /* ENDED waits for ACK */
        }
    } else if (heard && (state == STATE_NORMAL)) {
        state = STATE_HEARD;
    } else {
        /* no change */
    }
    return state;
}

/* True once per new event; refractory from the episode's last
 * confirm. */
static bool episode_step(episode_t *episode, bool heard, bool confirm,
                         bool clear, uint32_t now_ms)
{
    const bool alarmed = (episode->state == STATE_UNACK) ||
                         (episode->state == STATE_ACKED);
    const bool rested =
        !episode->has_time ||
        (bento_time_ticks_diff(now_ms, episode->time_ms) >= REFRACT_MS);
    bool is_new = false;

    if (confirm && !alarmed && rested) {
        episode->state = STATE_UNACK;
        episode->count++;
        episode->has_time = true;
        episode->time_ms = now_ms;
        is_new = true;
    } else {
        if (confirm && alarmed) {
            episode->time_ms = now_ms;
        }
        episode->state = settled_state(episode, heard, clear);
    }
    return is_new;
}

static void episode_acknowledge(episode_t *episode)
{
    if (episode->state == STATE_UNACK) {
        episode->state = STATE_ACKED;
    } else if (episode->state == STATE_ENDED) {
        episode->state = STATE_NORMAL;
    } else {
        /* nothing to acknowledge */
    }
}

/* The strongest EVENT class (class 0 = nothing) and its
 * thresholds. */
static void event_of(const bento_edgeai_result_t *result,
                     const bento_edgeai_thresholds_t *thresholds,
                     event_t *event)
{
    const uint32_t count = (result->class_count < (uint8_t)MAX_CLASSES)
                               ? result->class_count
                               : (uint32_t)MAX_CLASSES;
    uint32_t strongest = 0U;
    uint32_t klass;

    for (klass = 1U; klass < count; klass++) {
        if ((result->score_pct[klass] > result->score_pct[strongest]) ||
            (strongest == 0U)) {
            strongest = klass;
        }
    }
    event->klass = strongest;
    event->percent = result->score_pct[strongest];
    event->hearing = thresholds->enter[strongest];
    event->confirm = thresholds->confirm[strongest];
    event->clear = thresholds->exit[strongest];
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

/* 10 LEDs; switch only the ones that change. */
static void show_meter(app_t *app, int32_t percent)
{
    int32_t lit = (percent + (PERCENT_PER_LED / 2)) / PERCENT_PER_LED;
    const int32_t low = (lit < app->lit) ? lit : app->lit;
    const int32_t high = (lit > app->lit) ? lit : app->lit;
    int32_t i;

    if (lit > METER_LEDS) {
        lit = METER_LEDS; /* a score is at most 100 % */
    }
    for (i = low; (i < high) && (i < METER_LEDS); i++) {
        set_value(app, (uint32_t)SLOT_METER + (uint32_t)i,
                  (i < lit) ? 1 : 0);
    }
    app->lit = lit;
}

/* The card shown when the model is not on this board. */
static bento_status_t show_missing(app_t *app)
{
    static const widget_spec_t card[] = {
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 96, 110, 600, 180,
         COLOR_CARD, COLOR_BAD, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Model not on this board", 120,
         126, AUTO, AUTO, COLOR_BAD, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 120, 170, AUTO, AUTO,
         COLOR_TEXT, 0, 0, 20},
    };
    char text[TEXT_CHARS];
    bento_status_t status;

    (void)snprintf(text, sizeof(text),
                   "%s\nis built into the TESAIoT Dev Kit.\n"
                   "Not on this board.",
                   MODEL_NAME);
    (void)printf("%s\n", text);
    status = clear_screen(app);
    if (status == BENTO_OK) {
        status = add_widgets(app, card, sizeof(card) / sizeof(card[0]));
    }
    if (status == BENTO_OK) {
        set_text(app, SLOT_STATUS, text); /* .text() fits 126 bytes */
        drain_events(app->ui);
    }
    return status;
}

/* ---- 4) This model's screen (T1 annunciator) ---- */

/* The title, the heartbeat and the "Now" card down to its state. */
static bento_status_t add_now_card(app_t *app)
{
    static const widget_spec_t top[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Alarm Listener", 12, 4, AUTO,
         AUTO, COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LED, SLOT_HEARTBEAT, NULL, 756, 6, 26, 26, COLOR_OK,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Teaching demo - NOT a safety device. Detects alarm beeps, "
         "not "
         "smoke or CO.",
         12, 38, AUTO, AUTO, COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_LEFT_CARD, NULL, 12, 64, 470, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Now", 12 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        /* a round Panel: an Led keeps its first colour on the board */
        {UI_WIDGET_PANEL, SLOT_DISC, NULL, 28, 100, 72, 72, COLOR_DIM,
         COLOR_DIM, 72 / 2, 0},
        {UI_WIDGET_LABEL, SLOT_VERDICT, "Quiet", 116, 104, AUTO, AUTO,
         COLOR_DIM, 0, 0, 28},
        {UI_WIDGET_LABEL, SLOT_STATE, " ", 116, 144, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
    };

    return add_widgets(app, top, sizeof(top) / sizeof(top[0]));
}

/* The meter's LEDs, the legend and the Snooze button. */
static bento_status_t add_meter(app_t *app)
{
    static const widget_spec_t led = {
        UI_WIDGET_LED, SLOT_METER, NULL, METER_X, 190, 36, 20,
        COLOR_TEXT,    0,          0,    0};
    static const widget_spec_t legend = {
        UI_WIDGET_LABEL, SLOT_LEGEND, " ", 28,      218, AUTO, AUTO,
        COLOR_DIM,       0,           0,   LABEL_PX};
    static const widget_spec_t snooze_button = {
        UI_WIDGET_BUTTON, SLOT_SNOOZE, NULL, 336, 248, 132, 76,
        COLOR_BUTTON,     0,           0,    16};
    widget_spec_t spec = led;
    char text[TEXT_CHARS];
    int32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0; (i < METER_LEDS) && (status == BENTO_OK); i++) {
        spec.slot = (uint8_t)(SLOT_METER + i);
        spec.x = (int16_t)(METER_X + (METER_STEP_PX * i));
        status = add_widget(app, &spec);
    }
    if (status == BENTO_OK) {
        status = add_widget(app, &legend);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "Snooze %d s", SNOOZE_S);
        spec = snooze_button;
        spec.text = text;
        status = add_widget(app, &spec);
    }
    return status;
}

/* The "Alarms" card: the count and the four newest lines. */
static bento_status_t add_alarms_card(app_t *app)
{
    static const widget_spec_t card[] = {
        {UI_WIDGET_PANEL, SLOT_RIGHT_CARD, NULL, 494, 64, 294, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Alarms", 494 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_SEG7, SLOT_ALARMS, "0", 508, 96, 120, 56, COLOR_TEXT,
         0, 0, 0},
    };
    static const widget_spec_t line = {
        UI_WIDGET_LABEL, SLOT_LINES, " ", 508,     LINES_Y, AUTO, AUTO,
        COLOR_TEXT,      0,          0,   LABEL_PX};
    widget_spec_t spec = line;
    int32_t i;
    bento_status_t status =
        add_widgets(app, card, sizeof(card) / sizeof(card[0]));

    for (i = 0; (i < LOG_LINES) && (status == BENTO_OK); i++) {
        spec.slot = (uint8_t)(SLOT_LINES + i);
        spec.y = (int16_t)(LINES_Y + (LINE_STEP_PX * i));
        status = add_widget(app, &spec);
    }
    return status;
}

static bento_status_t build_screen(app_t *app)
{
    static const widget_spec_t bottom[] = {
        {UI_WIDGET_BUTTON, SLOT_ACKNOWLEDGE, "ACK", 508, 266, 266, 60,
         COLOR_BUTTON, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 12, 346, AUTO, AUTO,
         COLOR_WARN, 0, 0, LABEL_PX},
    };
    bento_status_t status = clear_screen(app);

    if (status == BENTO_OK) {
        status = add_now_card(app);
    }
    if (status == BENTO_OK) {
        status = add_meter(app);
    }
    if (status == BENTO_OK) {
        status = add_alarms_card(app);
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

/* The state line: the score, or what the alarm waits for. */
static void state_text(const episode_t *alarm, int32_t percent,
                       uint32_t bursts, char *text, size_t cap)
{
    if (alarm->state == STATE_NORMAL) {
        (void)snprintf(text, cap, "score %" PRId32 "%%", percent);
    } else if (alarm->state == STATE_HEARD) {
        (void)snprintf(text, cap,
                       "! hearing %" PRId32 "%% - bursts %" PRIu32,
                       percent, bursts);
    } else if (alarm->state == STATE_UNACK) {
        (void)snprintf(text, cap, "!! ALARM SOUND - tap ACK");
    } else if (alarm->state == STATE_ACKED) {
        (void)snprintf(text, cap, "ACKED - still hearing");
    } else {
        (void)snprintf(text, cap, "ENDED %s - tap ACK", alarm->end);
    }
}

/* UNACK and ENDED flash at 1 Hz. */
static uint32_t disc_color(uint32_t state, bool blink)
{
    static const uint32_t lit_colors[STATE_COUNT] = {
        COLOR_DIM, COLOR_WARN, COLOR_BAD, COLOR_BAD, COLOR_DIM};
    static const uint32_t dark_colors[STATE_COUNT] = {
        COLOR_DIM, COLOR_WARN, COLOR_ALARM_BACK, COLOR_BAD,
        COLOR_ALARM_BACK};

    return blink ? lit_colors[state] : dark_colors[state];
}

/* Both cards take over in dark red until ACK. */
static void draw(app_t *app, const run_t *run)
{
    static const char *const verdicts[STATE_COUNT] = {
        "Quiet", "Hearing", "ALARM SOUND", "ALARM SOUND", "Quiet"};
    static const uint32_t verdict_colors[STATE_COUNT] = {
        COLOR_DIM, COLOR_WARN, COLOR_BAD, COLOR_BAD, COLOR_DIM};
    static const uint32_t state_colors[STATE_COUNT] = {
        COLOR_DIM, COLOR_WARN, COLOR_TEXT, COLOR_TEXT, COLOR_TEXT};
    const uint32_t state = run->alarm.state;
    const int32_t percent = (run->peak > 0) ? run->peak : 0;
    const bool flashing =
        (state == STATE_UNACK) || (state == STATE_ENDED);
    const uint32_t card = flashing ? COLOR_ALARM_BACK : COLOR_CARD;
    char text[TEXT_CHARS];

    set_value(app, SLOT_HEARTBEAT,
              (run->blink && (run->peak >= 0)) ? 1 : 0);
    put_color(app, SLOT_LEFT_CARD, card);
    put_color(app, SLOT_RIGHT_CARD, card);
    put_color(app, SLOT_DISC, disc_color(state, run->blink));
    state_text(&run->alarm, percent, run->burst_count, text,
               sizeof(text));
    put(app, SLOT_STATE, text, state_colors[state]);
    put(app, SLOT_VERDICT, verdicts[state], verdict_colors[state]);
    put_color(app, SLOT_ACKNOWLEDGE,
              flashing ? COLOR_BAD : COLOR_BUTTON);
    show_meter(app, percent);
}

/* ---- 5) Main program ---- */

static const char *help_text(const app_t *app)
{
    return app->has_buttons ? "Listening - SW6 = ACK, SW5 = pause"
                            : "Listening";
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

/* A new alarm: the log line and the count. */
static void log_alarm(app_t *app, const run_t *run, uint32_t klass)
{
    const char *label =
        (klass < app->label_count) ? app->labels[klass] : "";
    char stamp[CLOCK_CHARS];
    char text[TEXT_CHARS];
    uint32_t i;

    clock_text(stamp, sizeof(stamp));
    for (i = (uint32_t)LOG_LINES - 1U; i > 0U; i--) {
        (void)memcpy(app->log[i], app->log[i - 1U],
                     sizeof(app->log[i]));
    }
    (void)snprintf(app->log[0], sizeof(app->log[0]),
                   "%s %s %" PRId32 "%% #%" PRId32, stamp, label,
                   vote_max(&run->vote), run->alarm.count);
    if (app->log_count < (uint32_t)LOG_LINES) {
        app->log_count++;
    }
    (void)printf("%s\n", app->log[0]);
    for (i = 0U; i < app->log_count; i++) { /* newest first */
        put_text(app, (uint32_t)SLOT_LINES + i, app->log[i]);
    }
    (void)snprintf(text, sizeof(text), "%" PRId32, run->alarm.count);
    put_text(app, SLOT_ALARMS, text);
}

/* A full burst table forgets its oldest burst. */
static void drop_oldest_burst(run_t *run)
{
    uint32_t i;

    for (i = 1U; i < run->burst_count; i++) {
        run->bursts[i - 1U] = run->bursts[i];
    }
    run->burst_count--;
}

/* In a beep burst or not: a new burst starts only after GAP_MS of
 * quiet (a shorter dip is the same burst); bursts older than T3_MS
 * are forgotten. */
static void track_bursts(run_t *run, bool on, uint32_t now_ms)
{
    uint32_t kept = 0U;
    uint32_t i;

    if (on && !run->was &&
        (!run->has_off ||
         (bento_time_ticks_diff(now_ms, run->off_ms) >= GAP_MS))) {
        if (run->burst_count == (uint32_t)BURSTS_MAX) {
            drop_oldest_burst(run);
        }
        run->bursts[run->burst_count] = now_ms;
        run->burst_count++;
    } else if (run->was && !on) {
        run->has_off = true;
        run->off_ms = now_ms;
    } else {
        /* the same burst, or the same quiet */
    }
    run->was = on;
    for (i = 0U; i < run->burst_count; i++) {
        if (bento_time_ticks_diff(now_ms, run->bursts[i]) <= T3_MS) {
            run->bursts[kept] = run->bursts[i];
            kept++;
        }
    }
    run->burst_count = kept;
}

/* One new result: the vote, the bursts and the alarm. */
static void take_result(app_t *app, run_t *run,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    event_t event;
    bool clear;

    event_of(result, &run->thresholds, &event);
    vote_add(&run->vote, event.percent);
    if (event.percent > run->peak) {
        run->peak = event.percent;
    }
    track_bursts(run, vote_hits(&run->vote, event.confirm), now_ms);
    /* alarm = 2 bursts in T3_MS; over after T3_MS without one */
    clear =
        (run->burst_count == 0U) && vote_clear(&run->vote, event.clear);
    if (episode_step(&run->alarm, event.percent >= event.hearing,
                     run->burst_count >= (uint32_t)ALARM_BURSTS, clear,
                     now_ms)) {
        log_alarm(app, run, event.klass);
    }
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

/* Every DRAW_MS: the blink, the end of Snooze, the status line and
 * the screen. */
static void redraw(app_t *app, run_t *run, uint32_t now_ms)
{
    char text[TEXT_CHARS];
    int32_t left_s = 0;

    run->drawn_ms = now_ms;
    run->blink = !run->blink;
    if (run->shelved) {
        left_s =
            SNOOZE_S -
            (bento_time_ticks_diff(now_ms, run->shelved_ms) / MS_PER_S);
    }
    if ((left_s <= 0) && run->shelved) {
        run->shelved = false;
        if (run->alarm.state == STATE_ACKED) {
            run->alarm.state = STATE_UNACK; /* still sounding when
                                               Snooze ends: announce it
                                               again */
        }
    }
    if (!run->paused && (left_s > 0)) {
        (void)snprintf(text, sizeof(text),
                       "SHELVED %" PRId32 " s - still counting",
                       left_s);
        put(app, SLOT_STATUS, text, COLOR_WARN);
    } else if (!run->paused) {
        put(app, SLOT_STATUS, help_text(app), COLOR_TEXT);
    } else {
        /* paused: the status line says so */
    }
    draw(app, run);
    run->peak = -1;
}

/* Taps: ACK, and Snooze shelving the alarm. The answer: ACK was
 * tapped. A failed poll answers no events. */
static bool read_taps(app_t *app, run_t *run)
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
        const bool clicked =
            is_kind(&events[i], BENTO_UI_EVENT_CLICKED);

        if (clicked && (events[i].id == app->ids[SLOT_ACKNOWLEDGE])) {
            tapped = true;
        }
        if (clicked && (events[i].id == app->ids[SLOT_SNOOZE])) {
            run->shelved = true;
            run->shelved_ms = bento_time_ticks_ms();
        }
    }
    return tapped;
}

/* SW5: pause stops the AI core; resume starts it again. */
static bento_status_t toggle_pause(app_t *app, run_t *run)
{
    bento_status_t status;

    run->paused = !run->paused;
    run->vote.count = 0U;
    run->was = false;
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

/* One pass of the loop: buttons, taps, pause, results, ACK and
 * drawing. */
static bento_status_t run_once(app_t *app, run_t *run)
{
    uint32_t now_ms;
    bool acknowledged = false;
    bento_status_t status = sample_button(app, &run->pause_button);

    if (status == BENTO_OK) {
        status = sample_button(app, &run->alarm_button);
    }
    if (status == BENTO_OK) {
        acknowledged = pressed_now(&run->alarm_button);
        if (read_taps(app, run)) {
            acknowledged = true;
        }
        if (pressed_now(&run->pause_button)) {
            status = toggle_pause(app, run);
        }
    }
    now_ms = bento_time_ticks_ms();
    if ((status == BENTO_OK) && !run->paused &&
        (bento_time_ticks_diff(now_ms, run->polled_ms) >= POLL_MS)) {
        run->polled_ms = now_ms;
        status = poll_model(app, run, now_ms);
    }
    if ((status == BENTO_OK) && (acknowledged || run->shelved)) {
        episode_acknowledge(&run->alarm); /* shelved: counted, never
                                             flashes */
    }
    if ((status == BENTO_OK) &&
        (bento_time_ticks_diff(now_ms, run->drawn_ms) >= DRAW_MS)) {
        redraw(app, run, now_ms);
    }
    return status;
}

static bento_status_t run_model(app_t *app)
{
    run_t run;
    char text[TEXT_CHARS];
    bento_status_t status;

    (void)memset(&run, 0, sizeof(run));
    read_thresholds(app->model_index, &run.thresholds);
    (void)snprintf(
        text, sizeof(text), "Hearing at %d%% | Confirm at %d%%",
        (int)run.thresholds.enter[1], (int)run.thresholds.confirm[1]);
    put_text(app, SLOT_LEGEND, text);
    run.pause_button.index = PAUSE_BUTTON;
    run.alarm_button.index = ALARM_BUTTON;
    run.peak = -1;
    status = start_model(app, false);
    run.drawn_ms = bento_time_ticks_ms();
    run.polled_ms = run.drawn_ms;
    while (status == BENTO_OK) {
        status = run_once(app, &run);
        if (status == BENTO_OK) {
            status = wait_ms(LOOP_MS);
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
        status = find_model(app) ? watch_model(app) : show_missing(app);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
