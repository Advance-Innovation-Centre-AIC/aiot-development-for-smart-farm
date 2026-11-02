/*
 * anomalous_vibration.c -- Pump Health: เตือนเมื่อการสั่นผิดจากเครื่องที่โมเดลเคยเรียน
 * (โมเดลจาก Edge AI Store)
 *
 * ใช้โมเดล "AnomalousVibration" ที่ติดตั้งจาก Edge AI Store บน TESAIoT
 * Dev Kit แปะบอร์ดไว้กับพัดลมที่มีตะแกรงกั้น หน้าปัดบอกคะแนนความผิดปกติ
 * การ์ด "Anomaly" บอก Normal / ALERT / DANGER นับครั้งที่เตือน และค่าสูงสุด
 * การ์ด "Thresholds" มีแถบเลื่อนสองแถบตั้งเกณฑ์ Alert (Hearing at) และ
 * Danger (Confirm at) สัญญาณ DANGER สีแดงค้างอยู่จนกว่าจะรับทราบ: แตะ ACK
 * หรือกด SW6 (ปุ่มบน) ส่วน SW5 (ปุ่มล่าง) หยุดชั่วคราว/ทำต่อ จบโปรแกรม
 * เมื่อไรก็คืนเกณฑ์เดิมให้บอร์ด
 *
 * Runs the Store model "AnomalousVibration" on a TESAIoT Dev Kit.
 * Tape the board to a guarded fan. The gauge shows the anomaly score;
 * the "Anomaly" card says Normal, ALERT or DANGER and counts the alerts
 * and the dangers, with the peak score. Two sliders on the
 * "Thresholds" card set Alert (Hearing at) and Danger (Confirm at).
 * The red DANGER stays until it is acknowledged: tap ACK or press SW6
 * (upper). SW5 (lower) = pause/resume. Whenever the program ends, the
 * board gets its original thresholds back.
 *
 * Twin of anomalous_vibration.py (aiot-development-for-smart-farm/s3/models/anomalous_vibration.py). Style: CLEAN_C.md.
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
 *   - A score is the board's whole percent: int(score * 100 + 0.5)
 *     gives it back unchanged, so the percent is used as it comes. A
 *     model with one class reads class 1 as 0 % and "" where the .py
 *     raises IndexError.
 *   - The dict of pending slider values is two entries kept in the
 *     order they first arrived, as the dict iterates.
 *   - `w.get("th") is not th` (a new read-back) is a counter that
 *     every read-back moves on.
 *   - The thresholds are put back in run's finally also after Stop,
 *     as MicroPython runs finally after KeyboardInterrupt.
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
static const char MODEL_NAME[] = "AnomalousVibration"; /* exact name */
/* BUILTIN (false = deployed from the Store) lives in source_matches() */

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

/* The verdict: Normal, ALERT (not latched) or DANGER. */
enum {
    VERDICT_NORMAL = 0,
    VERDICT_ALERT = 1,
    VERDICT_DANGER = 2,
    VERDICT_COUNT = 3
};

/* Times, counts and sizes. */
enum {
    SETTLE_MS = 150, /* let the cleared screen settle */
    LOOP_MS = 20,
    OP_POLL_MS = 20,  /* ask a pending select or stop this often */
    SILENT_MS = 3000, /* result() failing this long = Stop */
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    ANOMALY_CLASS = 1, /* class 1: anomaly */
    TEXT_CHARS = 128,  /* a SET_TEXT carries at most 126 bytes */
    CLOCK_CHARS = 36,  /* "HH:MM:SS" of any three int32_t */
    PAUSE_BUTTON = 1,  /* SW5, the lower button */
    ALARM_BUTTON = 0,  /* SW6, the upper button: ACK */
    PENDING_MAX = 2,   /* the two sliders */
    THRESHOLD_KINDS = 3
};

/* Layout on the 792 x 398 canvas, and the gauge. */
enum {
    AUTO = -1, /* x, y, w or h the .py leaves out */
    LABEL_PX = 16,
    CARD_RADIUS = 12,
    CARD_TITLE_DX = 14,
    CARD_TITLE_DY = 8,
    PERCENT = 100,
    TICKS_TOTAL = 11, /* 0 10 20 ... 100 */
    TICKS_MAJOR = 2,  /* a number every 2 ticks */
    NEEDLE_PX = 60,
    NEEDLE_SHIFT = 16 /* needle = (length << 16) | value */
};

