/*
 * sf3_04_ai_pump_doctor.c -- ให้ AI บนบอร์ดตัดสินแทนกฎ (Edge AI)
 *
 * ใช้โมเดล AI บนบอร์ด (จาก Edge AI Store หรือที่ติดมากับบอร์ด) อ่านการ
 * เคลื่อนไหวจาก IMU แล้วตอบว่าเครื่อง "นิ่ง / หมุนเป็นจังหวะ /
 * สั่นผิดปกติ" พร้อมคะแนนทุกคลาส คลาสอันตรายชนะด้วยความมั่นใจสูง
 * อย่างน้อย 3 ใน 5 ผลล่าสุด = เตือน (เสียง + ไฟแดง) ลองวางบอร์ดนิ่ง
 * ถือบอร์ดวาดวงกลมในอากาศช้า ๆ แล้วเขย่าแรง ๆ ดูวงแหวนความมั่นใจ
 * แถบคะแนนแต่ละคลาส เวลาที่ AI ใช้คิด กราฟ และจอไฟ RGB (ใน Emulator
 * เป็นผลจำลอง: ปุ่ม Shake = shaking, เอียงบอร์ด = circle)
 *
 * An AI model on the board (from the Edge AI Store, or built in)
 * reads the IMU's motion and says whether the machine is "still /
 * turning steadily / shaking abnormally", with every class's score.
 * A danger class winning with high confidence in at least 3 of the
 * last 5 results raises the alarm (a tone and the red LED). Leave
 * the board still, draw a slow circle in the air with it, then shake
 * it hard: watch the confidence ring, each class's bar, the time the
 * AI took to think, the chart and the RGB matrix (simulated in the
 * Emulator: Shake = shaking, tilt = circle).
 *
 * Twin of sf3_04_ai_pump_doctor.py
 * (aiot-development-for-smart-farm/s3/sf3_04_ai_pump_doctor.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - edge_ai.models() reads the whole list or raises; here the rows
 *     are read one by one and a row that cannot be read is skipped.
 *     The labels of a model are read when it is shown.
 *   - edge_ai.select() and edge_ai.stop() wait inside the binding;
 *     here they are started and polled, so Stop ends a wait at once.
 *     A failed select prints the C status number, not the OSError
 *     text. The program also closes the module's Edge AI link
 *     (bento_edgeai_deinit) after the final stop.
 *   - A board without the model link (reTerminal) answers
 *     BENTO_E_UNSUPPORTED to bento_edgeai_init(): the same "no model"
 *     path as an empty edge_ai.models().
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available
 *     yet: the program says so (a line under the title, and on the
 *     console), then takes the .py's own "no model" path.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_edgeai.h"
#include "bento_fmt.h"
#include "bento_gpio.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- 1) Settings (edit) ---- */
/* Tried in order: the Store's model first, else the built-in one.
 * Try the course's sound models (the second one built in):
 * ("HomeSounds", "Cough") ("SirenDetection", "Siren Detection")
 * ("Environment Sounds", "Alarm"), or the radar ("Push"). */
static const char *const MODEL_KEYS[] = {"AnomalousVibration",
                                         "Motion"};
/* The danger classes are in is_danger(), the farm words in
 * meaning_of(). */
/* SENSOR_MIC: now BENTO_EDGEAI_SENSOR_MIC in uses_mic(). */

enum {
    VOLUME = 25,      /* tone loudness 0-127 (about 20 %) */
    SPEAKER = 40,     /* speaker volume 0-100 % (firmware 2.4.2 up) */
    CONF_MIN = 60,    /* below this many % a result is no event */
    CONFIRM_N = 3,    /* a danger class must win this many times ... */
    WINDOW_N = 5,     /* ... in this many latest results */
    STALL_MS = 8000,  /* no new answer this long = say so */
    MUTE_MS = 800,    /* a mic model: no trust after the board's tone */
    MATRIX_MS = 3000, /* send the matrix picture again this often */
    ROWS = 4,         /* score rows: the 4 top classes */
    MAX_CLASSES = 8,  /* the AI core reports at most 8 classes */
    RUN_MS = 120000,
    TICK_MS = 500
};

/* The course palette, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_CARD = 0x171B22,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* The waits, the tones, the matrix and the sizes of what is built. */
enum {
    OP_POLL_MS = 20,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 120,
    NOTE_HIGH = 84, /* the alarm falls 84 -> 76, the all-clear rises */
    NOTE_ALARM_LOW = 76,
    NOTE_CLEAR_LOW = 79,
    PERCENT = 100,
    MS_PER_US = 1000,
    CARD_RADIUS = 12,
    CHART_POINTS = 400, /* no round dots: as many points as pixels */
    MATRIX_W = 16,
    MATRIX_H = 8,
    MATRIX_ROW_BYTES = 8, /* two 4-bit pixels a byte */
    NIBBLE_BITS = 4,
    FOUND_MAX = 8,
    TEXT_BYTES = 128 /* a label's text: a SET_TEXT holds 126 */
};

/* The widgets the program changes, in the order they are made. */
enum {
    SLOT_MODEL = 0,
    SLOT_ARC = 1,
    SLOT_VERDICT = 2,
    SLOT_LED = 3,
    SLOT_MEANING = 4,
    SLOT_CONF = 5,
    SLOT_NAME_0 = 6,
    SLOT_BAR_0 = 7,
    SLOT_NAME_1 = 8,
    SLOT_BAR_1 = 9,
    SLOT_NAME_2 = 10,
    SLOT_BAR_2 = 11,
    SLOT_NAME_3 = 12,
    SLOT_BAR_3 = 13,
    SLOT_CHART = 14,
    SLOT_LATENCY = 15,
    SLOT_STREAK = 16,
    SLOT_STATUS = 17,
    SLOT_COUNT = 18,
    SLOT_NONE = 18 /* a widget nobody changes again */
};

