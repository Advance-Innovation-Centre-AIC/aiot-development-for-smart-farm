/*
 * home_sounds.c -- Three-Sound Board: กระดานเสียงในบ้าน 3 ชนิด
 * (ต้องใช้ HomeSounds จาก Edge AI Store)
 *
 * ใช้โมเดล "HomeSounds" จาก Edge AI Store (ต้อง deploy ลงบอร์ดก่อน)
 * ฟังเสียงเด็กร้อง เสียงไอ และเสียงน้ำไหลจากก๊อก แต่ละชนิดมีช่องของ
 * ตัวเอง บอกจำนวนครั้งและเวลาครั้งล่าสุด แตะช่องเพื่อปิด/เปิดเสียง
 * เตือนของชนิดนั้น น้ำไหลต่อเนื่อง 30 วินาทีเป็นสัญญาณเตือนสีแดงค้าง
 * ไว้จนกว่าจะรับทราบ: แตะ ACK หรือกด SW6 (ปุ่มบน) ส่วน SW5 (ปุ่มล่าง)
 * หยุดชั่วคราว/ทำต่อ
 *
 * Runs "HomeSounds" from the Edge AI Store (deploy it first): a baby
 * crying, a cough and a running tap, each on its own tile with its
 * count and the time of the last one. Tap a tile to mute or unmute
 * that sound. A tap running for 30 s is a red alarm that stays
 * until it is acknowledged: tap ACK or press SW6 (upper). SW5
 * (lower) = pause/resume.
 *
 * Twin of home_sounds.py (aiot-development-for-smart-farm/s3/models/home_sounds.py). Style: CLEAN_C.md.
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
 *   - labels[c].upper() changes the ASCII letters a-z, which is what
 *     the model's labels hold.
 *   - With no "water_tap" class, eps[tap] is the last sound's
 *     episode in the .py (index -1): an info episode that never
 *     latches, so ACK and the ACK colour use that one here too.
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
static const char MODEL_NAME[] = "HomeSounds"; /* exact name */
/* BUILTIN (false = deployed from the Store): in source_matches() */
/* TAP_LABEL "water_tap", the latched sound: in prepare_sounds() */

enum {
    VOTE_NEED = 3,        /* an event = 3 of ... */
    VOTE_WINDOW = 5,      /* ... the last 5 results at Confirm at */
    REFRACT_MS = 1500,    /* no second event sooner than this */
    HOLD_MS = 2000,       /* an event stays on screen this long */
    DEFAULT_HEARING = 40, /* Hearing at, when the board cannot say */
    DEFAULT_CONFIRM = 65, /* Confirm at */
    DEFAULT_CLEAR = 30,   /* clear below */
    POLL_MS = 100,
    DRAW_MS = 500,
    TAP_S = 30 /* water_tap running this long = alarm */
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
    HEALTH_MS = 2000,
    MS_PER_S = 1000,
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    SOUNDS_MAX = 4,   /* class 0 (background) and three sounds */
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
    DISC_PX = 28,
    STATE_DX = 36
};

/* Every widget the program changes later has a slot. */
enum {
    TILES = 3,
    SLOT_HEARTBEAT = 0,
    SLOT_TILES = 1, /* TILES slots each */
    SLOT_DISCS = SLOT_TILES + TILES,
    SLOT_STATES = SLOT_DISCS + TILES,
    SLOT_LOG = SLOT_STATES + TILES,
    SLOT_ACKNOWLEDGE = SLOT_LOG + 1,
    SLOT_STATUS = SLOT_ACKNOWLEDGE + 1,
    SLOT_HEALTH = SLOT_STATUS + 1,
    SLOT_COUNT = SLOT_HEALTH + 1,
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

/* One sound: its vote, its episode, muted or not, its last time. */
typedef struct {
    vote_t vote;
    episode_t episode;
    bool muted;
    char last[CLOCK_CHARS]; /* "" = none yet */
} sound_t;

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
    uint32_t sound_count; /* the labels, at most SOUNDS_MAX */
    int32_t tap;          /* the water_tap class, -1 = none */
    sound_t sounds[SOUNDS_MAX];
    reader_t reader;
    char stop_text[TEXT_CHARS]; /* "" until the program stops itself */
} app_t;