/* Every widget the program changes later has a slot. */
enum {
    SLOT_HEARTBEAT = 0,
    SLOT_GAUGE = 1,
    SLOT_DISC = 2,
    SLOT_VERDICT = 3,
    SLOT_STATE = 4,
    SLOT_LINES = 5, /* two slots */
    SLOT_HEARING_TEXT = SLOT_LINES + 2,
    SLOT_HEARING = SLOT_HEARING_TEXT + 1,
    SLOT_CONFIRM_TEXT = SLOT_HEARING + 1,
    SLOT_CONFIRM = SLOT_CONFIRM_TEXT + 1,
    SLOT_ACKNOWLEDGE = SLOT_CONFIRM + 1,
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
    int32_t needle; /* the needle's value on screen, -1 = none */
    int32_t shown_readback; /* the read-back on screen, -1 = none */
    bool has_buttons;
    uint32_t model_index;
    int32_t model_count; /* rows when the model was found */
    uint32_t label_count;
    char labels[MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
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

/* Slider values waiting for the next draw, in arrival order. */
typedef struct {
    uint32_t which[PENDING_MAX]; /* BENTO_EDGEAI_TH_ENTER or _CONFIRM */
    int32_t value[PENDING_MAX];
    uint32_t count;
} pending_t;

/* The loop's state. */
typedef struct {
    bento_edgeai_thresholds_t thresholds; /* the latest read-back */
    bento_edgeai_thresholds_t original;   /* put back at the end */
    int32_t readback;                     /* moves on every read-back */
    board_button_t pause_button;
    board_button_t alarm_button;
    vote_t vote;
    episode_t episode;
    pending_t pending;
    int32_t alerts;
    int32_t top; /* the highest score of the run */
    bool alert;
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

/* set_th(): the board keeps clear <= Hearing at <= Confirm at, so
 * show what it reads back. A refused change is printed. */
static void set_threshold(uint32_t index, run_t *run, uint32_t which,
                          int32_t percent)
{
    /* a slider answers 0..100 */
    const uint32_t value = (percent > 0) ? (uint32_t)percent : 0U;
    const bento_status_t status =
        bento_edgeai_threshold_set(index, ANOMALY_CLASS, which, value);

    if (status != BENTO_OK) {
        (void)printf("cannot set threshold: %s\n",
                     link_error_text(status));
    }
    read_thresholds(index, &run->thresholds);
    run->readback++;
}

static bool same_thresholds(const bento_edgeai_thresholds_t *left,
                            const bento_edgeai_thresholds_t *right)
{
    return (memcmp(left->enter, right->enter, sizeof(left->enter)) ==
            0) &&
           (memcmp(left->confirm, right->confirm,
                   sizeof(left->confirm)) == 0) &&
           (memcmp(left->exit, right->exit, sizeof(left->exit)) == 0);
}

/* restore_th(): put back what this program changed; enter, confirm,
 * exit in this order undoes the board's clamp. */
static void restore_thresholds(uint32_t index, run_t *run)
{
    static const uint32_t order[THRESHOLD_KINDS] = {
        BENTO_EDGEAI_TH_ENTER, BENTO_EDGEAI_TH_CONFIRM,
        BENTO_EDGEAI_TH_EXIT};
    const int32_t original[THRESHOLD_KINDS] = {
        run->original.enter[ANOMALY_CLASS],
        run->original.confirm[ANOMALY_CLASS],
        run->original.exit[ANOMALY_CLASS]};
    uint32_t i;

    if (!same_thresholds(&run->thresholds, &run->original)) {
        for (i = 0U; i < (uint32_t)THRESHOLD_KINDS; i++) {
            set_threshold(index, run, order[i], original[i]);
        }
    }
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

/* ---- 4) This model's screen (T2 gauge) ---- */

/* The title, the heartbeat and the "Anomaly" card's gauge. */
static bento_status_t add_gauge(app_t *app)
{
    static const widget_spec_t top[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Pump Health", 12, 4, AUTO, AUTO,
         COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LED, SLOT_HEARTBEAT, NULL, 756, 6, 26, 26, COLOR_OK,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Detects: vibration unlike its training machine. Cannot: name "
         "the fault.",
         12, 38, AUTO, AUTO, COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 12, 64, 470, 272, COLOR_CARD,
         COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Anomaly", 12 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_SCALE, SLOT_GAUGE, NULL, 28, 96, 170, 170,
         COLOR_TEXT, 0, PERCENT, 0},
    };
    bento_status_t status =
        add_widgets(app, top, sizeof(top) / sizeof(top[0]));

    if (status == BENTO_OK) {
        /* the setters ignore a refusal, as modui.c does */
        (void)bento_ui_set_prop(app->ui, app->ids[SLOT_GAUGE],
                                BENTO_UI_PROP_SCALE_MODE,
                                BENTO_UI_SCALE_ROUND_IN);
        (void)bento_ui_set_ticks(app->ui, app->ids[SLOT_GAUGE],
                                 TICKS_TOTAL, TICKS_MAJOR);
        (void)bento_ui_set_prop(app->ui, app->ids[SLOT_GAUGE],
                                BENTO_UI_PROP_SCALE_NEEDLE_COLOR,
                                COLOR_TEXT);
    }
    return status;
}

/* The round light, the verdict, the state and the two count lines. */
static bento_status_t add_verdict(app_t *app)
{
    static const widget_spec_t rest[] = {
        /* a round light */
        {UI_WIDGET_PANEL, SLOT_DISC, NULL, 214, 100, 56, 56, COLOR_DIM,
         COLOR_DIM, 28, 0},
        {UI_WIDGET_LABEL, SLOT_VERDICT, "Normal", 280, 108, AUTO, AUTO,
         COLOR_DIM, 0, 0, 28},
        {UI_WIDGET_LABEL, SLOT_STATE, " ", 214, 166, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_LINES, " ", 214, 194, AUTO, AUTO,
         COLOR_TEXT, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_LINES + 1, " ", 214, 194 + 24, AUTO,
         AUTO, COLOR_TEXT, 0, 0, LABEL_PX},
    };

    return add_widgets(app, rest, sizeof(rest) / sizeof(rest[0]));
}

/* The "Thresholds" card: a line and a slider each for Hearing at and
 * Confirm at. */
static bento_status_t add_thresholds_card(app_t *app)
{
    static const widget_spec_t card[] = {
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 494, 64, 294, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Thresholds", 494 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_HEARING_TEXT, " ", 508, 96, AUTO, AUTO,
         COLOR_TEXT, 0, 0, LABEL_PX},
        {UI_WIDGET_SLIDER, SLOT_HEARING, NULL, 520, 96 + 28, 250, 12, 0,
         0, PERCENT, 0},
        {UI_WIDGET_LABEL, SLOT_CONFIRM_TEXT, " ", 508, 150, AUTO, AUTO,
         COLOR_TEXT, 0, 0, LABEL_PX},
        {UI_WIDGET_SLIDER, SLOT_CONFIRM, NULL, 520, 150 + 28, 250, 12,
         0, 0, PERCENT, 0},
    };
    bento_status_t status =
        add_widgets(app, card, sizeof(card) / sizeof(card[0]));

    if (status == BENTO_OK) {
        /* a Slider's colour is set after creation */
        (void)bento_ui_set_color(app->ui, app->ids[SLOT_HEARING],
                                 COLOR_INFO);
        (void)bento_ui_set_color(app->ui, app->ids[SLOT_CONFIRM],
                                 COLOR_INFO);
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
        status = add_gauge(app);
    }
    if (status == BENTO_OK) {
        status = add_verdict(app);
    }
    if (status == BENTO_OK) {
        status = add_thresholds_card(app);
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
static void state_text(const episode_t *episode, int32_t percent,
                       char *text, size_t cap)
{
    if (episode->state == STATE_NORMAL) {
        (void)snprintf(text, cap, "score %" PRId32 "%%", percent);
    } else if (episode->state == STATE_HEARD) {
        (void)snprintf(text, cap, "! ALERT - score %" PRId32 "%%",
                       percent);
    } else if (episode->state == STATE_UNACK) {
        (void)snprintf(text, cap, "!! DANGER - tap ACK");
    } else if (episode->state == STATE_ACKED) {
        (void)snprintf(text, cap, "ACKED - still high");
    } else {
        (void)snprintf(text, cap, "ENDED %s - tap ACK", episode->end);
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

/* Normal, ALERT or DANGER: DANGER while the alarm holds. */
static uint32_t verdict_of(const run_t *run)
{
    const uint32_t state = run->episode.state;
    uint32_t verdict = VERDICT_NORMAL;

    if ((state == STATE_UNACK) || (state == STATE_ACKED)) {
        verdict = VERDICT_DANGER;
    } else if (run->alert) {
        verdict = VERDICT_ALERT;
    } else {
        /* neither */
    }
    return verdict;
}

/* The needle, and the threshold lines and sliders after a new
 * read-back. */
static void draw_gauge(app_t *app, const run_t *run, int32_t percent)
{
    char text[TEXT_CHARS];
    const int32_t hearing = run->thresholds.enter[ANOMALY_CLASS];
    const int32_t confirm = run->thresholds.confirm[ANOMALY_CLASS];

    if (app->needle != percent) {
        const uint32_t needle =
            ((uint32_t)NEEDLE_PX << (uint32_t)NEEDLE_SHIFT) |
            (uint32_t)percent;

        app->needle = percent;
        (void)bento_ui_set_prop(app->ui, app->ids[SLOT_GAUGE],
                                BENTO_UI_PROP_SCALE_NEEDLE,
                                (int32_t)needle);
    }
    if (app->shown_readback != run->readback) {
        app->shown_readback = run->readback;
        (void)snprintf(text, sizeof(text),
                       "Hearing at %" PRId32 "%% = Alert", hearing);
        put_text(app, SLOT_HEARING_TEXT, text);
        (void)snprintf(text, sizeof(text),
                       "Confirm at %" PRId32 "%% = Danger", confirm);
        put_text(app, SLOT_CONFIRM_TEXT, text);
        set_value(app, SLOT_HEARING, hearing);
        set_value(app, SLOT_CONFIRM, confirm);
    }
}

static void draw(app_t *app, const run_t *run)
{
    static const char *const verdicts[VERDICT_COUNT] = {
        "Normal", "ALERT", "DANGER"};
    static const uint32_t verdict_colors[VERDICT_COUNT] = {
        COLOR_DIM, COLOR_WARN, COLOR_BAD};
    static const uint32_t state_colors[STATE_COUNT] = {
        COLOR_DIM, COLOR_WARN, COLOR_BAD, COLOR_BAD, COLOR_TEXT};
    const uint32_t state = run->episode.state;
    const uint32_t verdict = verdict_of(run);
    const int32_t percent = (run->peak > 0) ? run->peak : 0;
    const bool flashing =
        (state == STATE_UNACK) || (state == STATE_ENDED);
    char text[TEXT_CHARS];

    set_value(app, SLOT_HEARTBEAT,
              (run->blink && (run->peak >= 0)) ? 1 : 0);
    put_color(app, SLOT_DISC, disc_color(state, run->blink));
    state_text(&run->episode, percent, text, sizeof(text));
    put(app, SLOT_STATE, text, state_colors[state]);
    put(app, SLOT_VERDICT, verdicts[verdict], verdict_colors[verdict]);
    put_color(app, SLOT_ACKNOWLEDGE,
              flashing ? COLOR_BAD : COLOR_BUTTON);
    (void)snprintf(text, sizeof(text),
                   "Alerts %" PRId32 " | Dangers %" PRId32, run->alerts,
                   run->episode.count);
    put_text(app, SLOT_LINES, text);
    (void)snprintf(text, sizeof(text), "Peak %" PRId32 "%%", run->top);
    put_text(app, SLOT_LINES + 1U, text);
    draw_gauge(app, run, percent);
}

/* ---- 5) Main program ---- */

static const char *help_text(const app_t *app)
{
    return app->has_buttons ? "Running - SW6 = ACK, SW5 = pause"
                            : "Running";
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

/* The alert line on the console: the time and the score. */
static void print_alert(int32_t percent)
{
    char stamp[CLOCK_CHARS];

    clock_text(stamp, sizeof(stamp));
    (void)printf("%s alert %" PRId32 "%%\n", stamp, percent);
}

/* A new danger on the console. */
static void print_danger(const app_t *app, const run_t *run)
{
    const char *label = (ANOMALY_CLASS < (int32_t)app->label_count)
                            ? app->labels[ANOMALY_CLASS]
                            : "";
    char stamp[CLOCK_CHARS];

    clock_text(stamp, sizeof(stamp));
    (void)printf("%s %s %" PRId32 "%% #%" PRId32 "\n", stamp, label,
                 vote_max(&run->vote), run->episode.count);
}

/* One new result: the vote, the alert (not latched) and the alarm. */
static void take_result(const app_t *app, run_t *run,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    const int32_t percent = result->score_pct[ANOMALY_CLASS];
    const int32_t hearing = run->thresholds.enter[ANOMALY_CLASS];
    const int32_t confirm = run->thresholds.confirm[ANOMALY_CLASS];
    const int32_t clear = run->thresholds.exit[ANOMALY_CLASS];
    const bool was_alert = run->alert;

    vote_add(&run->vote, percent);
    if (percent > run->peak) {
        run->peak = percent;
    }
    if (percent > run->top) {
        run->top = percent;
    }
    run->alert = (percent >= hearing) ||
                 (run->alert && !vote_clear(&run->vote, clear));
    if (run->alert && !was_alert) {
        run->alerts++;
        print_alert(percent);
    }
    if (episode_step(&run->episode, percent >= hearing,
                     vote_hits(&run->vote, confirm),
                     vote_clear(&run->vote, clear), now_ms)) {
        print_danger(app, run);
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

/* A slider moved: kept until the next draw; a later value of the
 * same slider replaces the earlier one in its place. */
static void add_pending(pending_t *pending, uint32_t which,
                        int32_t value)
{
    uint32_t i;
    bool found = false;

    for (i = 0U; i < pending->count; i++) {
        if (pending->which[i] == which) {
            pending->value[i] = value;
            found = true;
        }
    }
    if (!found && (pending->count < (uint32_t)PENDING_MAX)) {
        pending->which[pending->count] = which;
        pending->value[pending->count] = value;
        pending->count++;
    }
}

/* Every DRAW_MS: the blink, the pending sliders and the screen. */
static void redraw(app_t *app, run_t *run, uint32_t now_ms)
{
    uint32_t i;

    run->drawn_ms = now_ms;
    run->blink = !run->blink;
    for (i = 0U; i < run->pending.count; i++) {
        set_threshold(app->model_index, run, run->pending.which[i],
                      run->pending.value[i]);
    }
    run->pending.count = 0U;
    draw(app, run);
    run->peak = -1;
}

/* Taps on any event of ACK, and the sliders' new values. Answers
 * whether ACK was tapped. A failed poll answers no events. */
static bool read_taps(const app_t *app, run_t *run)
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
        const bento_ui_id_t widget = events[i].id;

        /* the .py reads only the handle here: any event of ACK */
        if (widget == app->ids[SLOT_ACKNOWLEDGE]) {
            tapped = true;
        }
        if (events[i].type == (uint8_t)BENTO_UI_EVENT_VALUE_CHANGED) {
            add_pending(&run->pending,
                        (widget == app->ids[SLOT_HEARING])
                            ? BENTO_EDGEAI_TH_ENTER
                            : BENTO_EDGEAI_TH_CONFIRM,
                        events[i].value);
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
    run->alert = false;
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
            episode_acknowledge(&run->episode);
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

/* The loop, then the board's own thresholds back whatever ended it
 * (the sliders' numbers would outlive the program). */
static bento_status_t run_model(app_t *app)
{
    run_t run;
    bento_status_t status;

    (void)memset(&run, 0, sizeof(run));
    read_thresholds(app->model_index, &run.thresholds);
    run.original = run.thresholds;
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
    restore_thresholds(app->model_index, &run);
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
    app->needle = -1;
    app->shown_readback = -1;
    status = bento_edgeai_supported(&supported);
    if ((status == BENTO_OK) && !supported) {
        status = BENTO_E_UNSUPPORTED; /* import edge_ai fails */
    }
    if (status == BENTO_OK) {
        status = find_model(app) ? watch_model(app) : show_missing(app);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
