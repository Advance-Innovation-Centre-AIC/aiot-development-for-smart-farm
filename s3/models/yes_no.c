/*
 * yes_no.c -- Voice Confirm: ยืนยันด้วยเสียง yes / no
 * (ต้อง Deploy "Yes / No" (ทดลอง) จาก Edge AI Store)
 *
 * แตะ Ask (หรือกด SW6 ปุ่มบน) แล้วพูด yes หรือ no ภายใน 5 วินาที
 * yes = เปิดปั๊ม (จำลองบนจอ) no = ปิด ไม่ตอบในเวลา = ปั๊มเหมือนเดิม
 * คำที่ได้ยินโดยไม่มีใครถาม จะนับเป็น ignored และไม่เปลี่ยนอะไร
 * การ์ด "Scores" แสดงคะแนนแต่ละคลาส แถบเป็นสีขาวเมื่อถึงเกณฑ์ Confirm
 * SW5 (ปุ่มล่าง) หยุดชั่วคราว/ทำต่อ โมเดลรู้จักแค่ yes และ no ภาษาอังกฤษ
 *
 * Tap Ask (or press SW6, upper), then say yes or no within 5 s: yes
 * turns the (simulated) pump on, no turns it off, no answer leaves it
 * as it was. A word heard when nobody asked is counted as ignored
 * and changes nothing. The "Scores" card shows each class's score;
 * a bar turns white at its Confirm level. SW5 (lower) =
 * pause/resume. The model knows only English yes and no.
 *
 * Twin of yes_no.py (aiot-development-for-smart-farm/s3/models/yes_no.py). Style: CLEAN_C.md.
 * Differs from the .py:
 *   - edge_ai.models() is read row by row (count, model_info, label);
 *     a row is from the Store when it comes after the rows the
 *     firmware compiled in (diag.compiled_models); a board that
 *     cannot say (the .py's None) has no Store model here.
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
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available
 *     yet: the program says so and ends, as the .py does without the
 *     model. It shows the .py's own "Deploy ... on a Dev Kit" card,
 *     with the Emulator line under it.
 *   - gc.collect() and gc.mem_free(): C has no garbage collector, so
 *     the "free heap:" line is not printed.
 *   - The dict of texts, colours and values already sent is one entry
 *     per widget slot; a text is kept up to TEXT_CHARS - 1 bytes.
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
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- 1) Settings: edit before running ---- */
static const char MODEL_NAME[] = "Yes / No"; /* exact name */
/* BUILTIN (false = deployed from the Store) is in source_matches(). */

enum {
    VOTE_NEED = 2,        /* a word = 2 of ... */
    VOTE_WINDOW = 4,      /* ... the last 4 results at Confirm at */
    REFRACT_MS = 1000,    /* no second word sooner than this */
    HOLD_MS = 2000,       /* a word stays on screen this long */
    DEFAULT_HEARING = 40, /* Hearing at, when the board cannot say */
    DEFAULT_CONFIRM = 65, /* Confirm at */
    DEFAULT_CLEAR = 30,   /* clear below */
    POLL_MS = 100,
    DRAW_MS = 500,
    ASK_MS = 5000 /* the answer window after Ask */
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
    COLOR_INFO = 0x4A9EFF
};

/* The states of a word: NORMAL > HEARD > SEEN for HOLD_MS. The
 * latch states (UNACK, ACKED, ENDED) are the shared episode's. */
enum {
    STATE_NORMAL = 0,
    STATE_HEARD = 1,
    STATE_UNACK = 2,
    STATE_ACKED = 3,
    STATE_ENDED = 4,
    STATE_SEEN = 5
};

enum {
    SETTLE_MS = 150, /* let the cleared screen settle */
    LOOP_MS = 20,
    OP_POLL_MS = 20,  /* ask a pending select or stop this often */
    SILENT_MS = 3000, /* result() failing this long = Stop */
    MS_PER_S = 1000,
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    ROWS = 3,         /* score rows on the card */
    LOG_LINES = 3,    /* the log label's lines */
    TEXT_CHARS = 128, /* a SET_TEXT carries at most 126 bytes */
    CLOCK_CHARS = 36, /* "HH:MM:SS" of any three int32_t */
    PAUSE_BUTTON = 1, /* SW5, the lower button */
    ASK_BUTTON = 0    /* SW6, the upper button: Ask */
};