/* The loop's state. */
typedef struct {
    bento_edgeai_thresholds_t thresholds;
    board_button_t pause_button;
    board_button_t alarm_button;
    bool tap_running;   /* the tap was confirmed ... */
    uint32_t tap_on_ms; /* ... at this time, until cleared */
    bool paused;
    bool blink;
    int32_t results;
    bento_real_t latency_ms;
    uint32_t drawn_ms;
    uint32_t polled_ms;
    uint32_t health_ms;
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

/* ---- 4) This model's screen (T5 tiles) ---- */

/* Three of one kind of widget, one per tile at x 28, 280, 532. */
static bento_status_t add_row(app_t *app, const widget_spec_t *first)
{
    static const int16_t tile_x[TILES] = {28, 280, 532};
    widget_spec_t spec = *first;
    uint32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0U; (i < (uint32_t)TILES) && (status == BENTO_OK); i++) {
        spec.slot = (uint8_t)(first->slot + i);
        spec.x = (int16_t)(tile_x[i] + first->x);
        status = add_widget(app, &spec);
    }
    return status;
}

/* The title, the heartbeat and the "Sounds" card. */
static bento_status_t add_header(app_t *app)
{
    static const widget_spec_t top[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Three-Sound Board", 12, 4, AUTO,
         AUTO, COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LED, SLOT_HEARTBEAT, NULL, 756, 6, 26, 26, COLOR_OK,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Detects: baby cry, cough, running tap. Other sounds read as "
         "unlabeled.",
         12, 38, AUTO, AUTO, COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 12, 64, 776, 272, COLOR_CARD,
         COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Sounds", 12 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
    };

    return add_widgets(app, top, sizeof(top) / sizeof(top[0]));
}

