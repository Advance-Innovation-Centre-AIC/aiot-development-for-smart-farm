/*
 * siren_detection_store.c -- Siren A/B: เทียบโมเดลไซเรนจาก Store กับ
 * โมเดลในตัว
 *
 * เทียบสองโมเดลแบบ A/B ทีละตัว: A = "SirenDetection" จาก Edge AI Store
 * (ต้อง deploy ลงบอร์ดก่อน) B = "Siren Detection" ที่มากับบอร์ด แต่ละ
 * การ์ดนับไซเรนตามเกณฑ์ Confirm at ของตัวเอง บอกคะแนนตอนนี้ คะแนนสูงสุด
 * และความหน่วงจากคะแนนแรกที่ถึงเกณฑ์จนถึงการเตือน เปิดเสียงไซเรน
 * แตะ Swap แล้วเปิดเสียงเดิมอีกครั้ง แตะ ACK หรือกด SW6 (ปุ่มบน)
 * เพื่อรับทราบ SW5 (ปุ่มล่าง) หยุดชั่วคราว/ทำต่อ
 *
 * Compares two siren models, one at a time: A = "SirenDetection"
 * from the Edge AI Store (deploy it first), B = the built-in "Siren
 * Detection". Each card counts sirens at its own Confirm at and
 * shows the score now, its top score and the delay from the first
 * score at Confirm at to the alarm. Play a siren clip, tap Swap,
 * play it again. Tap ACK or press SW6 (upper) to acknowledge; SW5
 * (lower) = pause/resume.
 *
 * Twin of siren_detection_store.py (aiot-development-for-smart-farm/s3/models/siren_detection_store.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - edge_ai.models() is read row by row (count, model_info, label);
 *     a row is built in when the firmware compiled it in
 *     (diag.compiled_models), unknown (the .py's None) when diag
 *     cannot say. A label that cannot be read is "".
 *   - The global MODEL and BUILTIN the .py changes are the name and
 *     source passed to find_model(), and the name of the running
 *     card kept in the program's state for the status texts.
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
 *   - Both cards' alarms are latched (Episode(True)), so the info
 *     state SEEN is left out.
 *   - A score is the board's whole percent: int(score * 100 + 0.5)
 *     gives it back unchanged, so the percent is used as it comes.
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
/* A = Store, B = built-in: one space apart, two models. */
static const char *const NAMES[] = {"SirenDetection",
                                    "Siren Detection"};

