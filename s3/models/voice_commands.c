/*
 * voice_commands.c -- Steer the Cart: บังคับรถเข็นด้วยคำสั่งเสียง
 * (ต้องใช้ "Voice Commands" รุ่นทดลองจาก Edge AI Store)
 *
 * ใช้โมเดล "Voice Commands" (รุ่นทดลอง) จาก Edge AI Store ฟังคำภาษา
 * อังกฤษ left right up down stop go แล้วบังคับจุดบนลานขนาด 5 x 5 ช่อง:
 * คำบอกทิศทำให้หันและก้าวหนึ่งช่องเมื่อหยุดอยู่ go ให้วิ่ง 1 ช่องต่อ
 * วินาที stop ให้หยุด รั้วทำให้หยุด การ์ดขวาบอกคะแนนสามอันดับแรก
 * Safe stop (แตะปุ่มหรือกด SW6) ให้คำ stop หยุดรถได้ทันทีที่ถึงเกณฑ์
 * Hearing at SW5 หยุดชั่วคราว/ทำต่อ
 *
 * Runs "Voice Commands" (experimental) from the Edge AI Store on the
 * English words left right up down stop go and steers a dot on a
 * 5 x 5 yard: a direction turns it (and steps one cell when it is
 * stopped), go drives it at 1 cell/s, stop halts it, and so does
 * the fence. The right card shows the top 3 scores. Safe stop (tap
 * it or press SW6) lets "stop" halt the cart as soon as it reaches
 * Hearing at. SW5 = pause/resume.
 *
 * Twin of voice_commands.py (aiot-development-for-smart-farm/s3/models/voice_commands.py). Style: CLEAN_C.md.
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
 *     bento_buttons_count() 0. A board without the model link
 *     (reTerminal) cannot read the model list, so it shows the "not
 *     on this board" card, as the .py does there.
 *   - gc.collect() and gc.mem_free(): C has no garbage collector, so
 *     the "free heap:" line is not printed.
 *   - The dict of texts and colours already sent is one entry per
 *     widget slot; a text is kept up to TEXT_CHARS - 1 bytes.
 *   - A score is the board's whole percent: int(score * 100 + 0.5)
 *     gives it back unchanged, so the percent is used as it comes.
 *   - wd.upper() changes the ASCII letters a-z, which is what the
 *     model's labels hold.
 *   - max() of the "stop" votes right after a go cleared them would
 *     raise there; a new result always adds a score first, so it
 *     never happens, and vote_max() answers 0 for none.
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
static const char MODEL_NAME[] = "Voice Commands"; /* exact name */
/* BUILTIN (false = deployed from the Store): in source_matches() */

enum {
    VOTE_NEED = 2,        /* word = 2 of ... */
    VOTE_WINDOW = 4,      /* ... the last 4 results at Confirm at */
    REFRACT_MS = 1000,    /* no second word sooner than this */
    HOLD_MS = 2000,       /* a word stays on screen this long */
    DEFAULT_HEARING = 40, /* Hearing at, when the board cannot say */
    DEFAULT_CONFIRM = 65, /* Confirm at */
    DEFAULT_CLEAR = 30,   /* clear below */
    POLL_MS = 100,
    DRAW_MS = 500
};

/* The words that steer: a step of the cart for each. */
typedef struct {
    const char *word;
    int32_t dx;
    int32_t dy;
} direction_t;

static const direction_t DIRS[] = {
    {"up", 0, -1}, {"down", 0, 1}, {"left", -1, 0}, {"right", 1, 0}};

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

/* The states: alarm (latch) NORMAL > HEARD > UNACK > ACKED or
 * ENDED; info NORMAL > HEARD > SEEN. */
enum {
    STATE_NORMAL = 0,
    STATE_HEARD = 1,
    STATE_UNACK = 2,
    STATE_ACKED = 3,
    STATE_ENDED = 4,
    STATE_COUNT = 5, /* the alarm's states: the tables' size */
    STATE_SEEN = 5   /* the info state, never in a table */
};