/* One ui.<Widget>(...) call: the kind, the slot that keeps its id,
 * its text, x, y, w, h, color, min, max, value, and the .color()
 * set right after it (0 = none). */
typedef struct {
    uint8_t kind;
    uint8_t slot;
    const char *text;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint32_t color;
    int32_t minimum;
    int32_t maximum;
    int32_t value;
    uint32_t then_color;
} part_t;

/* A model whose name holds one of MODEL_KEYS. */
typedef struct {
    uint32_t index;
    uint32_t class_count;
    char name[BENTO_EDGEAI_NAME_LEN];
} found_t;

/* The picture of the matrix: a bar per class, the winner, alarm. */
typedef struct {
    bool has;
    uint32_t count;
    int32_t heights[MAX_CLASSES];
    uint32_t top;
    bool alerting;
} frame_t;

/* The watch: the last results and what was sent to the matrix. */
typedef struct {
    bool has_sequence;
    uint32_t sequence;
    uint8_t history[WINDOW_N];
    uint32_t history_count;
    int32_t alerts;
    bool alerting;
    bool stalled;
    bool mic;
    uint32_t last_new_ms;
    uint32_t quiet_ms;
    uint32_t sent_ms;
    frame_t frame;
    frame_t drawn;
} watch_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT + 1]; /* + the spare for SLOT_NONE */
    uint8_t threshold_series;
    found_t found[FOUND_MAX];
    uint32_t found_count;
    uint32_t label_count;
    char labels[MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
    bool has_led;
    uint32_t led;
    watch_t watch;
} doctor_t;

/* Too big for the 8 KB program stack: file scope, zeroed by the
 * loader and set again at the start of main() (CLEAN_C.md C9,
 * deviation BENTO-MISRA-E03/E04). */
extern doctor_t g_doctor;
doctor_t g_doctor;

/* ---- 2) Hardware ---- */

/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t wait_ms(uint32_t duration_ms)
{
    bento_status_t status = bento_time_sleep_ms(duration_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* ui.poll() only keeps the display moving here: a failed poll
 * answers no events in MicroPython, so it is not a failure. */
static void poll_display(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static void show_text(const doctor_t *doctor, uint32_t slot,
                      const char *text)
{
    /* MicroPython's widget setters ignore a refusal (modui.c) */
    (void)bento_ui_set_text(doctor->ui, doctor->ids[slot], text,
                            (uint32_t)strlen(text));
}

static void show_color(const doctor_t *doctor, uint32_t slot,
                       uint32_t color)
{
    (void)bento_ui_set_color(doctor->ui, doctor->ids[slot], color);
}

static void show_value(const doctor_t *doctor, uint32_t slot,
                       int32_t value)
{
    (void)bento_ui_set_value(doctor->ui, doctor->ids[slot], value);
}

/* Soft MIDI notes (ui.sfx cannot be made quieter), 120 ms apart. */
static bento_status_t beep(const doctor_t *doctor, uint8_t first,
                           uint8_t second)
{
    const uint8_t notes[2] = {first, second};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < 2U) && (status == BENTO_OK); i++) {
        /* a tone's result is not read in MicroPython either */
        (void)bento_ui_sfx_tone(doctor->ui, notes[i],
                                BENTO_UI_WAVE_SINE, (uint8_t)VOLUME,
                                (uint16_t)TONE_MS);
        status = wait_ms((uint32_t)TONE_MS);
    }
    return status;
}

/* Waits for a started select or stop; Stop cancels it. */
static bento_status_t finish_op(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = wait_ms((uint32_t)OP_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_edgeai_op_poll(NULL);
        } else {
            (void)bento_edgeai_op_cancel(); /* Stop wins */
        }
    }
    return status;
}

/* str.lower() of one ASCII letter; any other byte as it is. */
static char lower_letter(char letter)
{
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    char result = letter;
    uint32_t i;

    for (i = 0U; upper[i] != '\0'; i++) {
        if (upper[i] == letter) {
            result = lower[i];
        }
    }
    return result;
}

/* key.lower() in name.lower() */
static bool name_has(const char *name, const char *key)
{
    const size_t key_length = strlen(key);
    bool match = false;
    size_t start = 0U;

    while ((name[start] != '\0') && !match) {
        size_t i = 0U;

        while (
            (i < key_length) && (name[start + i] != '\0') &&
            (lower_letter(name[start + i]) == lower_letter(key[i]))) {
            i++;
        }
        match = i == key_length;
        start++;
    }
    return match;
}

/* Every model whose name holds key, as edge_ai.models() lists them. */
static void find_key(doctor_t *doctor, const char *key, int32_t count)
{
    bento_edgeai_model_info_t info;
    int32_t row;

    for (row = 0;
         (row < count) && (doctor->found_count < (uint32_t)FOUND_MAX);
         row++) {
        found_t *found = &doctor->found[doctor->found_count];

        bento_edgeai_model_info_init(&info);
        if ((bento_edgeai_model_info((uint32_t)row, &info) ==
             BENTO_OK) &&
            name_has(info.name, key)) {
            found->index = (uint32_t)row;
            found->class_count = info.class_count;
            (void)snprintf(found->name, sizeof(found->name), "%s",
                           info.name);
            doctor->found_count++;
        }
    }
}

