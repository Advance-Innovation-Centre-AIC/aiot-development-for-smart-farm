/*
 * push_detection.c -- Touchless Button: ปุ่มไร้สัมผัส ดันฝ่ามือเข้าหาเรดาร์
 * (โมเดลในตัว Built-in model ต้องใช้เรดาร์ของ Dev Kit)
 *
 * ใช้โมเดลในตัว "Push Detection" ของ TESAIoT Dev Kit อ่านเรดาร์ ดันฝ่ามือ
 * เข้าหาเรดาร์ที่ระยะราว 60 ซม. การ์ด "Now" มีไฟกลม คำตัดสิน
 * (Ready / maybe push / PUSH) คะแนน และไฟ LED 10 ดวง พร้อมขีด Hearing at
 * (เทา) กับ Confirm at (ขาว) การ์ด "Pushes" นับครั้งที่ดัน เวลาล่าสุด
 * จำนวนครั้งที่ห่างกันไม่ถึง 1 วินาที และสามบรรทัดล่าสุด กด SW6 (ปุ่มบน)
 * หรือแตะปุ่ม Refractory เลือกช่วงพัก 0.3 / 1 / 2 วินาที กด SW5 (ปุ่มล่าง)
 * หยุดชั่วคราว/ทำต่อ โปรแกรมตั้งเกณฑ์สอน Hearing at 20% Confirm at 30%
 * แล้วคืนค่าเดิมของบอร์ดตอนจบ
 *
 * Runs the Dev Kit's built-in model "Push Detection" on the radar:
 * push a palm toward it from about 60 cm. The "Now" card shows a
 * round light, the verdict (Ready / maybe push / PUSH), the score and
 * a 10-LED meter with marks at Hearing at (grey) and Confirm at
 * (white). The "Pushes" card counts the pushes, the last time, the
 * pushes less than 1 s apart and the three newest lines. SW6 (upper)
 * or a tap on Refractory picks 0.3, 1 or 2 s between pushes; SW5
 * (lower) = pause/resume. The program sets the teaching thresholds
 * Hearing at 20 % and Confirm at 30 %, and puts the board's own
 * numbers back at the end.
 *
 * Twin of push_detection.py (aiot-development-for-smart-farm/s3/models/push_detection.py). Style: CLEAN_C.md.
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
 *   - edge_ai.thresholds(i, k, which, pct) waits up to 200 ms for the
 *     board to apply it, then reads the table back; here the same
 *     wait is polled every 4 ms. After Stop the board's numbers are
 *     still sent back, without that wait.
 *   - class Stop is a text kept in the program's state and a failed
 *     status; main() shows and prints it as the except does.
 *   - global REFRACT_MS is the loop's refractory_ms.
 *   - `import buttons` fails on the Eva Kit: here that board has
 *     bento_buttons_count() 0. `import edge_ai` fails where there is
 *     no model link: here bento_edgeai_supported() false ends the
 *     program with BENTO_E_UNSUPPORTED.
 *   - gc.collect() and gc.mem_free(): C has no garbage collector, so
 *     the "free heap:" line is not printed.
 *   - The dict of texts and colours already sent is one entry per
 *     widget slot; a text is kept up to TEXT_CHARS - 1 bytes.
 *   - The only episode here is the info kind (Episode(False)), so the
 *     alarm states UNACK, ACKED and ENDED are left out.
 *   - A tap is any event of the Refractory button, as the .py reads
 *     only ev["handle"].
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
static const char MODEL_NAME[] = "Push Detection"; /* exact name */
/* BUILTIN (false = deployed from the Store) lives in source_matches() */