static bento_status_t build_screen(app_t *app)
{
    /* x is added to each tile's x */
    static const widget_spec_t rows[] = {
        {UI_WIDGET_BUTTON, SLOT_TILES, " ", 0, 96, 236, 130,
         COLOR_BUTTON, 0, 0, 20},
        /* a round Panel: an Led keeps its first colour on the board */
        {UI_WIDGET_PANEL, SLOT_DISCS, NULL, 0, 236, DISC_PX, DISC_PX,
         COLOR_DIM, COLOR_DIM, DISC_PX / 2, 0},
        {UI_WIDGET_LABEL, SLOT_STATES, "Quiet", STATE_DX, 240, AUTO,
         AUTO, COLOR_DIM, 0, 0, LABEL_PX},
    };
    static const widget_spec_t bottom[] = {
        {UI_WIDGET_LABEL, SLOT_LOG, " ", 28, 280, AUTO, AUTO,
         COLOR_TEXT, 0, 0, LABEL_PX},
        {UI_WIDGET_BUTTON, SLOT_ACKNOWLEDGE, "ACK", 508, 270, 264, 56,
         COLOR_BUTTON, 0, 0, 24},
        {UI_WIDGET_LABEL, SLOT_STATUS, " ", 12, 346, AUTO, AUTO,
         COLOR_WARN, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_HEALTH, " ", 12, 370, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
    };
    uint32_t i;
    bento_status_t status = clear_screen(app);

    if (status == BENTO_OK) {
        status = add_header(app);
    }
    for (i = 0U;
         (i < (sizeof(rows) / sizeof(rows[0]))) && (status == BENTO_OK);
         i++) {
        status = add_row(app, &rows[i]);
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

/* The tap alarm's line: grey > amber > red until ACK. on_s = seconds
 * the tap has run (0 = not running). */
static void tap_text(const sound_t *sound, int32_t on_s, char *text,
                     size_t cap)
{
    const uint32_t state = sound->episode.state;

    if (state == STATE_NORMAL) {
        (void)snprintf(text, cap, "Quiet");
    } else if ((state == STATE_HEARD) && (on_s != 0)) {
        (void)snprintf(text, cap, "! running %" PRId32 " s", on_s);
    } else if (state == STATE_HEARD) {
        (void)snprintf(text, cap, "! hearing");
    } else if (state == STATE_UNACK) {
        (void)snprintf(text, cap, "!! RUNNING %" PRId32 " s - ACK",
                       on_s);
    } else if (state == STATE_ACKED) {
        (void)snprintf(text, cap, "ACKED - running %" PRId32 " s",
                       on_s);
    } else {
        (void)snprintf(text, cap, "ENDED %s - ACK", sound->episode.end);
    }
}

/* An info sound's line: white while recognised. */
static void info_text(const app_t *app, uint32_t klass, char *text,
                      size_t cap)
{
    const uint32_t state = app->sounds[klass].episode.state;

    if (state == STATE_SEEN) {
        upper_text(app->labels[klass], text, cap);
    } else if (state == STATE_HEARD) {
        (void)snprintf(text, cap, "maybe %s", app->labels[klass]);
    } else {
        (void)snprintf(text, cap, "Quiet");
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

/* One tile: its text, its light and its line. */
static void draw_sound(app_t *app, const run_t *run, uint32_t klass,
                       int32_t on_s)
{
    static const uint32_t tap_colors[STATE_COUNT] = {
        COLOR_DIM, COLOR_WARN, COLOR_BAD, COLOR_TEXT, COLOR_TEXT};
    const sound_t *sound = &app->sounds[klass];
    const uint32_t tile = klass - 1U;
    char text[TEXT_CHARS];
    uint32_t light = COLOR_DIM;
    uint32_t color = COLOR_DIM;

    (void)snprintf(text, sizeof(text), "%s %" PRId32 "x\nlast %s%s",
                   app->labels[klass], sound->episode.count,
                   (sound->last[0] != '\0') ? sound->last : "-",
                   sound->muted ? "\nMUTED" : "");
    put_text(app, SLOT_TILES + tile, text);
    if (sound->muted) {
        (void)snprintf(text, sizeof(text), "muted");
    } else if ((int32_t)klass == app->tap) {
        light = disc_color(sound->episode.state, run->blink);
        color = tap_colors[sound->episode.state];
        tap_text(sound, on_s, text, sizeof(text));
    } else {
        light = (sound->episode.state == STATE_SEEN) ? COLOR_TEXT
                                                     : COLOR_DIM;
        color = light;
        info_text(app, klass, text, sizeof(text));
    }
    put_color(app, SLOT_DISCS + tile, light);
    put(app, SLOT_STATES + tile, text, color);
}

/* eps[tap]: the tap, or the last sound when there is none. */
static episode_t *latched_episode(app_t *app)
{
    const uint32_t klass =
        (app->tap >= 0) ? (uint32_t)app->tap : (app->sound_count - 1U);

    return &app->sounds[klass].episode;
}

static void draw(app_t *app, const run_t *run, int32_t on_s)
{
    const uint32_t state = latched_episode(app)->state;
    uint32_t klass;

    for (klass = 1U; klass < app->sound_count; klass++) {
        draw_sound(app, run, klass, on_s);
    }
    put_color(app, SLOT_ACKNOWLEDGE,
              ((state == STATE_UNACK) || (state == STATE_ENDED))
                  ? COLOR_BAD
                  : COLOR_BUTTON);
}

/* ---- 5) Main program ---- */

static const char *help_text(const app_t *app)
{
    return app->has_buttons
               ? "Listening - tap a tile = mute, SW6 = ACK, SW5 = pause"
               : "Listening - tap a tile = mute";
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

/* The tap runs once confirmed, until cleared; its alarm needs TAP_S
 * of running. *hit: the vote, turned into "ran long enough". */
static bool track_tap(run_t *run, bool *hit, bool clear,
                      uint32_t now_ms)
{
    if (clear) {
        run->tap_running = false;
    } else if (!run->tap_running && *hit) {
        run->tap_running = true;
        run->tap_on_ms = now_ms;
    } else {
        /* still running, or still quiet */
    }
    *hit = run->tap_running &&
           (bento_time_ticks_diff(now_ms, run->tap_on_ms) >=
            ((int32_t)TAP_S * MS_PER_S));
    return run->tap_running;
}

/* One sound of a new result: its vote, the tap and its episode. */
static void take_sound(app_t *app, run_t *run,
                       const bento_edgeai_result_t *result,
                       uint32_t klass, uint32_t now_ms)
{
    sound_t *sound = &app->sounds[klass];
    const bento_edgeai_thresholds_t *thresholds = &run->thresholds;
    const int32_t percent = result->score_pct[klass];
    bool hit;
    bool clear;
    bool on = false;

    vote_add(&sound->vote, percent);
    hit = vote_hits(&sound->vote, thresholds->confirm[klass]);
    clear = vote_clear(&sound->vote, thresholds->exit[klass]);
    if ((int32_t)klass == app->tap) {
        on = track_tap(run, &hit, clear, now_ms);
    }
    if (!sound->muted &&
        episode_step(&sound->episode,
                     (percent >= (int32_t)thresholds->enter[klass]) ||
                         on,
                     hit, clear, now_ms)) {
        char text[TEXT_CHARS];

        clock_text(sound->last, sizeof(sound->last));
        (void)snprintf(text, sizeof(text),
                       "%s %s %" PRId32 "%% #%" PRId32, sound->last,
                       app->labels[klass], vote_max(&sound->vote),
                       sound->episode.count);
        (void)printf("%s\n", text);
        put_text(app, SLOT_LOG, text);
    }
}

/* Every POLL_MS while not paused. */
static bento_status_t poll_model(app_t *app, run_t *run,
                                 uint32_t now_ms)
{
    bento_edgeai_result_t result;
    bool fresh = false;
    const bento_status_t status = read_result(app, &result, &fresh);
    uint32_t klass;

    if ((status == BENTO_OK) && fresh) {
        const float latency_ms =
            (float)result.latency_us / (float)MS_PER_S;

        run->results++;
        run->latency_ms += (bento_real_t)latency_ms;
        for (klass = 1U; klass < app->sound_count; klass++) {
            take_sound(app, run, &result, klass, now_ms);
        }
    }
    return status;
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

static void show_health(app_t *app, const run_t *run,
                        int32_t elapsed_ms)
{
    const int32_t divisor = (run->results > 0) ? run->results : 1;
    char rate[BENTO_FMT_BUF_SIZE];
    char latency[BENTO_FMT_BUF_SIZE];
    char text[TEXT_CHARS];

    tenths_text((bento_real_t)(run->results * MS_PER_S) /
                    (bento_real_t)elapsed_ms,
                rate);
    tenths_text(run->latency_ms / (bento_real_t)divisor, latency);
    (void)snprintf(text, sizeof(text), "%s results/s | %s ms", rate,
                   latency);
    put_text(app, SLOT_HEALTH, text);
}

/* Every DRAW_MS: the blink, the heartbeat, the tiles and, every
 * HEALTH_MS, the results per second. */
static void redraw(app_t *app, run_t *run, uint32_t now_ms)
{
    const int32_t on_s =
        run->tap_running
            ? (bento_time_ticks_diff(now_ms, run->tap_on_ms) / MS_PER_S)
            : 0;
    int32_t elapsed_ms;

    run->drawn_ms = now_ms;
    run->blink = !run->blink;
    set_value(app, SLOT_HEARTBEAT,
              (run->blink && (run->results != 0)) ? 1 : 0);
    draw(app, run, on_s);
    elapsed_ms = bento_time_ticks_diff(now_ms, run->health_ms);
    if (elapsed_ms >= HEALTH_MS) {
        show_health(app, run, elapsed_ms);
        run->health_ms = now_ms;
        run->results = 0;
        run->latency_ms = (bento_real_t)0.0;
    }
}

/* A tile tap mutes or unmutes that sound. */
static void toggle_mute(app_t *app, run_t *run, uint32_t klass)
{
    sound_t *sound = &app->sounds[klass];
    char stamp[CLOCK_CHARS];

    sound->muted = !sound->muted;
    sound->vote.count = 0U;
    sound->episode.state = STATE_NORMAL;
    if ((int32_t)klass == app->tap) {
        run->tap_running = false;
    }
    clock_text(stamp, sizeof(stamp));
    (void)printf("%s %s %s\n", stamp, app->labels[klass],
                 sound->muted ? "muted" : "on");
}

/* The sound whose tile has this id, 0 = none. */
static uint32_t tile_sound(const app_t *app, bento_ui_id_t widget)
{
    uint32_t found = 0U;
    uint32_t klass;

    for (klass = 1U; klass < app->sound_count; klass++) {
        if (widget == app->ids[SLOT_TILES + klass - 1U]) {
            found = klass;
        }
    }
    return found;
}

/* Taps: ACK, and a tile muting its sound. The answer: ACK was
 * tapped. A failed poll answers no events. */
static bool read_taps(app_t *app, run_t *run)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;
    uint32_t i;
    uint32_t klass;
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
        klass = clicked ? tile_sound(app, events[i].id) : 0U;
        if (klass != 0U) {
            toggle_mute(app, run, klass);
        }
    }
    return tapped;
}

/* SW5: pause stops the AI core; resume starts it again. */
static bento_status_t toggle_pause(app_t *app, run_t *run)
{
    uint32_t klass;
    bento_status_t status;

    run->paused = !run->paused;
    run->tap_running = false;
    for (klass = 0U; klass < app->sound_count; klass++) {
        app->sounds[klass].vote.count = 0U;
    }
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

/* One pass of the loop: buttons, taps, pause, results, drawing. */
static bento_status_t run_once(app_t *app, run_t *run)
{
    uint32_t now_ms;
    bool acknowledged;
    bento_status_t status = sample_button(app, &run->pause_button);

    if (status == BENTO_OK) {
        status = sample_button(app, &run->alarm_button);
    }
    if (status == BENTO_OK) {
        acknowledged = pressed_now(&run->alarm_button);
        if (read_taps(app, run)) {
            acknowledged = true;
        }
        if (acknowledged) {
            episode_acknowledge(latched_episode(app));
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
    if ((status == BENTO_OK) &&
        (bento_time_ticks_diff(now_ms, run->drawn_ms) >= DRAW_MS)) {
        redraw(app, run, now_ms);
    }
    return status;
}

/* The sounds: at most SOUNDS_MAX classes, and which one is the tap
 * (the only latched episode). */
static void prepare_sounds(app_t *app)
{
    static const char tap_label[] = "water_tap"; /* TAP_LABEL */
    uint32_t klass;

    app->sound_count = (app->label_count < (uint32_t)SOUNDS_MAX)
                           ? app->label_count
                           : (uint32_t)SOUNDS_MAX;
    app->tap = -1;
    for (klass = 0U; klass < app->sound_count; klass++) {
        if ((app->tap < 0) &&
            (strcmp(app->labels[klass], tap_label) == 0)) {
            app->tap = (int32_t)klass;
        }
    }
    for (klass = 0U; klass < app->sound_count; klass++) {
        app->sounds[klass].episode.latch = (int32_t)klass == app->tap;
    }
}

static bento_status_t run_model(app_t *app)
{
    run_t run;
    bento_status_t status = BENTO_E_INVALID_ARGUMENT;

    (void)memset(&run, 0, sizeof(run));
    read_thresholds(app->model_index, &run.thresholds);
    prepare_sounds(app);
    run.pause_button.index = PAUSE_BUTTON;
    run.alarm_button.index = ALARM_BUTTON;
    if (app->sound_count > 0U) { /* eps[-1] of no sounds fails there */
        status = start_model(app, false);
    }
    run.drawn_ms = bento_time_ticks_ms();
    run.polled_ms = run.drawn_ms;
    run.health_ms = run.drawn_ms;
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
    } else if ((status == BENTO_OK) && !supported) {
        status = BENTO_E_UNSUPPORTED; /* import edge_ai fails */
    } else {
        /* a board with the model link, or a link error that ends it */
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