/* By name, in MODEL_KEYS order: the order on each board differs
 * (and can change after a reboot). No answer = none found. *linked
 * is false when there is no Edge AI from C at all (the Emulator). */
static void find_models(doctor_t *doctor, bool *linked)
{
    const uint32_t keys =
        (uint32_t)(sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]));
    const bento_status_t status = bento_edgeai_init();
    int32_t count = 0;
    uint32_t i;
    bool has_link = false;

    /* a reTerminal answers init the same way but says "no link" to
     * the query; only the BENTO Emulator's C runtime refuses both */
    *linked = true;
    if (status == BENTO_E_UNSUPPORTED) {
        *linked =
            bento_edgeai_supported(&has_link) != BENTO_E_UNSUPPORTED;
    }
    if ((status != BENTO_OK) ||
        (bento_edgeai_count(&count) != BENTO_OK)) {
        count = 0;
    }
    for (i = 0U; *linked && (i < keys); i++) {
        find_key(doctor, MODEL_KEYS[i], count);
    }
}

/* Does this model listen to the microphone? An unreadable row does
 * not. */
static bool uses_mic(uint32_t index)
{
    bento_edgeai_model_info_t info;

    bento_edgeai_model_info_init(&info);
    return (bento_edgeai_model_info(index, &info) == BENTO_OK) &&
           (info.sensor == BENTO_EDGEAI_SENSOR_MIC);
}

/* edge_ai.stop(): an OSError is ignored there. */
static void stop_ai(void)
{
    if (bento_edgeai_stop_start() == BENTO_OK) {
        (void)finish_op(); /* the .py ignores an OSError from stop() */
    }
}

/* By name, not number: LED1/LED2 of the Dev Kit sit on the SoM where
 * nobody sees them. None when the board cannot say. */
static void find_led(doctor_t *doctor)
{
    bento_gpio_board_info_t info;
    const char *name = NULL;
    uint32_t i;

    bento_gpio_board_info_init(&info);
    doctor->has_led =
        (bento_gpio_board_info(&info) == BENTO_OK) && (info.leds > 0U);
    doctor->led = 0U;
    for (i = 0U; doctor->has_led && (i < info.leds); i++) {
        if ((bento_gpio_led_name(i, &name) == BENTO_OK) &&
            (name != NULL) && (strcmp(name, "RGB_RED") == 0)) {
            doctor->led = i;
        }
    }
    doctor->has_led = doctor->has_led &&
                      (bento_gpio_led_off(doctor->led) == BENTO_OK);
}

static bento_status_t set_led(const doctor_t *doctor, bool on)
{
    bento_status_t status = BENTO_OK;

    if (doctor->has_led) {
        status = on ? bento_gpio_led_on(doctor->led)
                    : bento_gpio_led_off(doctor->led);
    }
    return status;
}

/* One pixel (x, y) of the 64-byte frame, 4 bits a pixel, the odd
 * column in the high nibble. */
static void put_pixel(uint8_t *picture, int32_t x, int32_t y,
                      uint8_t color)
{
    const int32_t at = (y * MATRIX_ROW_BYTES) + (x / 2);
    const uint8_t keep_low = 0x0FU;
    const uint8_t keep_high = 0xF0U;

    if ((x % 2) != 0) {
        picture[at] =
            (uint8_t)((uint32_t)picture[at] & keep_low) |
            (uint8_t)((uint32_t)color << (uint32_t)NIBBLE_BITS);
    } else {
        picture[at] =
            (uint8_t)((uint32_t)picture[at] & keep_high) | color;
    }
}

/* One bar: bar_w - 1 columns from x0, height rows from the bottom. */
static void fill_bar(uint8_t *picture, int32_t x0, int32_t bar_w,
                     int32_t height, uint8_t color)
{
    int32_t x;

    for (x = x0; x < ((x0 + bar_w) - 1); x++) {
        int32_t y;

        for (y = MATRIX_H - height; y < MATRIX_H; y++) {
            put_pixel(picture, x, y, color);
        }
    }
}

/* A bar per class on the 16 x 8 matrix (a still picture, sent again
 * without a flicker); a column is left between the bars. */
static void draw_scores(const frame_t *frame)
{
    uint8_t picture[BENTO_RGBMATRIX_FRAME_LEN];
    const int32_t bars =
        (frame->count > 1U) ? (int32_t)frame->count : 1;
    const int32_t bar_w = MATRIX_W / bars;
    uint32_t i;

    (void)memset(picture, 0, sizeof(picture));
    for (i = 0U; i < frame->count; i++) {
        uint8_t color = (uint8_t)BENTO_RGBMATRIX_BLUE;

        if (i == frame->top) {
            color = frame->alerting ? (uint8_t)BENTO_RGBMATRIX_RED
                                    : (uint8_t)BENTO_RGBMATRIX_GREEN;
        }
        fill_bar(picture, (int32_t)i * bar_w, bar_w, frame->heights[i],
                 color);
    }
    /* the matrix did not answer in time: this picture is skipped */
    (void)bento_rgbmatrix_blit(picture, BENTO_RGBMATRIX_FRAME_LEN);
}

static bool same_frame(const frame_t *left, const frame_t *right)
{
    bool same = left->has && right->has &&
                (left->count == right->count) &&
                (left->top == right->top) &&
                (left->alerting == right->alerting);
    uint32_t i;

    for (i = 0U; same && (i < left->count); i++) {
        same = left->heights[i] == right->heights[i];
    }
    return same;
}

/* Sent when the picture changes, and again every MATRIX_MS: a picture
 * can be lost while the board is busy. */