enum {
    VOTE_NEED = 3,        /* an event = 3 of ... */
    VOTE_WINDOW = 5,      /* ... the last 5 results at Confirm at */
    REFRACT_MS = 1000,    /* no second push sooner (SW6: 0.3, 1, 2 s) */
    HOLD_MS = 2000,       /* an event stays on screen this long */
    DEFAULT_HEARING = 40, /* Hearing at, when the board cannot say */
    DEFAULT_CONFIRM = 65, /* Confirm at */
    DEFAULT_CLEAR = 30,   /* clear below */
    POLL_MS = 100,
    DRAW_MS = 500,
    TEACH_HEARING = 20, /* teaching thresholds (see the header) */
    TEACH_CONFIRM = 30
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

/* The info episode's states: NORMAL > HEARD > SEEN for HOLD_MS. */
enum {
    STATE_NORMAL = 0,
    STATE_HEARD = 1,
    STATE_SEEN = 2
};

/* Times, counts and sizes. */
enum {
    SETTLE_MS = 150, /* let the cleared screen settle */
    LOOP_MS = 20,
    OP_POLL_MS = 20,    /* ask a pending select or stop this often */
    SILENT_MS = 3000,   /* result() failing this long = Stop */
    NEAR_MS = 1000,     /* two pushes closer than this: "< 1 s apart" */
    HANDLED_POLLS = 50, /* 50 x 4 ms = the binding's 200 ms wait */
    HANDLED_MS = 4,
    MAX_CLASSES = BENTO_EDGEAI_MAX_CLASSES,
    TEACH_CLASS = 1, /* the push class */
    METER_LEDS = 10,
    PERCENT_PER_LED = 10,
    PERCENT = 100,
    LOG_LINES = 3,
    REFRACT_CHOICES = 3,
    REFRACT_START = 1, /* 1 s */
    TEXT_CHARS = 128,  /* a SET_TEXT carries at most 126 bytes */
    CLOCK_CHARS = 36,  /* "HH:MM:SS" of any three int32_t */
    PAUSE_BUTTON = 1,  /* SW5, the lower button */
    REFRACT_BUTTON = 0 /* SW6, the upper button */
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
    TICK_X = 28,
    TICK_Y = 256,
    TICK_SPAN_PX = 432, /* the meter's width for 100 % */
    LINES_Y = 188,
    LINE_STEP_PX = 24
};

/* Every widget the program changes later has a slot. */
enum {
    SLOT_HEARTBEAT = 0,
    SLOT_DISC = 1,
    SLOT_VERDICT = 2,
    SLOT_STATE = 3,
    SLOT_METER = 4, /* METER_LEDS slots */
    SLOT_TICK_HEARING = SLOT_METER + (int32_t)METER_LEDS,
    SLOT_TICK_CONFIRM = SLOT_TICK_HEARING + 1,
    SLOT_LEGEND = SLOT_TICK_CONFIRM + 1,
    SLOT_PUSHES = SLOT_LEGEND + 1,
    SLOT_LAST = SLOT_PUSHES + 1,
    SLOT_NEAR = SLOT_LAST + 1,
    SLOT_LINES = SLOT_NEAR + 1, /* LOG_LINES slots */
    SLOT_REFRACTORY = SLOT_LINES + (int32_t)LOG_LINES,
    SLOT_STATUS = SLOT_REFRACTORY + 1,
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

/* The info episode: NORMAL > HEARD > SEEN for HOLD_MS, no ACK. */
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
    bento_edgeai_thresholds_t original; /* the board's own numbers */
    bento_edgeai_thresholds_t thresholds;
    board_button_t pause_button;
    board_button_t refract_button;
    vote_t vote;
    episode_t push;
    int32_t refract_choice;
    int32_t refractory_ms;
    int32_t near;
    bool has_last;
    uint32_t last_ms;
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
        true; /* false = deployed from the Store */
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
        bento_edgeai_thresholds_init(thresholds);
        for (i = 0U; i < (uint32_t)MAX_CLASSES; i++) {
            thresholds->enter[i] = DEFAULT_HEARING;
            thresholds->confirm[i] = DEFAULT_CONFIRM;
            thresholds->exit[i] = DEFAULT_CLEAR;
        }
    }
}

/* The board's count of control commands it has applied. */
static bool commands_handled(uint32_t *handled)
{
    bento_edgeai_diag_t diag;
    bool known;

    bento_edgeai_diag_init(&diag);
    known = bento_edgeai_diag(&diag) == BENTO_OK;
    *handled = diag.ctrl_handled;
    return known;
}

/* The board applies a threshold at its next pass: wait up to 200 ms
 * for its command count to move, as the binding does. */