enum {
    VOTE_NEED = 3,        /* an event = 3 of ... */
    VOTE_WINDOW = 5,      /* ... the last 5 results at Confirm at */
    REFRACT_MS = 1500,    /* no second event sooner than this */
    HOLD_MS = 2000,       /* not used by a latched alarm */
    DEFAULT_HEARING = 40, /* Hearing at, when the board cannot say */
    DEFAULT_CONFIRM = 65, /* Confirm at */
    DEFAULT_CLEAR = 30,   /* clear below */
    POLL_MS = 100,
    DRAW_MS = 500
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

/* Times, counts and sizes. */
enum {
    SETTLE_MS = 150, /* let the cleared screen settle */
    LOOP_MS = 20,
    OP_POLL_MS = 20,  /* ask a pending select or stop this often */
    SILENT_MS = 3000, /* result() failing this long = Stop */
    MS_PER_S = 1000,
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    SIDE_A = 0,
    SIDE_B = 1,
    TEXT_CHARS = 128, /* a SET_TEXT carries at most 126 bytes */
    CLOCK_CHARS = 36, /* "HH:MM:SS" of any three int32_t */
    PAUSE_BUTTON = 1, /* SW5, the lower button */
    ALARM_BUTTON = 0  /* SW6, the upper button: ACK */
};

/* Layout on the 792 x 398 canvas. */
enum {
    AUTO = -1, /* x, y, w or h the .py leaves out */
    LABEL_PX = 16,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 14,
    CARD_TITLE_DY = 8,
    SIDE_X = 28,        /* card A's content ... */
    SIDE_STEP_PX = 394, /* ... and card B's, this far right */
    CARD_INSET = 16,    /* the card starts this far left of it */
    DISC_PX = 56,
    WORD_DX = 68
};

/* Every widget the program changes later has a slot; each card has
 * SIDE_SLOTS of them. */
enum {
    SIDES = 2, /* A and B */
    SIDE_DISC = 0,
    SIDE_WORD = 1,
    SIDE_NOW = 2,
    SIDE_TALLY = 3,
    SIDE_DELAY = 4,
    SIDE_SLOTS = 5,
    SLOT_HEARTBEAT = 0,
    SLOT_SIDES = 1, /* SIDES * SIDE_SLOTS slots */
    SLOT_SWAP = SLOT_SIDES + (SIDES * SIDE_SLOTS),
    SLOT_ACKNOWLEDGE = SLOT_SWAP + 1,
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

/* One card: its model (index, labels) or none, alarm and tallies. */
typedef struct {
    bool has_model;
    uint32_t index;
    uint32_t label_count;
    char labels[MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
    bento_edgeai_thresholds_t thresholds;
    vote_t vote;
    episode_t alarm;
    int32_t top;       /* the top score this card has heard */
    int32_t delay_ms;  /* onset -> alarm of the last siren, -1 = none */
    bool has_onset;    /* a score reached Confirm at ... */
    uint32_t onset_ms; /* ... at this time */
} side_t;

/* The program's state: too big for the 8 KB stack. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT + 1]; /* + the spare for SLOT_NONE */
    sent_t sent[SLOT_COUNT];
    bool has_buttons;
    const char *model_name; /* the card that runs, for the status */
    int32_t model_count;    /* rows when model A was found */
    side_t sides[SIDES];
    reader_t reader;
    char stop_text[TEXT_CHARS]; /* "" until the program stops itself */
} app_t;

/* The loop's state. */
typedef struct {
    board_button_t pause_button;
    board_button_t alarm_button;
    uint32_t running; /* the card that listens */
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

/* Built in or Store as asked; a row the board cannot place (None on
 * the emulator) counts as built in. */
static bool source_matches(int32_t row, int32_t compiled, bool builtin)
{
    bool matches;

    if (builtin) {
        matches = (compiled < 0) || (row < compiled);
    } else {
        matches = (compiled >= 0) && (row >= compiled);
    }
    return matches;
}

/* The model's class names, "" for one that cannot be read. */
static void read_labels(side_t *side,
                        const bento_edgeai_model_info_t *info)
{
    uint32_t length = 0U;
    uint32_t i;

    side->label_count = info->class_count;
    if (side->label_count > (uint32_t)MAX_CLASSES) {
        side->label_count = MAX_CLASSES;
    }
    for (i = 0U; i < side->label_count; i++) {
        if (bento_edgeai_label(side->index, i, side->labels[i],
                               BENTO_EDGEAI_LABEL_LEN,
                               &length) != BENTO_OK) {
            side->labels[i][0] = '\0';
        }
    }
}

/* Every row, as edge_ai.models() reads them; the first one with the
 * exact name and the right source is the card's model. */
static bento_status_t read_rows(const char *name, bool builtin,
                                side_t *side, int32_t *count)
{
    bento_edgeai_model_info_t info;
    int32_t row;
    bento_status_t status = bento_edgeai_count(count);
    const int32_t compiled =
        (status == BENTO_OK) ? compiled_count() : -1;

    for (row = 0; (row < *count) && (status == BENTO_OK); row++) {
        bento_edgeai_model_info_init(&info);
        status = bento_edgeai_model_info((uint32_t)row, &info);
        if ((status == BENTO_OK) && !side->has_model &&
            (strcmp(info.name, name) == 0) &&
            source_matches(row, compiled, builtin)) {
            side->has_model = true;
            side->index = (uint32_t)row;
            read_labels(side, &info);
        }
    }
    return status;
}

/* find_model(): a link that fails prints why, and then the card has
 * no model. *count: the rows read. */
static bool find_model(const char *name, bool builtin, side_t *side,
                       int32_t *count)
{
    const bento_status_t status = read_rows(name, builtin, side, count);

    if (status != BENTO_OK) {
        (void)printf("cannot read models: %s\n",
                     link_error_text(status));
        side->has_model = false;
    }
    return side->has_model;
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
static bento_status_t read_result(app_t *app, uint32_t index,
                                  bento_edgeai_result_t *result,
                                  bool *fresh)
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
                 (result->index == index);
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
                   "Deploy %s\nfrom edgeai-store.tesaiot.dev\n(sign in "
                   "with GitHub) on a Dev Kit.",
                   NAMES[SIDE_A]);
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

/* ---- 4) This model's screen (T3 A/B) ---- */

/* One card: its panel, title, light and four lines. The table is
 * card A's, x relative to the card's content and slots relative to
 * the card's first slot; card B is SIDE_STEP_PX to the right. */
static bento_status_t add_side(app_t *app, uint32_t side)
{
    static const char *const titles[SIDES] = {"A: %s (Store)",
                                              "B: %s (built-in)"};
    static const widget_spec_t card[] = {
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, -CARD_INSET, 64, 382, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, NULL, CARD_TITLE_DX - CARD_INSET,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        /* a round Panel: an Led keeps its first colour on the board */
        {UI_WIDGET_PANEL, SIDE_DISC, NULL, 0, 100, DISC_PX, DISC_PX,
         COLOR_DIM, COLOR_DIM, DISC_PX / 2, 0},
        {UI_WIDGET_LABEL, SIDE_WORD, "paused", WORD_DX, 108, AUTO, AUTO,
         COLOR_DIM, 0, 0, 28},
        {UI_WIDGET_LABEL, SIDE_NOW, " ", 0, 168, AUTO, AUTO, COLOR_TEXT,
         0, 0, 20},
        {UI_WIDGET_LABEL, SIDE_TALLY, " ", 0, 198, AUTO, AUTO,
         COLOR_TEXT, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SIDE_DELAY, " ", 0, 222, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
    };
    const int32_t x = SIDE_X + ((int32_t)side * SIDE_STEP_PX);
    const uint32_t first = SLOT_SIDES + (side * (uint32_t)SIDE_SLOTS);
    widget_spec_t spec;
    char title[TEXT_CHARS];
    uint32_t i;
    bento_status_t status = BENTO_OK;

    (void)snprintf(title, sizeof(title), titles[side], NAMES[side]);
    for (i = 0U;
         (i < (sizeof(card) / sizeof(card[0]))) && (status == BENTO_OK);
         i++) {
        spec = card[i];
        spec.x = (int16_t)(x + spec.x);
        if (spec.slot != (uint8_t)SLOT_NONE) {
            spec.slot = (uint8_t)(first + spec.slot);
        }
        if (i == 1U) {
            spec.text = title;
        }
        status = add_widget(app, &spec);
    }
    return status;
}

static bento_status_t build_screen(app_t *app)
{
    static const widget_spec_t top[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Siren: Store vs Built-in", 12, 4,
         AUTO, AUTO, COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LED, SLOT_HEARTBEAT, NULL, 756, 6, 26, 26, COLOR_OK,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Same siren, two models, one at a time. Each counts at its "
         "own "
         "Confirm at.",
         12, 38, AUTO, AUTO, COLOR_DIM, 0, 0, LABEL_PX},
    };
    static const widget_spec_t bottom[] = {
        {UI_WIDGET_BUTTON, SLOT_SWAP, "Swap: run B", 28, 272, 350, 56,
         COLOR_BUTTON, 0, 0, 20},
        {UI_WIDGET_BUTTON, SLOT_ACKNOWLEDGE, "ACK", 422, 272, 350, 56,
         COLOR_BUTTON, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 12, 346, AUTO, AUTO,
         COLOR_WARN, 0, 0, LABEL_PX},
    };
    uint32_t side;
    bento_status_t status = clear_screen(app);

    if (status == BENTO_OK) {
        status = add_widgets(app, top, sizeof(top) / sizeof(top[0]));
    }
    for (side = 0U; (side < (uint32_t)SIDES) && (status == BENTO_OK);
         side++) {
        status = add_side(app, side);
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

/* A real number as '%.1f' writes it on this board. */
static void tenths_text(bento_real_t value, char *text)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value, 1,
                                 &length) != BENTO_OK) {
        text[0] = '\0';
    }
}

/* The word of a card: its state while it listens, else "paused". */
static void show_word(app_t *app, const side_t *side, uint32_t first,
                      bool running)
{
    static const char *const words[STATE_COUNT] = {
        "Quiet", "! hearing", "!! SIREN", "ACKED", "ENDED "};
    static const uint32_t word_colors[STATE_COUNT] = {
        COLOR_DIM, COLOR_WARN, COLOR_BAD, COLOR_BAD, COLOR_TEXT};
    const uint32_t state = side->alarm.state;
    char text[TEXT_CHARS];

    if (!running) {
        (void)snprintf(text, sizeof(text), "paused");
    } else if (state == STATE_ENDED) {
        (void)snprintf(text, sizeof(text), "%s%s", words[state],
                       side->alarm.end);
    } else {
        (void)snprintf(text, sizeof(text), "%s", words[state]);
    }
    put(app, first + SIDE_WORD, text, word_colors[state]);
}

/* One card with a model: light, word, scores, count and delay. */
static void draw_side(app_t *app, uint32_t index, const run_t *run,
                      int32_t on)
{
    const side_t *side = &app->sides[index];
    const uint32_t first = SLOT_SIDES + (index * (uint32_t)SIDE_SLOTS);
    const bool running = (int32_t)index == on;
    const int32_t now_pct =
        (running && (run->peak > 0)) ? run->peak : 0;
    char delay[BENTO_FMT_BUF_SIZE];
    char text[TEXT_CHARS];

    put_color(app, first + SIDE_DISC,
              disc_color(side->alarm.state, run->blink));
    show_word(app, side, first, running);
    (void)snprintf(text, sizeof(text),
                   "now %" PRId32 "%% | max %" PRId32 "%%", now_pct,
                   side->top);
    put_text(app, first + SIDE_NOW, text);
    (void)snprintf(text, sizeof(text),
                   "sirens %" PRId32 " | Confirm at %d%%",
                   side->alarm.count, (int)side->thresholds.confirm[1]);
    put_text(app, first + SIDE_TALLY, text);
    if (side->delay_ms >= 0) {
        tenths_text((bento_real_t)side->delay_ms /
                        (bento_real_t)MS_PER_S,
                    delay);
        (void)snprintf(text, sizeof(text), "delay %s s", delay);
    } else {
        (void)snprintf(text, sizeof(text), "delay -");
    }
    put_text(app, first + SIDE_DELAY, text);
}

/* on = the listening card, -1 = paused. */
static void draw(app_t *app, const run_t *run, int32_t on)
{
    const uint32_t shown = (on >= 0) ? (uint32_t)on : 0U;
    const uint32_t state = app->sides[shown].alarm.state;
    uint32_t index;

    set_value(app, SLOT_HEARTBEAT,
              (run->blink && (run->peak >= 0)) ? 1 : 0);
    for (index = 0U; index < (uint32_t)SIDES; index++) {
        if (app->sides[index].has_model) {
            draw_side(app, index, run, on);
        }
    }
    put_color(app, SLOT_ACKNOWLEDGE,
              ((state == STATE_UNACK) || (state == STATE_ENDED))
                  ? COLOR_BAD
                  : COLOR_BUTTON);
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
                       app->model_name);
        status = raise_stop(app, text);
    } else if (count != app->model_count) {
        status = raise_stop(app, "Model list changed - run again");
    } else {
        /* the same list */
    }
    return status;
}