static void matrix_update(watch_t *watch)
{
    const uint32_t now_ms = bento_time_ticks_ms();

    if (watch->frame.has &&
        (!same_frame(&watch->frame, &watch->drawn) ||
         (bento_time_ticks_diff(now_ms, watch->sent_ms) >=
          MATRIX_MS))) {
        draw_scores(&watch->frame);
        watch->drawn = watch->frame;
        watch->sent_ms = now_ms;
    }
}

/* ---- 3) The brain ---- */

static bool is_danger(const char *label, int32_t conf)
{
    static const char *const danger[] = {
        "shaking", "anomaly",  "cough",    "alarm",         "sirens",
        "Push",    "baby_cry", "chainsaw", "crackling_fire"};
    const uint32_t count =
        (uint32_t)(sizeof(danger) / sizeof(danger[0]));
    bool found = false;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        found = found || (strcmp(label, danger[i]) == 0);
    }
    return found && (conf >= CONF_MIN);
}

/* The last WINDOW_N results (1 = danger). The AI flips between
 * anomaly and unlabeled while the machine shakes, so it counts "in
 * the window", not "in a row"; the alarm ends only when the whole
 * window is safe (no blinking alarm light). */
static void alert_rule(watch_t *watch, bool danger)
{
    uint32_t hits = 0U;
    uint32_t i;

    if (watch->history_count == (uint32_t)WINDOW_N) {
        for (i = 1U; i < (uint32_t)WINDOW_N; i++) {
            watch->history[i - 1U] = watch->history[i];
        }
        watch->history_count--;
    }
    watch->history[watch->history_count] = danger ? 1U : 0U;
    watch->history_count++;
    for (i = 0U; i < watch->history_count; i++) {
        hits += watch->history[i];
    }
    watch->alerting = (hits >= (uint32_t)CONFIRM_N) ||
                      (watch->alerting && (hits > 0U));
}

static uint32_t history_hits(const watch_t *watch)
{
    uint32_t hits = 0U;
    uint32_t i;

    for (i = 0U; i < watch->history_count; i++) {
        hits += watch->history[i];
    }
    return hits;
}

/* The untaken class with the highest score, the lower class on a
 * tie. */
static uint32_t best_untaken(const bento_edgeai_result_t *result,
                             const bool *taken, uint32_t count)
{
    uint32_t best = count;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        if (!taken[i] &&
            ((best == count) ||
             (result->score_pct[i] > result->score_pct[best]))) {
            best = i;
        }
    }
    return best;
}

/* The classes with the ROWS highest scores, highest first; a tie
 * keeps the lower class first, as a stable sort. */
static uint32_t top_rows(const bento_edgeai_result_t *result,
                         uint32_t *rows)
{
    const uint32_t count = (result->class_count < (uint32_t)MAX_CLASSES)
                               ? result->class_count
                               : (uint32_t)MAX_CLASSES;
    bool taken[MAX_CLASSES];
    uint32_t filled = 0U;

    (void)memset(taken, 0, sizeof(taken));
    while ((filled < (uint32_t)ROWS) && (filled < count)) {
        const uint32_t best = best_untaken(result, taken, count);

        taken[best] = true;
        rows[filled] = best;
        filled++;
    }
    return filled;
}

/* int(pct / 100 * 100) in the board's float, as the .py works it. */
static int32_t percent_of(uint8_t pct)
{
    const float hundred = 100.0F;
    const float score = (float)pct / hundred;
    const bento_real_t scaled =
        (bento_real_t)score * (bento_real_t)PERCENT;

    return (int32_t)scaled;
}

/* The matrix picture: a bar of 0-8 rows a class (round(score * 8)),
 * the winner, and the alarm. */
static void score_frame(watch_t *watch,
                        const bento_edgeai_result_t *result)
{
    const float hundred = 100.0F;
    frame_t *frame = &watch->frame;
    uint32_t i;

    frame->has = true;
    frame->count = (result->class_count < (uint32_t)MAX_CLASSES)
                       ? result->class_count
                       : (uint32_t)MAX_CLASSES;
    for (i = 0U; i < frame->count; i++) {
        const float score = (float)result->score_pct[i] / hundred;
        const bento_real_t scaled =
            (bento_real_t)score * (bento_real_t)MATRIX_H;
        const int32_t height = (int32_t)nearbyint((double)scaled);

        frame->heights[i] = (height < MATRIX_H) ? height : MATRIX_H;
    }
    frame->top = result->top;
    frame->alerting = watch->alerting;
}

/* ---- 4) The screen ---- */

static bento_status_t add_part(doctor_t *doctor, const part_t *part)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    if (part->text != NULL) {
        args.text = part->text;
        args.text_len = (uint32_t)strlen(part->text);
    }
    args.x = part->x;
    args.y = part->y;
    args.w = part->w;
    args.h = part->h;
    args.color = part->color;
    args.min_val = part->minimum;
    args.max_val = part->maximum;
    args.font_px = part->value;
    status = bento_ui_create(doctor->ui, part->kind, &args,
                             &doctor->ids[part->slot]);
    if ((status == BENTO_OK) && (part->then_color != 0U)) {
        show_color(doctor, part->slot, part->then_color);
    }
    return status;
}

static bento_status_t add_parts(doctor_t *doctor, const part_t *parts,
                                uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_part(doctor, &parts[i]);
    }
    return status;
}

/* The title, the model line, and the two upper cards: the answer and
 * every class's score. */