static bento_status_t wait_handled(uint32_t before)
{
    uint32_t handled = before;
    int32_t poll;
    bento_status_t status = BENTO_OK;
    bool moved = false;

    for (poll = 0;
         (poll < HANDLED_POLLS) && !moved && (status == BENTO_OK);
         poll++) {
        moved = commands_handled(&handled) && (handled != before);
        if (!moved) {
            status = wait_ms(HANDLED_MS);
        }
    }
    return status;
}

/* set_th(): one threshold of one class, then the table the board
 * keeps (it holds clear <= Hearing at <= Confirm at). A refused
 * set prints why and goes on. */
static bento_status_t set_threshold(uint32_t index, uint32_t which,
                                    int32_t percent,
                                    bento_edgeai_thresholds_t *out)
{
    uint32_t before = 0U;
    const bool counted = commands_handled(&before);
    bento_status_t status = bento_edgeai_threshold_set(
        index, TEACH_CLASS, which, (uint32_t)percent);

    if (status != BENTO_OK) {
        (void)printf("cannot set threshold: %s\n",
                     "edge_ai: threshold send failed");
        status = BENTO_OK;
    } else if (counted) {
        status = wait_handled(before);
    } else {
        /* no count to wait for: read back at once */
    }
    read_thresholds(index, out);
    return status;
}

static bool same_thresholds(const bento_edgeai_thresholds_t *left,
                            const bento_edgeai_thresholds_t *right)
{
    return (left->class_count == right->class_count) &&
           (memcmp(left->enter, right->enter, sizeof(left->enter)) ==
            0) &&
           (memcmp(left->confirm, right->confirm,
                   sizeof(left->confirm)) == 0) &&
           (memcmp(left->exit, right->exit, sizeof(left->exit)) == 0);
}

/* restore_th(): put back what this program changed; enter, confirm,
 * exit in this order undoes the board's clamp. Every one is sent,
 * also after Stop. */