/* Loads and starts the model: up to 15 s. */
static bento_status_t select_model(app_t *app, uint32_t index)
{
    char text[TEXT_CHARS];
    bento_status_t status = bento_edgeai_select_start(index);
    const bool sent = status == BENTO_OK;

    if (sent) {
        status = finish_op();
    }
    if ((status != BENTO_OK) && (status != BENTO_E_CANCELLED)) {
        (void)printf("select: %s\n",
                     sent ? "edge_ai: select not confirmed"
                          : "edge_ai: select send failed");
        (void)snprintf(text, sizeof(text), "Cannot start %s",
                       app->model_name);
        status = raise_stop(app, text);
    }
    return status;
}

/* start(): on resume, Stop if a model was added or removed
 * meanwhile. */
static bento_status_t start_model(app_t *app, uint32_t index,
                                  bool resuming)
{
    char text[TEXT_CHARS];
    bento_status_t status = BENTO_OK;

    (void)snprintf(text, sizeof(text), "Loading %s ...",
                   app->model_name);
    put(app, SLOT_STATUS, text, COLOR_WARN);
    drain_events(app->ui);
    app->reader.failing = false;
    if (resuming) {
        status = check_model_list(app);
    }
    if (status == BENTO_OK) {
        status = select_model(app, index);
    }
    if (status == BENTO_OK) {
        put(app, SLOT_STATUS, help_text(app), COLOR_TEXT);
    }
    return status;
}