/* Times, counts and sizes. */
enum {
    SETTLE_MS = 150, /* let the cleared screen settle */
    LOOP_MS = 20,
    OP_POLL_MS = 20,  /* ask a pending select or stop this often */
    SILENT_MS = 3000, /* result() failing this long = Stop */
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    DIR_COUNT = 4,
    YARD_CELLS = 5,
    START_CELL = 2,
    LOG_LINES = 3,
    TEXT_CHARS = 128, /* a SET_TEXT carries at most 126 bytes */
    CLOCK_CHARS = 36, /* "HH:MM:SS" of any three int32_t */
    PAUSE_BUTTON = 1, /* SW5, the lower button */
    SAFE_BUTTON = 0   /* SW6, the upper button: Safe stop */
};

/* Layout on the 792 x 398 canvas. */
enum {
    AUTO = -1, /* x, y, w or h the .py leaves out */
    LABEL_PX = 16,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 14,
    CARD_TITLE_DY = 8,
    YARD_X = 28,
    YARD_Y = 96,
    CELL_PX = 45,
    DOT_X = 32, /* the dot in cell (0, 0) */
    DOT_Y = 100,
    RANK_Y = 100,
    RANK_STEP_PX = 40
};

/* Every widget the program changes later has a slot. */
enum {
    RANKS = 3,
    SLOT_HEARTBEAT = 0,
    SLOT_DOT = 1,
    SLOT_WORD = 2,
    SLOT_MODE = 3,
    SLOT_LOG = 4,
    SLOT_SAFE = 5,
    SLOT_RANKS = 6, /* RANKS slots */
    SLOT_LEGEND = SLOT_RANKS + RANKS,
    SLOT_STATUS = SLOT_LEGEND + 1,
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

/* alarm (latch): NORMAL > HEARD > UNACK > ACKED or ENDED, needs ACK;
 * info: NORMAL > HEARD > SEEN for HOLD_MS, no ACK. */
typedef struct {
    bool latch;
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

/* The dot on the 5 x 5 yard; the fence stops it. */
typedef struct {
    int32_t x;
    int32_t y;
    uint32_t facing; /* one of DIRS */
    bool going;
} cart_t;

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
    vote_t votes[MAX_CLASSES];
    char log[LOG_LINES][TEXT_CHARS];
    uint32_t log_count;
    reader_t reader;
    char stop_text[TEXT_CHARS]; /* "" until the program stops itself */
} app_t;

/* The loop's state. */
typedef struct {
    bento_edgeai_thresholds_t thresholds;
    board_button_t pause_button;
    board_button_t safe_button;
    episode_t word;
    cart_t cart;
    uint32_t stop_class; /* the "stop" class, 0 = none */
    bool fresh_votes;    /* votes and the word start again */
    uint32_t held;       /* the word that has not ended yet, 0 = none */
    uint32_t heard;      /* the last word, as a class */
    bool has_top;        /* the newest result's scores, until drawn */
    int32_t top[MAX_CLASSES];
    uint32_t top_count;
    bool safe;
    bool paused;
    bool blink;
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

/* Where the episode goes once the event is no longer confirmed. */
static uint32_t settled_state(episode_t *episode, bool heard,
                              bool clear)
{
    uint32_t state =
        (episode->state == STATE_SEEN) ? STATE_NORMAL : episode->state;

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

/* True once per new event; refractory from the last event (info)
 * or the episode's last confirm. SEEN stays HOLD_MS. */
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
        episode->state = episode->latch ? STATE_UNACK : STATE_SEEN;
        episode->count++;
        episode->has_time = true;
        episode->time_ms = now_ms;
        is_new = true;
    } else {
        if (confirm && alarmed) {
            episode->time_ms = now_ms;
        }
        if ((episode->state != STATE_SEEN) ||
            (bento_time_ticks_diff(now_ms, episode->time_ms) >=
             HOLD_MS)) {
            episode->state = settled_state(episode, heard, clear);
        }
    }
    return is_new;
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

/* ---- 4) This model's screen (T6 keywords) ---- */

/* Moves the dot to its cell; a setter's refusal is ignored. */
static void place_dot(const app_t *app, const cart_t *cart)
{
    (void)bento_ui_set_pos(app->ui, app->ids[SLOT_DOT],
                           (int16_t)(DOT_X + (CELL_PX * cart->x)),
                           (int16_t)(DOT_Y + (CELL_PX * cart->y)));
}

/* One cell in the way it faces; the fence stops the cart. */
static void cart_step(const app_t *app, cart_t *cart)
{
    const int32_t x = cart->x + DIRS[cart->facing].dx;
    const int32_t y = cart->y + DIRS[cart->facing].dy;

    if ((x >= 0) && (x < YARD_CELLS) && (y >= 0) && (y < YARD_CELLS)) {
        cart->x = x;
        cart->y = y;
        place_dot(app, cart);
    } else {
        cart->going = false;
    }
}

/* A direction turns (and steps when halted); go and stop switch
 * driving. */
static void cart_hear(const app_t *app, cart_t *cart, const char *word)
{
    int32_t facing = -1;
    uint32_t i;

    for (i = 0U; i < (uint32_t)DIR_COUNT; i++) {
        if (strcmp(DIRS[i].word, word) == 0) {
            facing = (int32_t)i;
        }
    }
    if (facing >= 0) {
        cart->facing = (uint32_t)facing;
        if (!cart->going) {
            cart_step(app, cart);
        }
    } else {
        cart->going = strcmp(word, "go") == 0;
    }
}

/* The title, the heartbeat and the yard with its grid lines. */
static bento_status_t add_yard(app_t *app)
{
    static const widget_spec_t top[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Steer the Cart", 12, 4, AUTO,
         AUTO, COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LED, SLOT_HEARTBEAT, NULL, 756, 6, 26, 26, COLOR_OK,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Experimental. English only: left right up down stop go. "
         "Cannot: other words.",
         12, 38, AUTO, AUTO, COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 12, 64, 470, 272, COLOR_CARD,
         COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Feed yard", 12 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, YARD_X, YARD_Y, 225, 225,
         COLOR_CARD, COLOR_DIM, 0, 1},
    };
    static const widget_spec_t column = {
        UI_WIDGET_PANEL, SLOT_NONE, NULL, YARD_X - 1, YARD_Y, 2, 225,
        COLOR_BUTTON,    0,         0,    0};
    static const widget_spec_t row = {
        UI_WIDGET_PANEL, SLOT_NONE, NULL, YARD_X, YARD_Y - 1, 225, 2,
        COLOR_BUTTON,    0,         0,    0};
    widget_spec_t spec;
    int32_t line;
    bento_status_t status =
        add_widgets(app, top, sizeof(top) / sizeof(top[0]));

    for (line = 1; (line < YARD_CELLS) && (status == BENTO_OK);
         line++) {
        spec = column;
        spec.x = (int16_t)(spec.x + (CELL_PX * line));
        status = add_widget(app, &spec);
        if (status == BENTO_OK) {
            spec = row;
            spec.y = (int16_t)(spec.y + (CELL_PX * line));
            status = add_widget(app, &spec);
        }
    }
    return status;
}

/* The three rows of the "Top 3 scores" card. */
static bento_status_t add_ranks(app_t *app)
{
    static const widget_spec_t rank = {
        UI_WIDGET_LABEL, SLOT_RANKS, " ", 508, RANK_Y, AUTO, AUTO,
        COLOR_DIM,       0,          0,   20};
    widget_spec_t spec = rank;
    int32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0; (i < RANKS) && (status == BENTO_OK); i++) {
        spec.slot = (uint8_t)(SLOT_RANKS + i);
        spec.y = (int16_t)(RANK_Y + (RANK_STEP_PX * i));
        status = add_widget(app, &spec);
    }
    return status;
}