static bento_status_t add_upper(doctor_t *doctor)
{
    static const part_t parts[] = {
        {UI_WIDGET_LABEL, SLOT_NONE, "AI หมอเครื่องจักร (Edge AI)", 12, 6,
         -1, -1, COLOR_TEXT, 0, 0, 24, 0U},
        {UI_WIDGET_LABEL, SLOT_MODEL, "โมเดล: -", 12, 38, -1, -1,
         COLOR_DIM, 0, 0, 20, 0U},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 12, 64, 380, 172, COLOR_CARD,
         COLOR_DIM, CARD_RADIUS, 1, 0U},
        {UI_WIDGET_LABEL, SLOT_NONE, "AI ตอบว่า", 24, 70, -1, -1,
         COLOR_INFO, 0, 0, 20, 0U},
        {UI_WIDGET_ARC, SLOT_ARC, NULL, 24, 98, 132, 132, 0U, 0, 100, 0,
         COLOR_DIM},
        {UI_WIDGET_LABEL, SLOT_VERDICT, "-", 170, 94, -1, -1,
         COLOR_TEXT, 0, 0, 28, 0U},
        {UI_WIDGET_LED, SLOT_LED, NULL, 340, 96, 36, 36, COLOR_BAD, 0,
         0, 0, 0U},
        {UI_WIDGET_LABEL, SLOT_MEANING, "-", 170, 140, -1, -1,
         COLOR_INFO, 0, 0, 20, 0U},
        {UI_WIDGET_LABEL, SLOT_CONF, "มั่นใจ - %", 170, 176, -1, -1,
         COLOR_DIM, 0, 0, 20, 0U},
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 402, 64, 378, 172,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1, 0U},
        {UI_WIDGET_LABEL, SLOT_NONE, "คะแนนทุกคลาส (%)", 414, 70, -1, -1,
         COLOR_INFO, 0, 0, 20, 0U},
        {UI_WIDGET_LABEL, SLOT_NAME_0, " ", 414, 102, -1, -1,
         COLOR_TEXT, 0, 0, 16, 0U},
        {UI_WIDGET_BAR, SLOT_BAR_0, NULL, 512, 104, 256, 18, 0U, 0, 100,
         0, COLOR_DIM},
        {UI_WIDGET_LABEL, SLOT_NAME_1, " ", 414, 136, -1, -1,
         COLOR_TEXT, 0, 0, 16, 0U},
        {UI_WIDGET_BAR, SLOT_BAR_1, NULL, 512, 138, 256, 18, 0U, 0, 100,
         0, COLOR_DIM},
        {UI_WIDGET_LABEL, SLOT_NAME_2, " ", 414, 170, -1, -1,
         COLOR_TEXT, 0, 0, 16, 0U},
        {UI_WIDGET_BAR, SLOT_BAR_2, NULL, 512, 172, 256, 18, 0U, 0, 100,
         0, COLOR_DIM},
        {UI_WIDGET_LABEL, SLOT_NAME_3, " ", 414, 204, -1, -1,
         COLOR_TEXT, 0, 0, 16, 0U},
        {UI_WIDGET_BAR, SLOT_BAR_3, NULL, 512, 206, 256, 18, 0U, 0, 100,
         0, COLOR_DIM},
        {UI_WIDGET_LABEL, SLOT_NONE, "ฟ้า = มั่นใจ %   แดง = เกณฑ์", 12,
         242, -1, -1, COLOR_DIM, 0, 0, 20, 0U}};

    return add_parts(doctor, parts,
                     (uint32_t)(sizeof(parts) / sizeof(parts[0])));
}

/* The chart of the confidence against the limit: a smooth line, no
 * round dots (at most 400 wide, 400 points). */
static bento_status_t add_chart(doctor_t *doctor)
{
    static const part_t chart = {
        UI_WIDGET_CHART, SLOT_CHART, NULL, 12, 270, 400, 76,
        COLOR_INFO,      0,          100,  0,  0U};
    bento_status_t status = add_part(doctor, &chart);

    if (status == BENTO_OK) {
        /* .prop() never raises in MicroPython */
        (void)bento_ui_set_prop(doctor->ui, doctor->ids[SLOT_CHART],
                                (uint8_t)UI_PROP_CHART_POINTS,
                                CHART_POINTS);
        status = bento_ui_chart_add_series(
            doctor->ui, doctor->ids[SLOT_CHART], COLOR_BAD,
            &doctor->threshold_series);
    }
    return status;
}

/* The "AI's brain" card and the status line. */
static bento_status_t add_lower(doctor_t *doctor)
{
    static const part_t parts[] = {
        {UI_WIDGET_PANEL, SLOT_NONE, NULL, 422, 244, 358, 94,
         COLOR_CARD, COLOR_DIM, CARD_RADIUS, 1, 0U},
        {UI_WIDGET_LABEL, SLOT_NONE, "สมองของ AI", 434, 250, -1, -1,
         COLOR_INFO, 0, 0, 20, 0U},
        {UI_WIDGET_LABEL, SLOT_LATENCY, "ใช้เวลาคิด - ms", 434, 278, -1,
         -1, COLOR_DIM, 0, 0, 20, 0U},
        {UI_WIDGET_LABEL, SLOT_STREAK, " ", 434, 308, -1, -1, COLOR_DIM,
         0, 0, 20, 0U},
        {UI_WIDGET_LABEL, SLOT_STATUS, "กำลังหาโมเดล...", 12, 352, -1,
         -1, COLOR_WARN, 0, 0, 20, 0U}};

    return add_parts(doctor, parts,
                     (uint32_t)(sizeof(parts) / sizeof(parts[0])));
}