/* A card that stops starts fresh. */
static void rest_side(side_t *side)
{
    side->vote.count = 0U;
    side->has_onset = false;
    side->alarm.state = STATE_NORMAL;
}

/* One new result for the running card: the vote, the onset, the
 * alarm and its delay. */
static void take_result(app_t *app, run_t *run,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    side_t *side = &app->sides[run->running];
    char stamp[CLOCK_CHARS];
    event_t event;

    event_of(result, &side->thresholds, &event);
    vote_add(&side->vote, event.percent);
    if (event.percent > run->peak) {
        run->peak = event.percent;
    }
    if (event.percent > side->top) {
        side->top = event.percent;
    }
    if (vote_clear(&side->vote, event.clear)) {
        side->has_onset = false;
    } else if ((event.percent >= event.confirm) && !side->has_onset) {
        side->has_onset =
            true; /* onset: delay = the price of the rule */
        side->onset_ms = now_ms;
    } else {
        /* the onset stays */
    }
    if (episode_step(&side->alarm, event.percent >= event.hearing,
                     vote_hits(&side->vote, event.confirm),
                     vote_clear(&side->vote, event.clear), now_ms)) {
        side->delay_ms = bento_time_ticks_diff(
            now_ms, side->has_onset ? side->onset_ms : now_ms);
        clock_text(stamp, sizeof(stamp));
        (void)printf("%s %c %s %" PRId32 "%% #%" PRId32 "\n", stamp,
                     (run->running == (uint32_t)SIDE_A) ? 'A' : 'B',
                     (event.klass < side->label_count)
                         ? side->labels[event.klass]
                         : "",
                     vote_max(&side->vote), side->alarm.count);
    }
}