static bento_status_t restore_thresholds(uint32_t index,
                                         const run_t *run)
{
    static const uint32_t which[] = {BENTO_EDGEAI_TH_ENTER,
                                     BENTO_EDGEAI_TH_CONFIRM,
                                     BENTO_EDGEAI_TH_EXIT};
    const uint8_t *original[] = {
        run->original.enter, run->original.confirm, run->original.exit};
    bento_edgeai_thresholds_t read_back;
    bento_status_t status = BENTO_OK;
    uint32_t i;

    if (!same_thresholds(&run->thresholds, &run->original)) {
        for (i = 0U; i < (sizeof(which) / sizeof(which[0])); i++) {
            const bento_status_t one = set_threshold(
                index, which[i], (int32_t)original[i][TEACH_CLASS],
                &read_back);

            status = (status == BENTO_OK) ? one : status;
        }
    }
    return status;
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

/* Where the info episode goes when no new event starts. */
static uint32_t settled_state(const episode_t *episode, bool heard,
                              bool clear, uint32_t now_ms)
{
    uint32_t state = episode->state;
    bool held = false;

    if (state == STATE_SEEN) {
        held =
            bento_time_ticks_diff(now_ms, episode->time_ms) < HOLD_MS;
        state = held ? STATE_SEEN : STATE_NORMAL;
    }
    if (!held && clear) {
        state = STATE_NORMAL;
    } else if (!held && heard && (state == STATE_NORMAL)) {
        state = STATE_HEARD;
    } else {
        /* no change */
    }
    return state;
}

/* True once per new event; refractory from the last event. */
static bool episode_step(episode_t *episode, bool heard, bool confirm,
                         bool clear, uint32_t now_ms,
                         int32_t refractory_ms)
{
    const bool rested =
        !episode->has_time ||
        (bento_time_ticks_diff(now_ms, episode->time_ms) >=
         refractory_ms);
    bool is_new = false;

    if (confirm && rested) {
        episode->state = STATE_SEEN;
        episode->count++;
        episode->has_time = true;
        episode->time_ms = now_ms;
        is_new = true;
    } else {
        episode->state = settled_state(episode, heard, clear, now_ms);
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

/* Marks over the meter at Hearing at (grey) and Confirm at
 * (white). */
static void show_ticks(app_t *app, const bento_edgeai_thresholds_t *th)
{
    const int32_t hearing = th->enter[TEACH_CLASS];
    const int32_t confirm = th->confirm[TEACH_CLASS];
    char text[TEXT_CHARS];

    /* a setter's refusal was ignored before too (modui.c) */
    (void)bento_ui_set_pos(
        app->ui, app->ids[SLOT_TICK_HEARING],
        (int16_t)((int32_t)TICK_X + ((hearing * (int32_t)TICK_SPAN_PX) /
                                     (int32_t)PERCENT)),
        (int16_t)TICK_Y);
    (void)bento_ui_set_pos(
        app->ui, app->ids[SLOT_TICK_CONFIRM],
        (int16_t)((int32_t)TICK_X + ((confirm * (int32_t)TICK_SPAN_PX) /
                                     (int32_t)PERCENT)),
        (int16_t)TICK_Y);
    (void)snprintf(text, sizeof(text),
                   "Hearing at %" PRId32 "%% | Confirm at %" PRId32
                   "%%",
                   hearing, confirm);
    put_text(app, SLOT_LEGEND, text);
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

/* ---- 4) This model's screen (T1 annunciator, no chart, no ACK) ---- */

/* The title, the heartbeat and the "Now" card down to its state. */
static bento_status_t add_now_card(app_t *app)
{
    static const widget_spec_t top[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "Touchless Button", 12, 4, AUTO,
         AUTO, COLOR_TEXT, 0, 0, 24},
        {UI_WIDGET_LED, SLOT_HEARTBEAT, NULL, 756, 6, 26, 26, COLOR_OK,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "Detects: a palm push toward the radar, ~60 cm. Cannot: other "
         "gestures. Demo.",
         12, 38, AUTO, AUTO, COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 12, 64, 470, 272, COLOR_CARD,
         COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Now", 12 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        /* a round light */
        {UI_WIDGET_PANEL, SLOT_DISC, NULL, 28, 100, 140, 140, COLOR_DIM,
         COLOR_DIM, 70, 0},
        {UI_WIDGET_LABEL, SLOT_VERDICT, "Ready", 184, 110, AUTO, AUTO,
         COLOR_DIM, 0, 0, 28},
        {UI_WIDGET_LABEL, SLOT_STATE, " ", 184, 150, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
    };

    return add_widgets(app, top, sizeof(top) / sizeof(top[0]));
}

/* The meter's LEDs, the two threshold marks and the legend. */
static bento_status_t add_meter(app_t *app)
{
    static const widget_spec_t led = {
        UI_WIDGET_LED, SLOT_METER, NULL, METER_X, 262, 36, 20,
        COLOR_TEXT,    0,          0,    0};
    static const widget_spec_t marks[] = {
        {UI_WIDGET_PANEL, SLOT_TICK_HEARING, NULL, TICK_X, TICK_Y, 4,
         32, COLOR_DIM, 0, 0, 0},
        {UI_WIDGET_PANEL, SLOT_TICK_CONFIRM, NULL, TICK_X, TICK_Y, 4,
         32, COLOR_TEXT, 0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_LEGEND, " ", 28, 290, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
    };
    widget_spec_t spec = led;
    int32_t i;
    bento_status_t status = BENTO_OK;

    for (i = 0; (i < METER_LEDS) && (status == BENTO_OK); i++) {
        spec.slot = (uint8_t)(SLOT_METER + i);
        spec.x = (int16_t)(METER_X + (METER_STEP_PX * i));
        status = add_widget(app, &spec);
    }
    if (status == BENTO_OK) {
        status =
            add_widgets(app, marks, sizeof(marks) / sizeof(marks[0]));
    }
    return status;
}

/* The "Pushes" card: the count, the last one, the near pushes and
 * the three newest lines. */
static bento_status_t add_pushes_card(app_t *app)
{
    static const widget_spec_t card[] = {
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 494, 64, 294, 272,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1},
        {UI_WIDGET_LABEL, SLOT_NONE, "Pushes", 494 + CARD_TITLE_DX,
         64 + CARD_TITLE_DY, AUTO, AUTO, COLOR_INFO, 0, 0, LABEL_PX},
        {UI_WIDGET_SEG7, SLOT_PUSHES, "0", 508, 96, 120, 56, COLOR_TEXT,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_LAST, "last -", 640, 110, AUTO, AUTO,
         COLOR_DIM, 0, 0, LABEL_PX},
        {UI_WIDGET_LABEL, SLOT_NEAR, "< 1 s apart: 0", 508, 164, AUTO,
         AUTO, COLOR_TEXT, 0, 0, LABEL_PX},
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
        {UI_WIDGET_BUTTON, SLOT_REFRACTORY, "Refractory 1 s", 508, 266,
         266, 60, COLOR_BUTTON, 0, 0, 20},
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
        status = add_pushes_card(app);
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

/* Info kind: grey, then white for HOLD_MS; "maybe" only in dim
 * text. */
static void draw(app_t *app, const run_t *run)
{
    static const char *const verdicts[] = {"Ready", "maybe push",
                                           "PUSH"};
    const uint32_t state = run->push.state;
    const int32_t percent = (run->peak > 0) ? run->peak : 0;
    const uint32_t color =
        (state == STATE_SEEN) ? COLOR_TEXT : COLOR_DIM;
    char text[TEXT_CHARS];

    /* the heartbeat; peak -1 = no new result */
    set_value(app, SLOT_HEARTBEAT,
              (run->blink && (run->peak >= 0)) ? 1 : 0);
    put_color(app, SLOT_DISC, color);
    put(app, SLOT_VERDICT, verdicts[state], color);
    (void)snprintf(text, sizeof(text), "score %" PRId32 "%%", percent);
    put_text(app, SLOT_STATE, text);
    show_meter(app, percent);
}

/* ---- 5) Main program ---- */

static const char *help_text(const app_t *app)
{
    return app->has_buttons
               ? "Push your palm - SW6 = refractory, SW5 = pause"
               : "Push your palm toward the radar";
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

/* A new push: the near count, the log lines, the count and the
 * time. */
static void log_push(app_t *app, run_t *run, uint32_t klass,
                     uint32_t now_ms)
{
    const char *label =
        (klass < app->label_count) ? app->labels[klass] : "";
    char stamp[CLOCK_CHARS];
    char text[TEXT_CHARS];
    uint32_t i;

    if (run->has_last &&
        (bento_time_ticks_diff(now_ms, run->last_ms) < NEAR_MS)) {
        run->near++; /* a double push, or one push counted twice */
        (void)snprintf(text, sizeof(text), "< 1 s apart: %" PRId32,
                       run->near);
        put_text(app, SLOT_NEAR, text);
    }
    run->has_last = true;
    run->last_ms = now_ms;
    clock_text(stamp, sizeof(stamp));
    for (i = (uint32_t)LOG_LINES - 1U; i > 0U; i--) {
        (void)memcpy(app->log[i], app->log[i - 1U],
                     sizeof(app->log[i]));
    }
    (void)snprintf(app->log[0], sizeof(app->log[0]),
                   "%s %s %" PRId32 "%% #%" PRId32, stamp, label,
                   vote_max(&run->vote), run->push.count);
    app->log_count += (app->log_count < (uint32_t)LOG_LINES) ? 1U : 0U;
    (void)printf("%s\n", app->log[0]);
    for (i = 0U; i < app->log_count; i++) { /* newest first */
        put_text(app, (uint32_t)SLOT_LINES + i, app->log[i]);
    }
    (void)snprintf(text, sizeof(text), "%" PRId32, run->push.count);
    put_text(app, SLOT_PUSHES, text);
    clock_text(stamp, sizeof(stamp));
    (void)snprintf(text, sizeof(text), "last %s", stamp);
    put_text(app, SLOT_LAST, text);
}

/* One new result: the vote and the episode. */
static void take_result(app_t *app, run_t *run,
                        const bento_edgeai_result_t *result,
                        uint32_t now_ms)
{
    event_t event;

    event_of(result, &run->thresholds, &event);
    vote_add(&run->vote, event.percent);
    if (event.percent > run->peak) {
        run->peak = event.percent;
    }
    if (episode_step(&run->push, event.percent >= event.hearing,
                     vote_hits(&run->vote, event.confirm),
                     vote_clear(&run->vote, event.clear), now_ms,
                     run->refractory_ms)) {
        log_push(app, run, event.klass, now_ms);
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

/* A tap of Refractory: any event of that button counts, as the .py
 * reads only ev["handle"]. A failed poll answers no events. */
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
        if (events[i].id == app->ids[SLOT_REFRACTORY]) {
            tapped = true;
        }
    }
    return tapped;
}

/* Refractory 0.3 -> 1 -> 2 s. */
static void next_refractory(app_t *app, run_t *run)
{
    static const int32_t refract_ms[REFRACT_CHOICES] = {300, 1000,
                                                        2000};
    static const char *const refract_text[REFRACT_CHOICES] = {"0.3",
                                                              "1", "2"};
    char stamp[CLOCK_CHARS];
    char text[TEXT_CHARS];

    run->refract_choice = (run->refract_choice + 1) % REFRACT_CHOICES;
    run->refractory_ms = refract_ms[run->refract_choice];
    (void)snprintf(text, sizeof(text), "Refractory %s s",
                   refract_text[run->refract_choice]);
    put_text(app, SLOT_REFRACTORY, text);
    clock_text(stamp, sizeof(stamp));
    (void)printf("%s refractory %" PRId32 " ms\n", stamp,
                 run->refractory_ms);
}

/* SW5: pause stops the AI core; resume starts it again. */
static bento_status_t toggle_pause(app_t *app, run_t *run)
{
    bento_status_t status;

    run->paused = !run->paused;
    run->vote.count = 0U;
    run->push.state = STATE_NORMAL;
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

/* Every DRAW_MS: the blink and the screen. */
static void redraw(app_t *app, run_t *run, uint32_t now_ms)
{
    run->drawn_ms = now_ms;
    run->blink = !run->blink;
    draw(app, run);
    run->peak = -1;
}

/* One pass of the loop: buttons, taps, pause, results, drawing. */
static bento_status_t run_once(app_t *app, run_t *run)
{
    uint32_t now_ms;
    bool tapped;
    bento_status_t status = sample_button(app, &run->pause_button);

    if (status == BENTO_OK) {
        status = sample_button(app, &run->refract_button);
    }
    if (status == BENTO_OK) {
        tapped = pressed_now(&run->refract_button);
        if (read_taps(app)) {
            tapped = true;
        }
        if (tapped) {
            next_refractory(app, run);
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

/* The teaching thresholds for the push class, and their marks. */
static bento_status_t teach_thresholds(app_t *app, run_t *run)
{
    bento_status_t status;

    read_thresholds(app->model_index, &run->original);
    run->thresholds = run->original;
    status = set_threshold(app->model_index, BENTO_EDGEAI_TH_ENTER,
                           TEACH_HEARING, &run->thresholds);
    if (status == BENTO_OK) {
        /* exit follows enter down to 20 */
        status =
            set_threshold(app->model_index, BENTO_EDGEAI_TH_CONFIRM,
                          TEACH_CONFIRM, &run->thresholds);
    }
    if (status == BENTO_OK) {
        show_ticks(app, &run->thresholds);
    }
    return status;
}

static bento_status_t loop_model(app_t *app, run_t *run)
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

/* The board keeps the thresholds until reboot: its own are put back
 * whatever ended the loop. */
static bento_status_t run_model(app_t *app)
{
    run_t run;
    bento_status_t status;

    (void)memset(&run, 0, sizeof(run));
    run.pause_button.index = PAUSE_BUTTON;
    run.refract_button.index = REFRACT_BUTTON;
    run.refract_choice = REFRACT_START;
    run.refractory_ms = REFRACT_MS;
    run.peak = -1;
    status = teach_thresholds(app, &run);
    if (status == BENTO_OK) {
        const bento_status_t loop_status = loop_model(app, &run);
        const bento_status_t restore_status =
            restore_thresholds(app->model_index, &run);

        status =
            (loop_status != BENTO_OK) ? loop_status : restore_status;
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