static bento_status_t build_screen(doctor_t *doctor)
{
    bento_status_t status = bento_ui_screen_default(doctor->ui);

    if (status == BENTO_OK) {
        status = wait_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_upper(doctor);
    }
    if (status == BENTO_OK) {
        status = add_chart(doctor);
    }
    if (status == BENTO_OK) {
        status = add_lower(doctor);
    }
    if (status == BENTO_OK) {
        poll_display(doctor->ui);
    }
    return status;
}

static void say(const doctor_t *doctor, const char *text,
                uint32_t color)
{
    show_color(doctor, SLOT_STATUS, color);
    show_text(doctor, SLOT_STATUS, text);
}

/* The model's name, and its labels on the four rows. */
static void show_model(doctor_t *doctor, const found_t *found, bool mic)
{
    static const uint32_t names[ROWS] = {SLOT_NAME_0, SLOT_NAME_1,
                                         SLOT_NAME_2, SLOT_NAME_3};
    char text[TEXT_BYTES];
    uint32_t length = 0U;
    uint32_t i;

    (void)snprintf(text, sizeof(text), "โมเดล: %s%s", found->name,
                   mic ? "  (ฟังไมค์)" : "");
    show_text(doctor, SLOT_MODEL, text);
    doctor->label_count = (found->class_count < (uint32_t)MAX_CLASSES)
                              ? found->class_count
                              : (uint32_t)MAX_CLASSES;
    for (i = 0U; i < doctor->label_count; i++) {
        if (bento_edgeai_label(found->index, i, doctor->labels[i],
                               BENTO_EDGEAI_LABEL_LEN,
                               &length) != BENTO_OK) {
            doctor->labels[i][0] = '\0';
        }
    }
    for (i = 0U; i < (uint32_t)ROWS; i++) {
        show_text(doctor, names[i],
                  (i < doctor->label_count) ? doctor->labels[i] : " ");
    }
}

/* The model's class name in farm words (it must match the board's). */
static const char *meaning_of(const char *label)
{
    static const char *const meanings[][2] = {
        {"idle", "เครื่องหยุด / นิ่ง"},     {"circle", "หมุนเป็นจังหวะ (ปกติ)"},
        {"shaking", "สั่นผิดปกติ!"},       {"unlabelled", "ไม่มีเหตุการณ์"},
        {"anomaly", "สั่นผิดปกติ!"},       {"unlabeled", "ปกติ"},
        {"cough", "ได้ยินเสียงไอ"},       {"alarm", "เสียงสัญญาณเตือน"},
        {"sirens", "เสียงไซเรน"},       {"Push", "มีคนผลักเข้ามา"},
        {"baby_cry", "เสียงร้องแบบเด็ก"}, {"chainsaw", "เลื่อยยนต์!"},
        {"crackling_fire", "ไฟไหม้!"}};
    const uint32_t count =
        (uint32_t)(sizeof(meanings) / sizeof(meanings[0]));
    const char *meaning = label;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        if (strcmp(label, meanings[i][0]) == 0) {
            meaning = meanings[i][1];
        }
    }
    return meaning;
}

/* The label the result names, "" when it has none. */
static const char *label_of(const doctor_t *doctor, uint32_t klass)
{
    return (klass < doctor->label_count) ? doctor->labels[klass] : "";
}

/* One score row: the class's name (its number when it has none),
 * its score, green or red for the winner. */
static void show_row(const doctor_t *doctor,
                     const bento_edgeai_result_t *result, uint32_t row,
                     uint32_t klass)
{
    static const uint32_t names[ROWS] = {SLOT_NAME_0, SLOT_NAME_1,
                                         SLOT_NAME_2, SLOT_NAME_3};
    static const uint32_t bars[ROWS] = {SLOT_BAR_0, SLOT_BAR_1,
                                        SLOT_BAR_2, SLOT_BAR_3};
    const uint32_t hot = doctor->watch.alerting ? (uint32_t)COLOR_BAD
                                                : (uint32_t)COLOR_OK;
    char text[TEXT_BYTES];

    if (klass < doctor->label_count) {
        show_text(doctor, names[row], doctor->labels[klass]);
    } else {
        (void)snprintf(text, sizeof(text), "%" PRIu32, klass);
        show_text(doctor, names[row], text);
    }
    show_value(doctor, bars[row], percent_of(result->score_pct[klass]));
    show_color(doctor, bars[row],
               (klass == result->top) ? hot : (uint32_t)COLOR_DIM);
}

/* More than 4 classes: the top 4, the names follow the scores. */
static void show_rows(const doctor_t *doctor,
                      const bento_edgeai_result_t *result)
{
    static const uint32_t names[ROWS] = {SLOT_NAME_0, SLOT_NAME_1,
                                         SLOT_NAME_2, SLOT_NAME_3};
    static const uint32_t bars[ROWS] = {SLOT_BAR_0, SLOT_BAR_1,
                                        SLOT_BAR_2, SLOT_BAR_3};
    uint32_t rows[ROWS] = {0U, 0U, 0U, 0U};
    const uint32_t filled = top_rows(result, rows);
    uint32_t row;

    for (row = 0U; row < (uint32_t)ROWS; row++) {
        if (row < filled) {
            show_row(doctor, result, row, rows[row]);
        } else {
            show_text(doctor, names[row], " ");
            show_value(doctor, bars[row], 0);
        }
    }
}