/* Every POLL_MS while not paused. */
static bento_status_t poll_model(app_t *app, run_t *run,
                                 uint32_t now_ms)
{
    bento_edgeai_result_t result;
    bool fresh = false;
    const bento_status_t status = read_result(
        app, app->sides[run->running].index, &result, &fresh);

    if ((status == BENTO_OK) && fresh) {
        take_result(app, run, &result, now_ms);
    }
    return status;
}

/* Taps on ACK and Swap; any kind of event counts (the .py reads
 * only the handle). A failed poll answers no events. */
static void read_taps(const app_t *app, bool *acknowledged, bool *swap)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    uint32_t i;

    if (bento_ui_poll(app->ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                      NULL) != BENTO_OK) {
        count = 0U;
    }
    for (i = 0U; i < count; i++) {
        if (events[i].id == app->ids[SLOT_ACKNOWLEDGE]) {
            *acknowledged = true;
        }
        if (events[i].id == app->ids[SLOT_SWAP]) {
            *swap = true;
        }
    }
}

/* Drops what was pressed while the model loaded: no swap back, no
 * ACK of the new card. */
static bento_status_t forget_presses(app_t *app, run_t *run)
{
    bento_status_t status;

    drain_events(app->ui);
    status = sample_button(app, &run->pause_button);
    if (status == BENTO_OK) {
        status = sample_button(app, &run->alarm_button);
    }
    (void)pressed_now(&run->pause_button); /* the press is dropped */
    (void)pressed_now(&run->alarm_button); /* the press is dropped */
    return status;
}

/* Swap: the other card's model runs. */
static bento_status_t swap_side(app_t *app, run_t *run)
{
    char stamp[CLOCK_CHARS];
    char text[TEXT_CHARS];
    bento_status_t status;

    rest_side(&app->sides[run->running]);
    status = stop_ai();
    if (status == BENTO_OK) {
        run->running = (uint32_t)SIDES - 1U - run->running;
        app->model_name = NAMES[run->running]; /* start() shows it */
        clock_text(stamp, sizeof(stamp));
        (void)printf("%s swap: run %s\n", stamp, app->model_name);
        status = start_model(app, app->sides[run->running].index, true);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "Swap: run %c",
                       (run->running == (uint32_t)SIDE_A) ? 'B' : 'A');
        put_text(app, SLOT_SWAP, text);
        status = forget_presses(app, run);
    }
    return status;
}