static bento_status_t build_screen(app_t *app)
{
    static const widget_spec_t rest[] = {
        /* cell (2, 2) */
        {UI_WIDGET_PANEL, SLOT_DOT, NULL, 122, 190, 37, 37, COLOR_INFO,
         0, 18, 0},
        {UI_WIDGET_LABEL, SLOT_WORD, "-", 266, 100, AUTO, AUTO,
         COLOR_DIM, 0, 0, 28},
        {UI_WIDGET_LABEL, SLOT_MODE, " ", 266, 140, AUTO, AUTO,
         COLOR_TEXT, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_LOG, " ", 266, 170, AUTO, AUTO,
         COLOR_TEXT, 0, 0, LABEL_PX},
        {UI_WIDGET_BUTTON, SLOT_SAFE, "Safe stop: ON", 266, 262, 200,
         64, COLOR_BUTTON, 0, 0, 20},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 494, 64, 294, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Top 3 scores",
         494 + CARD_TITLE_DX, 64 + CARD_TITLE_DY, AUTO, AUTO,
         COLOR_INFO, 0, 0, LABEL_PX},
    };
    static const widget_spec_t bottom[] = {
        {UI_WIDGET_LABEL, SLOT_LEGEND, " ", 508, 230, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 12, 346, AUTO, AUTO,
         COLOR_WARN, 0, 0, LABEL_PX},
    };
    bento_status_t status = clear_screen(app);

    if (status == BENTO_OK) {
        status = add_yard(app);
    }
    if (status == BENTO_OK) {
        status = add_widgets(app, rest, sizeof(rest) / sizeof(rest[0]));
    }
    if (status == BENTO_OK) {
        status = add_ranks(app);
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

/* An ASCII letter a-z in capitals; any other byte as it is. */
static char upper_of(char letter)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char result = letter;
    size_t i;

    for (i = 0U; lower[i] != '\0'; i++) {
        if (letter == lower[i]) {
            result = upper[i];
        }
    }
    return result;
}