/* The "AI's brain" card and the chart. */
static void show_brain(const doctor_t *doctor,
                       const bento_edgeai_result_t *result,
                       int32_t conf)
{
    const watch_t *watch = &doctor->watch;
    const float ms_per_us = (float)MS_PER_US;
    const float latency_ms = (float)result->latency_us / ms_per_us;
    char number[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];
    uint32_t length = 0U;

    if (bento_fmt_real_precision(number, sizeof(number),
                                 (bento_real_t)latency_ms, 1,
                                 &length) != BENTO_OK) {
        number[0] = '\0'; /* BENTO_FMT_BUF_SIZE always holds it */
    }
    (void)snprintf(text, sizeof(text), "ใช้เวลาคิด %s ms", number);
    show_text(doctor, SLOT_LATENCY, text);
    /* 2/5 = danger in 2 of the last 5 results */
    (void)snprintf(
        text, sizeof(text),
        "อันตราย %" PRIu32 "/%" PRIu32 "  เตือนแล้ว %" PRId32 " ครั้ง",
        history_hits(watch), watch->history_count, watch->alerts);
    show_text(doctor, SLOT_STREAK, text);
    (void)bento_ui_chart_set_next(doctor->ui, doctor->ids[SLOT_CHART],
                                  0U, conf);
    (void)bento_ui_chart_set_next(doctor->ui, doctor->ids[SLOT_CHART],
                                  doctor->threshold_series, CONF_MIN);
}

static void show_result(const doctor_t *doctor,
                        const bento_edgeai_result_t *result,
                        int32_t conf)
{
    const bool alerting = doctor->watch.alerting;
    const char *label = label_of(doctor, result->top);
    char text[TEXT_BYTES];

    if (label[0] == '\0') {
        label = "-";
    }
    show_text(doctor, SLOT_VERDICT, label);
    show_text(doctor, SLOT_MEANING, meaning_of(label));
    show_color(doctor, SLOT_MEANING, alerting ? COLOR_BAD : COLOR_INFO);
    show_value(doctor, SLOT_ARC, conf);
    show_color(doctor, SLOT_ARC,
               (conf >= CONF_MIN) ? COLOR_OK : COLOR_WARN);
    (void)snprintf(text, sizeof(text), "มั่นใจ %" PRId32 " %%", conf);
    show_text(doctor, SLOT_CONF, text);
    show_value(doctor, SLOT_LED, alerting ? 1 : 0);
    show_rows(doctor, result);
    show_brain(doctor, result, conf);
}

/* A tone only when the alarm starts or ends, then the time results
 * are trusted again: a mic model hears the board's own speaker. */
static bento_status_t announce(doctor_t *doctor)
{
    watch_t *watch = &doctor->watch;
    const bento_status_t status =
        watch->alerting
            ? beep(doctor, (uint8_t)NOTE_HIGH, (uint8_t)NOTE_ALARM_LOW)
            : beep(doctor, (uint8_t)NOTE_CLEAR_LOW, (uint8_t)NOTE_HIGH);

    watch->quiet_ms = bento_time_ticks_add(bento_time_ticks_ms(),
                                           watch->mic ? MUTE_MS : 0);
    return status;
}

/* ---- 5) The main program ---- */

/* A new answer, unless the board has just made a sound: judge, act
 * and show. */
static bento_status_t take_result(doctor_t *doctor,
                                  const bento_edgeai_result_t *result,
                                  uint32_t now_ms)
{
    watch_t *watch = &doctor->watch;
    const bool was = watch->alerting;
    const int32_t conf = percent_of(result->score_pct[result->top]);
    bento_status_t status = BENTO_OK;

    if (bento_time_ticks_diff(now_ms, watch->quiet_ms) >= 0) {
        alert_rule(watch,
                   is_danger(label_of(doctor, result->top), conf));
        if (watch->alerting != was) {
            status = announce(doctor);
        }
        if ((status == BENTO_OK) && (watch->alerting != was)) {
            status = set_led(doctor, watch->alerting);
        }
        if ((status == BENTO_OK) && watch->alerting && !was) {
            watch->alerts++;
            (void)printf("AI เตือน: %s มั่นใจ %" PRId32 " %%\n",
                         label_of(doctor, result->top), conf);
        }
        show_result(doctor, result, conf);
        score_frame(watch, result);
    }
    return status;
}

/* One pass every TICK_MS: read, judge, act, show. */
static bento_status_t watch_once(doctor_t *doctor)
{
    watch_t *watch = &doctor->watch;
    const uint32_t now_ms = bento_time_ticks_ms();
    bento_edgeai_result_t result;
    bento_status_t status = BENTO_OK;

    bento_edgeai_result_init(&result);
    /* edge_ai.result() raising OSError is the .py's None */
    if ((bento_edgeai_result(&result) == BENTO_OK) &&
        result.has_verdict &&
        (!watch->has_sequence || (result.seq != watch->sequence))) {
        watch->has_sequence = true;
        watch->sequence = result.seq;
        watch->last_new_ms = now_ms;
        if (watch->stalled) {
            watch->stalled = false;
            say(doctor, "AI กำลังคิด...", COLOR_OK);
        }
        status = take_result(doctor, &result, now_ms);
    } else if (!watch->stalled &&
               (bento_time_ticks_diff(now_ms, watch->last_new_ms) >
                STALL_MS)) {
        watch->stalled = true;
        say(doctor, "ผลไม่ขยับ ลองรีเซ็ตบอร์ด", COLOR_BAD);
    } else {
        /* the same answer as before */
    }
    if (status == BENTO_OK) {
        matrix_update(watch);
        poll_display(doctor->ui);
        status = wait_ms((uint32_t)TICK_MS);
    }
    return status;
}

/* Reads the AI's answers until RUN_MS. */
static bento_status_t watch_run(doctor_t *doctor, bool mic)
{
    watch_t *watch = &doctor->watch;
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    (void)memset(watch, 0, sizeof(*watch));
    watch->mic = mic;
    watch->last_new_ms = start_ms;
    watch->quiet_ms = start_ms;
    watch->sent_ms = start_ms;
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = watch_once(doctor);
    }
    return status;
}