/* Layout on the 792 x 398 canvas. */
enum {
    AUTO = -1, /* x, y, w or h the .py leaves out */
    LABEL_PX = 16,
    CARD_RADIUS = 12
};

enum {
    SLOT_HEARTBEAT = 0,
    SLOT_QUESTION = 1,
    SLOT_WORD = 2,
    SLOT_PUMP = 3,
    SLOT_COUNTDOWN = 4,
    SLOT_IGNORED = 5,
    SLOT_ASK = 6,
    SLOT_ROW_LABEL = 7, /* ROWS (3) slots: 7, 8, 9 */
    SLOT_ROW_BAR = 10,  /* ROWS (3) slots: 10, 11, 12 */
    SLOT_LEGEND = 13,
    SLOT_LOG = SLOT_LEGEND + 1,
    SLOT_STATUS = SLOT_LOG + 1,
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

/* The last VOTE_WINDOW scores of one class. */
typedef struct {
    int32_t scores[VOTE_WINDOW];
    uint32_t count;
} vote_t;

/* info: NORMAL > HEARD > SEEN for HOLD_MS, no ACK. */
typedef struct {
    uint32_t state;
    int32_t count;
    bool has_time;
    uint32_t time_ms;
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
    board_button_t ask_button;
    episode_t episode;
    bool has_votes; /* false: fresh votes before the next result */
    vote_t votes[MAX_CLASSES];
    uint32_t held; /* the word that last counted, 0 = none */
    char word[BENTO_EDGEAI_LABEL_LEN];
    bool has_top; /* a new result since the last draw */
    int32_t top[MAX_CLASSES];
    uint32_t top_count;
    bool asking;
    uint32_t ask_ms;
    bool pump_on;
    char note[TEXT_CHARS];
    int32_t answered;
    int32_t ignored;
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
    const bool builtin = false; /* BUILTIN: false = from the Store */
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
        app->label_count = (uint32_t)MAX_CLASSES;
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
            thresholds->enter[i] = (uint8_t)DEFAULT_HEARING;
            thresholds->confirm[i] = (uint8_t)DEFAULT_CONFIRM;
            thresholds->exit[i] = (uint8_t)DEFAULT_CLEAR;
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
                 ((uint32_t)result->index == app->model_index);
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

/* Where the episode goes once the word is no longer confirmed. A
 * word's episode never latches, so UNACK and ENDED never come. */
static uint32_t settled_state(const episode_t *episode, bool heard,
                              bool clear)
{
    uint32_t state = (episode->state == (uint32_t)STATE_SEEN)
                         ? (uint32_t)STATE_NORMAL
                         : episode->state;

    if (clear) {
        if (state != (uint32_t)STATE_ENDED) {
            state = (uint32_t)STATE_NORMAL;
        }
    } else if (heard && (state == (uint32_t)STATE_NORMAL)) {
        state = (uint32_t)STATE_HEARD;
    } else {
        /* no change */
    }
    return state;
}

/* True once per new word; refractory from the last word. SEEN stays
 * HOLD_MS. */
static bool episode_step(episode_t *episode, bool heard, bool confirm,
                         bool clear, uint32_t now_ms)
{
    const bool alarmed = (episode->state == (uint32_t)STATE_UNACK) ||
                         (episode->state == (uint32_t)STATE_ACKED);
    const bool rested =
        !episode->has_time ||
        (bento_time_ticks_diff(now_ms, episode->time_ms) >= REFRACT_MS);
    bool is_new = false;

    if (confirm && !alarmed && rested) {
        episode->state = (uint32_t)STATE_SEEN;
        episode->count++;
        episode->has_time = true;
        episode->time_ms = now_ms;
        is_new = true;
    } else {
        if (confirm && alarmed) {
            episode->time_ms = now_ms;
        }
        if ((episode->state != (uint32_t)STATE_SEEN) ||
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
    const uint32_t count = (result->class_count < (uint32_t)MAX_CLASSES)
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
    event->percent = (int32_t)result->score_pct[strongest];
    event->hearing = (int32_t)thresholds->enter[strongest];
    event->confirm = (int32_t)thresholds->confirm[strongest];
    event->clear = (int32_t)thresholds->exit[strongest];
}

/* ---- 3) Screen helpers ---- */

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

/* Sends a Bar's value only when it changed. */
static void put_value(app_t *app, uint32_t slot, int32_t value)
{
    sent_t *sent = &app->sent[slot];

    if (!sent->has_value || (sent->value != value)) {
        sent->has_value = true;
        sent->value = value;
        (void)bento_ui_set_value(app->ui, app->ids[slot], value);
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
                   "Deploy %s\nfrom edgeai-store.tesaiot.dev\n"
                   "(sign in with GitHub) on a Dev Kit.",
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

/* The title, the heartbeat and the "Answer" card. */
static bento_status_t add_answer_card(app_t *app)
{
    static const widget_spec_t card[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Voice Confirm", 12, 4, AUTO, AUTO,
         COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LED, SLOT_HEARTBEAT, NULL, 756, 6, 26, 26, COLOR_OK,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Experimental. Detects English 'yes' and 'no'. Cannot: other "
         "words or Thai.",
         12, 38, AUTO, AUTO, COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 12, 64, 470, 272, COLOR_CARD,
         COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Answer", 26, 72, AUTO, AUTO,
         COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_QUESTION, "Start the pump?", 28, 96,
         AUTO, AUTO, COLOR_DIM, 0, 0, 24},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 28, 136, 200, 110,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_WORD, "-", 48, 176, AUTO, AUTO,
         COLOR_DIM, 0, 0, 28},
        {UI_WIDGET_LABEL, SLOT_PUMP, "Pump: OFF", 244, 140, AUTO, AUTO,
         COLOR_DIM, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_COUNTDOWN, " ", 244, 172, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_IGNORED, "ignored 0", 244, 200, AUTO,
         AUTO, COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_BUTTON, SLOT_ASK, "Ask", 28, 262, 200, 64,
         COLOR_BUTTON, 0, 0, 24},
    };

    return add_widgets(app, card, sizeof(card) / sizeof(card[0]));
}

/* The "Scores" card: three label rows, then three bars, the legend,
 * the log and the status line. */
static bento_status_t add_scores_card(app_t *app)
{
    static const widget_spec_t card[] = {
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 494, 64, 294, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Scores", 508, 72, AUTO, AUTO,
         COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_ROW_LABEL, " ", 508, 96, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_ROW_LABEL + 1, " ", 508, 140, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_ROW_LABEL + 2, " ", 508, 184, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_BAR, SLOT_ROW_BAR, NULL, 508, 118, 266, 12, 0U, 0,
         100, 0},
        {UI_WIDGET_BAR, SLOT_ROW_BAR + 1, NULL, 508, 162, 266, 12, 0U,
         0, 100, 0},
        {UI_WIDGET_BAR, SLOT_ROW_BAR + 2, NULL, 508, 206, 266, 12, 0U,
         0, 100, 0},
        {UI_WIDGET_LABEL, SLOT_LEGEND, " ", 508, 230, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_LOG, " ", 508, 256, AUTO, AUTO,
         COLOR_TEXT, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 12, 346, AUTO, AUTO,
         COLOR_WARN, 0, 0, LABEL_PX},
    };

    return add_widgets(app, card, sizeof(card) / sizeof(card[0]));
}

static bento_status_t build_screen(app_t *app)
{
    bento_status_t status = clear_screen(app);

    if (status == BENTO_OK) {
        status = add_answer_card(app);
    }
    if (status == BENTO_OK) {
        status = add_scores_card(app);
    }
    if (status == BENTO_OK) {
        drain_events(app->ui);
    }
    return status;
}

static const char *label_of(const app_t *app, uint32_t klass)
{
    return (klass < app->label_count) ? app->labels[klass] : "";
}

/* One row per class; white = at Confirm at. */
static void draw_bars(app_t *app, const run_t *run)
{
    char text[TEXT_CHARS];
    uint32_t row;

    for (row = 0U; (row < (uint32_t)ROWS) && (row < run->top_count);
         row++) {
        const int32_t percent = run->top[row];
        const bool confirmed =
            (row != 0U) &&
            (percent >= (int32_t)run->thresholds.confirm[row]);

        (void)snprintf(text, sizeof(text), "%s %" PRId32 "%%",
                       label_of(app, row), percent);
        put_text(app, (uint32_t)SLOT_ROW_LABEL + row, text);
        put_color(app, (uint32_t)SLOT_ROW_BAR + row,
                  confirmed ? COLOR_TEXT : COLOR_DIM);
        put_value(app, (uint32_t)SLOT_ROW_BAR + row, percent);
    }
}

/* str.upper() of one ASCII letter; any other byte as it is. */
static char upper_letter(char letter)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char result = letter;
    uint32_t i;

    for (i = 0U; lower[i] != '\0'; i++) {
        if (lower[i] == letter) {
            result = upper[i];
        }
    }
    return result;
}

/* The big word: upper case when seen, "maybe ..." when heard. */
static void draw_word(app_t *app, const run_t *run)
{
    char text[TEXT_CHARS];
    uint32_t i;

    if (run->episode.state == (uint32_t)STATE_SEEN) {
        for (i = 0U;
             (run->word[i] != '\0') && ((i + 1U) < sizeof(text)); i++) {
            text[i] = upper_letter(run->word[i]);
        }
        text[i] = '\0';
        put(app, SLOT_WORD, text, COLOR_TEXT);
    } else if (run->episode.state == (uint32_t)STATE_HEARD) {
        (void)snprintf(text, sizeof(text), "maybe %s", run->word);
        put(app, SLOT_WORD, text, COLOR_DIM);
    } else {
        put(app, SLOT_WORD, "-", COLOR_DIM);
    }
}

/* Every DRAW_MS: the question, the countdown, the pump, the
 * heartbeat, the word and the bars. */
static void draw(app_t *app, run_t *run, uint32_t now_ms)
{
    char text[TEXT_CHARS];
    int32_t left_s = 0;

    if (run->asking) {
        left_s =
            ((ASK_MS - bento_time_ticks_diff(now_ms, run->ask_ms)) +
             (MS_PER_S - 1)) /
            MS_PER_S;
    }
    put_color(app, SLOT_QUESTION,
              (left_s != 0) ? COLOR_TEXT : COLOR_DIM);
    if (left_s != 0) {
        (void)snprintf(text, sizeof(text), "answer in %" PRId32 " s",
                       left_s);
        put(app, SLOT_COUNTDOWN, text, COLOR_TEXT);
    } else {
        put(app, SLOT_COUNTDOWN, run->note, COLOR_DIM);
    }
    put(app, SLOT_PUMP, run->pump_on ? "Pump: ON" : "Pump: OFF",
        run->pump_on ? COLOR_TEXT : COLOR_DIM);
    (void)snprintf(text, sizeof(text), "ignored %" PRId32,
                   run->ignored);
    put_text(app, SLOT_IGNORED, text);
    (void)bento_ui_set_value(app->ui, app->ids[SLOT_HEARTBEAT],
                             (run->blink && run->has_top) ? 1 : 0);
    draw_word(app, run);
    if (run->has_top) {
        draw_bars(app, run);
    }
    run->has_top = false;
}

/* ---- 5) Main program ---- */

static const char *help_text(const app_t *app)
{
    return app->has_buttons
               ? "Tap Ask or SW6, then say yes or no - SW5 = pause"
               : "Tap Ask, then say yes or no";
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

/* The log label: the three newest lines, newest first. */
static void log_word(app_t *app, const char *line)
{
    char text[TEXT_CHARS];
    size_t used = 0U;
    uint32_t i;

    for (i = (uint32_t)LOG_LINES - 1U; i > 0U; i--) {
        (void)memcpy(app->log[i], app->log[i - 1U],
                     sizeof(app->log[i]));
    }
    (void)snprintf(app->log[0], sizeof(app->log[0]), "%s", line);
    if (app->log_count < (uint32_t)LOG_LINES) {
        app->log_count++;
    }
    (void)printf("%s\n", line);
    text[0] = '\0';
    for (i = 0U; (i < app->log_count) && (used < sizeof(text)); i++) {
        (void)snprintf(&text[used], sizeof(text) - used, "%s%s",
                       (i == 0U) ? "" : "\n", app->log[i]);
        used = strlen(text);
    }
    put_text(app, SLOT_LOG, text);
}

/* A new word: it answers the question, or nobody asked and it
 * changes nothing. */
static void count_word(app_t *app, run_t *run, uint32_t klass)
{
    char stamp[CLOCK_CHARS];
    char line[TEXT_CHARS];
    char tail[TEXT_CHARS];

    run->held = klass;
    clock_text(stamp, sizeof(stamp));
    if (!run->asking) {
        run->ignored++;
        (void)snprintf(tail, sizeof(tail), "ignored %" PRId32,
                       run->ignored);
    } else {
        run->answered++;
        run->asking = false;
        run->pump_on = strcmp(run->word, "yes") == 0;
        (void)snprintf(run->note, sizeof(run->note), "you said %s",
                       run->word);
        (void)snprintf(tail, sizeof(tail), "#%" PRId32, run->answered);
    }
    (void)snprintf(line, sizeof(line), "%s %s %" PRId32 "%% %s", stamp,
                   run->word, vote_max(&run->votes[klass]), tail);
    log_word(app, line);
}

/* Every vote below its exit level, class 0 left out. */
static bool all_clear(const app_t *app, const run_t *run, int32_t level)
{
    bool clear = true;
    uint32_t i;

    for (i = 1U; i < app->label_count; i++) {
        clear = clear && vote_clear(&run->votes[i], level);
    }
    return clear;
}

/* A word = K of its last N at Confirm at, once per utterance. */
static void take_result(app_t *app, run_t *run,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    event_t event;
    uint32_t i;
    bool is_new;

    run->top_count = (result->class_count < (uint32_t)MAX_CLASSES)
                         ? result->class_count
                         : (uint32_t)MAX_CLASSES;
    for (i = 0U; i < run->top_count; i++) {
        run->top[i] = (int32_t)result->score_pct[i];
        if (i < app->label_count) {
            vote_add(&run->votes[i], run->top[i]);
        }
    }
    run->has_top = true;
    event_of(result, &run->thresholds, &event);
    if ((run->held != 0U) &&
        !vote_hits(&run->votes[run->held],
                   (int32_t)run->thresholds.exit[run->held])) {
        run->held = 0U; /* that word ended: it may count again */
    }
    is_new = episode_step(
        &run->episode, event.percent >= event.hearing,
        (event.klass != run->held) &&
            vote_hits(&run->votes[event.klass], event.confirm),
        all_clear(app, run, event.clear), now_ms);
    if (is_new || (run->episode.state != (uint32_t)STATE_SEEN)) {
        (void)snprintf(run->word, sizeof(run->word), "%s",
                       label_of(app, event.klass));
    }
    if (is_new) {
        count_word(app, run, event.klass);
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

/* A tap on Ask. A failed poll answers no events. */
static bool ask_tapped(const app_t *app)
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
            (events[i].id == app->ids[SLOT_ASK])) {
            tapped = true;
        }
    }
    return tapped;
}

/* SW5: pause stops the AI core; resume starts it again. Either way
 * the question closes and the votes start fresh. */
static bento_status_t toggle_pause(app_t *app, run_t *run)
{
    bento_status_t status;

    run->paused = !run->paused;
    run->asking = false;
    run->has_votes = false;
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

/* SW6 or Ask opens the answer window; ASK_MS closes it. */
static void follow_question(const app_t *app, run_t *run,
                            uint32_t now_ms)
{
    bool go = pressed_now(&run->ask_button);

    go = ask_tapped(app) || go;
    if (go) {
        run->asking = true;
        run->ask_ms = now_ms;
    }
    if (run->asking &&
        (bento_time_ticks_diff(now_ms, run->ask_ms) >= ASK_MS)) {
        run->asking = false;
        (void)snprintf(run->note, sizeof(run->note),
                       "no answer - pump unchanged");
    }
}

/* One pass of the loop: buttons, taps, pause, results, drawing. */
static bento_status_t run_once(app_t *app, run_t *run)
{
    uint32_t now_ms = bento_time_ticks_ms();
    bento_status_t status = sample_button(app, &run->pause_button);

    if (status == BENTO_OK) {
        status = sample_button(app, &run->ask_button);
    }
    if (status == BENTO_OK) {
        now_ms = bento_time_ticks_ms();
        follow_question(app, run, now_ms);
        if (pressed_now(&run->pause_button)) {
            status = toggle_pause(app, run);
        }
    }
    if ((status == BENTO_OK) && !run->has_votes) {
        (void)memset(run->votes, 0, sizeof(run->votes));
        run->has_votes = true;
        run->held = 0U;
        run->episode.state = (uint32_t)STATE_NORMAL;
    }
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
    }
    return status;
}

static bento_status_t run_model(app_t *app, run_t *run)
{
    char text[TEXT_CHARS];
    bento_status_t status;

    read_thresholds(app->model_index, &run->thresholds);
    (void)snprintf(
        text, sizeof(text), "Hearing at %d%% | Confirm at %d%%",
        (int)run->thresholds.enter[1], (int)run->thresholds.confirm[1]);
    put_text(app, SLOT_LEGEND, text);
    run->pause_button.index = (uint32_t)PAUSE_BUTTON;
    run->ask_button.index = (uint32_t)ASK_BUTTON;
    (void)snprintf(run->note, sizeof(run->note), " ");
    status = start_model(app, false);
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

/* The screen, the loop, and the AI core always released after it.
 * A Stop the program raised itself is shown, then it ends. */
static bento_status_t watch_model(app_t *app, run_t *run)
{
    bento_status_t status = build_screen(app);

    if (status == BENTO_OK) {
        bento_status_t loop_status = run_model(app, run);
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

/* No Edge AI from C (BENTO_E_UNSUPPORTED, as in the BENTO Emulator):
 * the .py's own "no model" card, the Emulator line under it, then a
 * normal end, as the .py ends there. */
static bento_status_t show_emulator_gap(app_t *app)
{
    static const char gap[] = "Edge AI จาก C ยังไม่มีใน BENTO Emulator "
                              "ให้รันบน Dev Kit หรือ Eva Kit";
    static const widget_spec_t line = {
        UI_WIDGET_LABEL, SLOT_NONE, gap, 96, 300, AUTO, AUTO,
        COLOR_WARN,      0,         0,   20};
    bento_status_t status = show_missing(app);

    if (status == BENTO_OK) {
        status = add_widget(app, &line);
    }
    if (status == BENTO_OK) {
        drain_events(app->ui);
        (void)printf("%s / Edge AI from C is not in the BENTO Emulator "
                     "yet: run it on a Dev Kit or Eva Kit.\n",
                     gap);
    }
    return status;
}

int main(void)
{
    app_t *app = &g_app;
    run_t run;
    bool supported = false;
    bento_status_t status;

    (void)memset(app, 0, sizeof(*app));
    (void)memset(&run, 0, sizeof(run));
    app->ui = bento_runtime_get_ui();
    app->has_buttons = bento_buttons_count() > 0U;
    status = bento_edgeai_supported(&supported);
    if (status == BENTO_E_UNSUPPORTED) {
        status = show_emulator_gap(app);
    } else if ((status == BENTO_OK) && !supported) {
        status = BENTO_E_UNSUPPORTED; /* import edge_ai fails */
    } else if (status == BENTO_OK) {
        status = find_model(app) ? watch_model(app, &run)
                                 : show_missing(app);
    } else {
        /* another failure of the query ends the run with its status */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