/* The label in capitals (ASCII). */
static void upper_text(const char *label, char *text, size_t cap)
{
    size_t i;

    for (i = 0U; (label[i] != '\0') && (i < (cap - 1U)); i++) {
        text[i] = upper_of(label[i]);
    }
    text[i] = '\0';
}

/* The class at a rank: the highest score not ranked yet, the lower
 * class on a tie (a stable sort). */
static uint32_t ranked_class(const run_t *run, const bool *taken)
{
    uint32_t best = run->top_count;
    uint32_t klass;

    for (klass = 0U; klass < run->top_count; klass++) {
        if (!taken[klass] && ((best == run->top_count) ||
                              (run->top[klass] > run->top[best]))) {
            best = klass;
        }
    }
    return best;
}

/* The top 3 scores; white when an event class is at Confirm at. */
static void draw_ranks(app_t *app, const run_t *run)
{
    bool taken[MAX_CLASSES];
    char text[TEXT_CHARS];
    uint32_t rank;
    uint32_t klass;

    (void)memset(taken, 0, sizeof(taken));
    for (rank = 0U; (rank < (uint32_t)RANKS) && (rank < run->top_count);
         rank++) {
        klass = ranked_class(run, taken);
        taken[klass] = true;
        (void)snprintf(text, sizeof(text), "%s %" PRId32 "%%",
                       app->labels[klass], run->top[klass]);
        put(app, (uint32_t)SLOT_RANKS + rank, text,
            ((klass != 0U) && (run->top[klass] >=
                               (int32_t)run->thresholds.confirm[klass]))
                ? COLOR_TEXT
                : COLOR_DIM);
    }
}

/* Heard: the word in capitals while it shows, else "-". */
static void draw(app_t *app, const run_t *run)
{
    char text[TEXT_CHARS];

    set_value(app, SLOT_HEARTBEAT,
              (run->blink && run->has_top) ? 1 : 0);
    if (run->word.state == STATE_SEEN) { /* SEEN comes with a word */
        upper_text(app->labels[run->heard], text, sizeof(text));
    } else {
        (void)snprintf(text, sizeof(text), "-");
    }
    put(app, SLOT_WORD, text, COLOR_TEXT);
    (void)snprintf(text, sizeof(text),
                   run->cart.going ? "Going %s, 1 cell/s"
                                   : "Stopped, facing %s",
                   DIRS[run->cart.facing].word);
    put_text(app, SLOT_MODE, text);
    if (run->has_top) {
        draw_ranks(app, run);
    }
}