/* edge_ai.select(): a model that does not start lets the next one
 * try. Only Stop is an error. */
static bento_status_t start_model(const doctor_t *doctor,
                                  uint32_t index, bool *started)
{
    bento_status_t status = bento_edgeai_select_start(index);

    if (status == BENTO_OK) {
        status = finish_op();
    }
    *started = status == BENTO_OK;
    if (*started) {
        say(doctor, "AI กำลังคิด...", COLOR_OK);
    } else if (status != BENTO_E_CANCELLED) {
        say(doctor, "โหลดโมเดลไม่สำเร็จ", COLOR_BAD);
        (void)printf("select: %d\n", (int)status);
    } else {
        /* Stop ends the run */
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* Each model found in turn (the built-in Motion when the Store's will
 * not start), until one runs. *watched: one ran to RUN_MS. */
static bento_status_t try_models(doctor_t *doctor, bool *watched)
{
    bool started = false;
    bento_status_t status = BENTO_OK;
    uint32_t i = 0U;

    while ((i < doctor->found_count) && (status == BENTO_OK) &&
           !started) {
        const found_t *found = &doctor->found[i];
        const bool mic = uses_mic(found->index);

        show_model(doctor, found, mic);
        status = start_model(doctor, found->index, &started);
        if ((status == BENTO_OK) && started) {
            status = watch_run(doctor, mic);
        }
        i++;
    }
    *watched = started && (status == BENTO_OK);
    return status;
}

/* Stopped halfway or not: the AI core is released and the light goes
 * off. */
static bento_status_t finish(const doctor_t *doctor,
                             bento_status_t run_status)
{
    bento_status_t status;

    stop_ai();
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
    status = set_led(doctor, false);
    (void)bento_rgbmatrix_clear(); /* an OSError is ignored there */
    return (run_status != BENTO_OK) ? run_status : status;
}

/* The keys joined by " / ". */
static void join_keys(char *text, uint32_t capacity)
{
    const uint32_t keys =
        (uint32_t)(sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]));
    size_t used = 0U;
    uint32_t i;

    text[0] = '\0';
    for (i = 0U; (i < keys) && (used < (size_t)capacity); i++) {
        (void)snprintf(&text[used], (size_t)capacity - used, "%s%s",
                       (i == 0U) ? "" : " / ", MODEL_KEYS[i]);
        used = strlen(text);
    }
}

static void report_none(const doctor_t *doctor)
{
    char keys[TEXT_BYTES];
    char text[TEXT_BYTES];

    join_keys(keys, sizeof(keys));
    (void)snprintf(text, sizeof(text), "ไม่พบโมเดล %s บนบอร์ดนี้", keys);
    say(doctor, text, COLOR_BAD);
    poll_display(doctor->ui);
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
    /* sf3_03 is the hand-written rule: no model needed */
    (void)printf("%s - ใช้ sf3_03 แทน\n", text);
}

/* No Edge AI from C (BENTO_E_UNSUPPORTED, as in the BENTO Emulator):
 * one line under the title says so, then the .py's own "no model"
 * path follows. */
static bento_status_t show_emulator_gap(doctor_t *doctor)
{
    static const part_t gap = {UI_WIDGET_LABEL,
                               SLOT_NONE,
                               "Edge AI จาก C ยังไม่มีใน BENTO "
                               "Emulator ให้รันบน Dev Kit หรือ Eva Kit",
                               180,
                               38,
                               -1,
                               -1,
                               COLOR_WARN,
                               0,
                               0,
                               20,
                               0U};
    const bento_status_t status = add_part(doctor, &gap);

    if (status == BENTO_OK) {
        poll_display(doctor->ui);
        (void)printf(
            "%s / Edge AI from C is not in the BENTO Emulator yet: "
            "run it on a Dev Kit or Eva Kit.\n",
            gap.text);
    }
    return status;
}

static bento_status_t run_doctor(doctor_t *doctor)
{
    bool watched = false;
    bento_status_t status;

    find_led(doctor);
    status = finish(doctor, try_models(doctor, &watched));
    if ((status == BENTO_OK) && watched) {
        say(doctor, "ครบเวลา - กด Program to Device เพื่อเล่นใหม่",
            COLOR_WARN);
        (void)printf("AI เฝ้าเครื่องครบเวลา เตือน %" PRId32 " ครั้ง\n",
                     doctor->watch.alerts);
    }
    if (status == BENTO_OK) {
        poll_display(doctor->ui);
    }
    return status;
}

int main(void)
{
    doctor_t *doctor = &g_doctor;
    bento_status_t status;

    (void)memset(doctor, 0, sizeof(*doctor));
    doctor->ui = bento_runtime_get_ui();
    status = bento_ui_sfx_volume_set(doctor->ui, SPEAKER);
    if (status == BENTO_E_UNSUPPORTED) {
        status = BENTO_OK; /* firmware 2.4.1 skips the volume line */
    }
    if (status == BENTO_OK) {
        status = build_screen(doctor);
    }
    if (status == BENTO_OK) {
        bool linked = true;

        find_models(doctor, &linked);
        if (!linked) {
            status = show_emulator_gap(doctor);
        }
        if ((status == BENTO_OK) && (doctor->found_count == 0U)) {
            report_none(doctor);
        } else if (status == BENTO_OK) {
            status = run_doctor(doctor);
        } else {
            /* the gap line could not be drawn: the run ends */
        }
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