/* SW5: pause stops the AI core; resume starts it again. */
static bento_status_t toggle_pause(app_t *app, run_t *run)
{
    side_t *side = &app->sides[run->running];
    bento_status_t status;

    run->paused = !run->paused;
    rest_side(side);
    if (run->paused) {
        status = stop_ai();
        if (status == BENTO_OK) {
            put(app, SLOT_STATUS, "Paused - SW5 resumes", COLOR_WARN);
        }
    } else {
        status = start_model(app, side->index, true);
    }
    return status;
}

/* Buttons and taps: ACK, Swap and pause. */
static bento_status_t read_controls(app_t *app, run_t *run)
{
    const uint32_t other = (uint32_t)SIDES - 1U - run->running;
    bool acknowledged = false;
    bool swap = false;
    bento_status_t status = sample_button(app, &run->pause_button);

    if (status == BENTO_OK) {
        status = sample_button(app, &run->alarm_button);
    }
    if (status == BENTO_OK) {
        acknowledged = pressed_now(&run->alarm_button);
        read_taps(app, &acknowledged, &swap);
        if (acknowledged) {
            episode_acknowledge(&app->sides[run->running].alarm);
        }
        if (swap && app->sides[other].has_model && !run->paused) {
            status = swap_side(app, run);
        }
    }
    if ((status == BENTO_OK) && pressed_now(&run->pause_button)) {
        status = toggle_pause(app, run);
    }
    return status;
}

/* One pass of the loop: controls, results, drawing. */
static bento_status_t run_once(app_t *app, run_t *run)
{
    uint32_t now_ms;
    bento_status_t status = read_controls(app, run);

    now_ms = bento_time_ticks_ms();
    if ((status == BENTO_OK) && !run->paused &&
        (bento_time_ticks_diff(now_ms, run->polled_ms) >= POLL_MS)) {
        run->polled_ms = now_ms;
        status = poll_model(app, run, now_ms);
    }
    if ((status == BENTO_OK) &&
        (bento_time_ticks_diff(now_ms, run->drawn_ms) >= DRAW_MS)) {
        run->drawn_ms = now_ms;
        run->blink = !run->blink;
        draw(app, run, run->paused ? -1 : (int32_t)run->running);
        run->peak = -1;
    }
    return status;
}

/* Card B looks for the built-in model by the same exact-name rule;
 * each card with a model reads its thresholds. */
static void prepare_sides(app_t *app)
{
    int32_t count = 0;
    uint32_t index;

    (void)find_model(NAMES[SIDE_B], true, &app->sides[SIDE_B],
                     &count); /* B's row count is not kept */
    for (index = 0U; index < (uint32_t)SIDES; index++) {
        side_t *side = &app->sides[index];

        side->delay_ms = -1;
        if (side->has_model) {
            read_thresholds(side->index, &side->thresholds);
        }
    }
    if (!app->sides[SIDE_B].has_model) {
        put_text(app, SLOT_SIDES + SIDE_SLOTS + SIDE_WORD,
                 "not on this board");
        put_text(app, SLOT_SWAP, "B not on this board");
    }
}

static bento_status_t run_model(app_t *app)
{
    run_t run;
    bento_status_t status;

    (void)memset(&run, 0, sizeof(run));
    prepare_sides(app);
    run.pause_button.index = PAUSE_BUTTON;
    run.alarm_button.index = ALARM_BUTTON;
    run.running = SIDE_A;
    run.peak = -1;
    status = start_model(app, app->sides[SIDE_A].index, false);
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
    app->model_name = NAMES[SIDE_A];
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
        found = find_model(NAMES[SIDE_A], false, &app->sides[SIDE_A],
                           &app->model_count);
        status = found ? watch_model(app) : show_missing(app);
    }
    if ((status == BENTO_OK) && emulator && !found) {
        status = show_emulator_gap(app);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