/* ---- 5) Main program ---- */

static const char *help_text(const app_t *app)
{
    return app->has_buttons
               ? "Say a word - SW6 = Safe stop, SW5 = pause"
               : "Say a word";
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

/* A new word: the cart hears it, then the log. */
static void take_word(app_t *app, run_t *run, uint32_t klass)
{
    char stamp[CLOCK_CHARS];
    char text[TEXT_CHARS];
    uint32_t i;

    run->held = klass;
    run->heard = klass;
    cart_hear(app, &run->cart, app->labels[klass]);
    if (run->cart.going) {
        /* Safe stop hears only stop scores newer than this go */
        app->votes[run->stop_class].count = 0U;
    }
    for (i = (uint32_t)LOG_LINES - 1U; i > 0U; i--) {
        (void)memcpy(app->log[i], app->log[i - 1U],
                     sizeof(app->log[i]));
    }
    clock_text(stamp, sizeof(stamp));
    (void)snprintf(app->log[0], sizeof(app->log[0]),
                   "%s %s %" PRId32 "%% #%" PRId32, stamp,
                   app->labels[klass], vote_max(&app->votes[klass]),
                   run->word.count);
    if (app->log_count < (uint32_t)LOG_LINES) {
        app->log_count++;
    }
    (void)printf("%s\n", app->log[0]);
    (void)snprintf(text, sizeof(text), "%s", app->log[0]);
    for (i = 1U; i < app->log_count; i++) {
        const size_t used = strlen(text);

        (void)snprintf(&text[used], sizeof(text) - used, "\n%s",
                       app->log[i]);
    }
    put_text(app, SLOT_LOG, text);
}

/* Every class ends: all events below their exit level. */
static bool all_clear(const app_t *app, int32_t level)
{
    bool clear = true;
    uint32_t klass;

    for (klass = 1U; klass < app->label_count; klass++) {
        if (!vote_clear(&app->votes[klass], level)) {
            clear = false;
        }
    }
    return clear;
}

/* Safe stop: "stop" at Hearing at stops a driving cart at once. */
static bool safe_stop(const app_t *app, const run_t *run)
{
    const uint32_t stop = run->stop_class;

    return run->safe && run->cart.going && (stop != 0U) &&
           (vote_max(&app->votes[stop]) >=
            (int32_t)run->thresholds.enter[stop]);
}

/* A word = K of its last N at Confirm at, once per utterance. */
static void take_result(app_t *app, run_t *run,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    const bento_edgeai_thresholds_t *thresholds = &run->thresholds;
    event_t event;
    bool confirmed;
    uint32_t klass;

    run->has_top = true;
    run->top_count = (result->class_count < (uint8_t)MAX_CLASSES)
                         ? result->class_count
                         : (uint32_t)MAX_CLASSES;
    for (klass = 0U; klass < run->top_count; klass++) {
        run->top[klass] = result->score_pct[klass];
        if (klass < app->label_count) {
            vote_add(&app->votes[klass], run->top[klass]);
        }
    }
    event_of(result, thresholds, &event);
    if ((run->held != 0U) && !vote_hits(&app->votes[run->held],
                                        thresholds->exit[run->held])) {
        run->held = 0U; /* that word ended: it may count again */
    }
    confirmed = (event.klass != run->held) &&
                vote_hits(&app->votes[event.klass], event.confirm);
    if (safe_stop(app, run)) {
        event.klass = run->stop_class;
        confirmed = true;
        run->word.has_time = false; /* no refractory wait */
    }
    if (episode_step(&run->word, event.percent >= event.hearing,
                     confirmed, all_clear(app, event.clear), now_ms)) {
        take_word(app, run, event.klass);
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

/* A tap on Safe stop (a click). A failed poll answers no events. */
static bool safe_tapped(const app_t *app)
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
            (events[i].id == app->ids[SLOT_SAFE])) {
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
    run->cart.going = false;
    run->fresh_votes = true;
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

/* SW6 or a tap: Safe stop on or off; SW5: pause. */
static bento_status_t read_controls(app_t *app, run_t *run)
{
    bool toggled;
    bento_status_t status = sample_button(app, &run->pause_button);

    if (status == BENTO_OK) {
        status = sample_button(app, &run->safe_button);
    }
    if (status == BENTO_OK) {
        toggled = pressed_now(&run->safe_button);
        if (safe_tapped(app)) {
            toggled = true;
        }
        if (toggled) {
            run->safe = !run->safe;
            put_text(app, SLOT_SAFE,
                     run->safe ? "Safe stop: ON" : "Safe stop: OFF");
        }
        if (pressed_now(&run->pause_button)) {
            status = toggle_pause(app, run);
        }
    }
    return status;
}

/* Fresh votes: the last N scores of each class; the word ends. */
static void renew_votes(app_t *app, run_t *run)
{
    uint32_t klass;

    for (klass = 0U; klass < app->label_count; klass++) {
        app->votes[klass].count = 0U;
    }
    run->held = 0U;
    run->word.state = STATE_NORMAL;
    run->fresh_votes = false;
}

/* Every DRAW_MS: every other draw the cart drives one cell (1
 * cell/s), then the screen. */
static void redraw(app_t *app, run_t *run, uint32_t now_ms)
{
    run->drawn_ms = now_ms;
    run->blink = !run->blink;
    if (run->cart.going && run->blink) {
        cart_step(app, &run->cart);
    }
    draw(app, run);
    run->has_top = false;
}

/* One pass of the loop: controls, results, drawing. */
static bento_status_t run_once(app_t *app, run_t *run)
{
    uint32_t now_ms;
    bento_status_t status = read_controls(app, run);

    if (run->fresh_votes) {
        renew_votes(app, run);
    }
    now_ms = bento_time_ticks_ms();
    if ((status == BENTO_OK) && !run->paused &&
        (bento_time_ticks_diff(now_ms, run->polled_ms) >= POLL_MS)) {
        run->polled_ms = now_ms;
        status = poll_model(app, run, now_ms);
    }
    if ((status == BENTO_OK) &&
        (bento_time_ticks_diff(now_ms, run->drawn_ms) >= DRAW_MS)) {
        redraw(app, run, now_ms);
    }
    return status;
}

/* The "stop" class (0 = none) and the legend. */
static void prepare_words(app_t *app, run_t *run)
{
    const bento_edgeai_thresholds_t *thresholds = &run->thresholds;
    char text[TEXT_CHARS];
    uint32_t klass;
    bool found = false;

    run->stop_class = 0U;
    for (klass = 0U; klass < app->label_count; klass++) {
        if (!found && (strcmp(app->labels[klass], "stop") == 0)) {
            run->stop_class = klass;
            found = true;
        }
    }
    (void)snprintf(
        text, sizeof(text),
        "Hearing at %d%% | Confirm at %d%%\nSafe stop: stop at "
        "%d%%, 1 of %d",
        (int)thresholds->enter[1], (int)thresholds->confirm[1],
        (int)thresholds->enter[run->stop_class], VOTE_WINDOW);
    put_text(app, SLOT_LEGEND, text);
}

static bento_status_t run_model(app_t *app)
{
    run_t run;
    bento_status_t status;

    (void)memset(&run, 0, sizeof(run));
    read_thresholds(app->model_index, &run.thresholds);
    prepare_words(app, &run);
    run.pause_button.index = PAUSE_BUTTON;
    run.safe_button.index = SAFE_BUTTON;
    run.word.latch = false;
    run.cart.x = START_CELL;
    run.cart.y = START_CELL;
    run.cart.facing = 0U; /* "up" */
    run.safe = true;
    run.fresh_votes = true;
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
    } else {
        /* a board with the model link; a board without it
         * (reTerminal, supported false), whose model list cannot be
         * read either, so the same card follows with no Emulator
         * text; or a link error that ends it */
    }
    if (status == BENTO_OK) {
        found = find_model(app);
        status = found ? watch_model(app) : show_missing(app);
    }
    if ((status == BENTO_OK) && emulator && !found) {
        status = show_emulator_gap(app);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
