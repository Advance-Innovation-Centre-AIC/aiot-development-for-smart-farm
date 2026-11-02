/*
 * ai_1_pump_anomaly.c -- ปั๊มสั่นผิดปกติ: กฎที่เขียนเอง กับ AI ตัดสินคู่กัน
 * แล้วส่งผลขึ้น MQTT
 *
 * วางบอร์ดบนตัวปั๊ม ให้สองฝ่ายตัดสินพร้อมกัน กฎ = ค่าสั่นจาก IMU
 * เกินค่าปกติ K_VIB เท่า (เรียนค่าปกติ 5 วินาทีแรก วางนิ่ง) AI =
 * โมเดล AnomalousVibration (ถ้าไม่มีใช้ Motion ในตัว) ป้าย anomaly
 * หรือ shaking มั่นใจถึง CONF_MIN % อย่างน้อย 3 ใน 5 ผลล่าสุด
 * ทั้งสองเห็นตรงกัน = เตือนจริง (ไฟแดง ตัววิ่ง และเสียง) ฝ่ายเดียว =
 * ให้คนไปดู ผลส่งขึ้น bento-aiot/<TEAM>/ai เมื่อ label / lvl / alarm
 * เปลี่ยน และทุก 2 วินาที แก้ WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน
 * (ยังไม่แก้ = ทำงานออฟไลน์ พิมพ์ผลลง Console แทน) ผล AI ใน BENTO
 * Emulator เป็นผลจำลอง ต้องทดสอบบนบอร์ดจริง
 *
 * Put the board on the pump and let two judges decide together. The
 * rule: the IMU's vibration is K_VIB times its normal level (learnt
 * in the first 5 seconds, lying still). The AI: the
 * AnomalousVibration model (else the built-in Motion one) says
 * anomaly or shaking with at least CONF_MIN % in 3 of the last 5
 * results. Both agree = a real alarm (red LED, the matrix and a
 * tone); one side only = send someone to look. The verdict goes to
 * bento-aiot/<TEAM>/ai when label, lvl or alarm change, and every 2
 * seconds. Set WIFI_SSID, WIFI_PASS and TEAM first; unset, it runs
 * offline and prints every message on the console. In the BENTO
 * Emulator the AI result is simulated: test on a real board.
 *
 * Twin of ai_1_pump_anomaly.py
 * (aiot-development-for-smart-farm/project/ai_future/ai_1_pump_anomaly.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - json.dumps becomes snprintf into a buffer sized for every
 *     field; the keys are written in the .py's order (MicroPython's
 *     dict may print them in another order). Model names and labels
 *     are written as the board gives them: they hold no quote.
 *   - edge_ai.models() reads the whole list or raises; here the rows
 *     are read one by one and a row that cannot be read is skipped.
 *   - edge_ai.select(), wifi.connect() and mqtt.connect() wait inside
 *     the binding; here they are started and polled, so Stop ends a
 *     wait at once.
 *   - A reconnect closes the lost MQTT client before it opens a new
 *     one, and the program also closes the module's Edge AI link
 *     (bento_edgeai_deinit) after the final stop.
 *   - A board without the model link (reTerminal) answers
 *     BENTO_E_UNSUPPORTED to bento_edgeai_init(): the same "no model"
 *     path as an empty edge_ai.models().
 *   - In the BENTO Emulator, Edge AI/MQTT from C is not available
 *     yet: the program says so and ends. The line goes up beside the
 *     .py's own "no model" note, which then follows as before.
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
#include "bento_mqtt.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
/* Edit before running. Unset, the program runs offline and prints
 * every message on the console. */
/* make it up yourself: short English letters or digits, no spaces */
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
/* at least 8 characters; never send a file with the real one */
static const char WIFI_PASS[] = "<รหัส Hotspot ของกลุ่ม>";
/* the number the teacher hands out, such as team05 (it must match
 * the app) */
static const char TEAM[] = "teamXX";
static const char BROKER[] = "broker.hivemq.com";
/* CLIENT_ID = bento-ai-TEAM, built in open_broker() */
/* TOPIC = bento-aiot/TEAM/ai, built in run_with_model() */
/* the model from the Store first, else the built-in one */
static const char *const MODEL_KEYS[] = {"AnomalousVibration",
                                         "Motion"};
/* the labels that count as trouble (the built-in Motion says
 * shaking) */
static const char *const DANGER[] = {"anomaly", "shaking"};
enum {
    CONF_MIN = 60, /* below this many % the AI is not counted */
    CONFIRM_N = 3,
    WINDOW_N = 5,
    SAMPLE_MS = 50, /* read the IMU and ask the AI this often */
    WIN = 20,       /* the vibration is worked out over this many */
    LEARN_MS = 5000 /* learn the normal level this long (lie still) */
};
/* the lowest normal level, m/s^2 */
static const bento_real_t MIN_BASE = (bento_real_t)0.05;
/* this many times normal = the rule says trouble */
static const bento_real_t K_VIB = (bento_real_t)3.0;
enum {
    HEARTBEAT_MS = 2000,
    GAP_MS = 200,
    RETRY_MS = 10000, /* a lost link is tried again this often */
    DRAW_MS = 300,
    RUN_MS = 300000,
    VOLUME = 25,
    SPEAKER = 40
};
/* ---- end of settings ---- */

/* The course palette, 0xRRGGBB. */
enum {
    COLOR_TEXT = 0xE8EAED,
    COLOR_DIM = 0x9AA3AF,
    COLOR_OK = 0x30A46C,
    COLOR_WARN = 0xF5A623,
    COLOR_BAD = 0xE5484D,
    COLOR_INFO = 0x4A9EFF
};

/* The link, the tones and the sizes of what is built. */
enum {
    PORT = 1883,
    KEEPALIVE_S = 30,
    CLIENT_SUFFIX_MASK = 0xFFFF, /* four hex digits of the clock */
    WIFI_SLICE_MS = 50,
    MQTT_SLICE_MS = 20,
    OP_POLL_MS = 20,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 120,
    MATRIX_MS = 80,
    NOTE_HIGH = 84, /* the alarm falls 84 -> 76, the all-clear rises */
    NOTE_ALARM_LOW = 76,
    NOTE_CLEAR_LOW = 79,
    ALARM_LEVEL = 2,
    PERCENT = 100,
    MS_PER_US = 1000,
    NAME_BYTES = 48,     /* the client id or the topic */
    TEXT_BYTES = 128,    /* a label's text: a SET_TEXT holds 126 */
    PAYLOAD_BYTES = 448, /* the JSON line with three 47-byte names */
    TEAM_CHARS = 6,      /* "team" and two digits */
    TEAM_PREFIX = 4
};

/* The widgets the program changes, in the order they are made. */
typedef enum {
    SLOT_MODEL = 0,
    SLOT_ARC,
    SLOT_AI,
    SLOT_SECOND,
    SLOT_HISTORY,
    SLOT_RULE,
    SLOT_VERDICT,
    SLOT_LED,
    SLOT_AGREE,
    SLOT_MQTT,
    SLOT_SENT,
    SLOT_NOTE,
    SLOT_COUNT,
    SLOT_NONE
} slot_t;

typedef enum {
    KIND_LABEL = 0,
    KIND_ARC,
    KIND_LED
} kind_t;

/* Who sees trouble: the order gives the level (0, 1 or 2). */
typedef enum {
    AGREE_NONE = 0,
    AGREE_AI,
    AGREE_RULE,
    AGREE_BOTH
} agree_t;

/* The model that runs, with its labels. */
typedef struct {
    uint32_t index;
    bool found;
    uint32_t class_count;
    char name[BENTO_EDGEAI_NAME_LEN];
    char labels[BENTO_EDGEAI_MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
} model_t;

/* The rule: the standard deviation of the last WIN magnitudes, and
 * the normal level learnt in the first LEARN_MS. */
typedef struct {
    bento_real_t samples[WIN];
    uint32_t count;
    bento_real_t vib;
    int32_t fails;
    bool has_base;
    bento_real_t base;
    bento_real_t total;
    int32_t learnt;
    uint32_t start_ms;
} vib_t;

/* The AI: the latest result and the window of trouble flags. */
typedef struct {
    bool has_sequence;
    uint32_t sequence;
    char label[BENTO_EDGEAI_LABEL_LEN];
    int32_t conf;
    char second[BENTO_EDGEAI_LABEL_LEN];
    int32_t second_conf;
    int32_t latency_ms;
    uint8_t history[WINDOW_N];
    uint32_t history_count;
    bool on;
} verdict_t;

/* The network and what has been sent. */
typedef struct {
    bool can_net;
    bool online;
    bool has_broker;
    uint32_t broker;
    char topic[NAME_BYTES];
    int32_t sent;
    int32_t delivered;
    bool has_key;
    char key_label[BENTO_EDGEAI_LABEL_LEN];
    int32_t key_level;
    bool key_alarm;
    uint32_t sent_ms;
    uint32_t retry_ms;
} link_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT];
    model_t model;
    vib_t vib;
    verdict_t ai;
    link_t link;
    bool alarm;
    bool has_led;
    uint32_t led;
    uint32_t draw_ms;
    char payload[PAYLOAD_BYTES];
} pump_t;

/* Too big for the 8 KB program stack: file scope, zeroed by the
 * loader and set again at the start of main() (CLEAN_C.md C9,
 * deviation BENTO-MISRA-E03/E04). */
extern pump_t g_pump;
pump_t g_pump;

/* ---- hardware ---- */

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

static void show_text(const pump_t *pump, slot_t slot, const char *text)
{
    /* MicroPython's widget setters ignore a refusal (modui.c) */
    (void)bento_ui_set_text(pump->ui, pump->ids[slot], text,
                            (uint32_t)strlen(text));
}

static void show_color(const pump_t *pump, slot_t slot, uint32_t color)
{
    (void)bento_ui_set_color(pump->ui, pump->ids[slot], color);
}

/* Two notes of TONE_MS each, as the .py's beep(). */
static bento_status_t beep(const pump_t *pump, uint8_t first,
                           uint8_t second)
{
    const uint8_t notes[] = {first, second};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < 2U) && (status == BENTO_OK); i++) {
        /* a tone's result is not read in MicroPython either */
        (void)bento_ui_sfx_tone(pump->ui, notes[i], BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = wait_ms(TONE_MS);
    }
    return status;
}

static void matrix_scroll(const char *text, int32_t color)
{
    /* the .py ignores an OSError from the matrix */
    (void)bento_rgbmatrix_scroll(text, (uint32_t)strlen(text), color,
                                 MATRIX_MS);
}

/* The LED named RGB_RED, else LED 0; none when the board cannot say
 * or has no LED, as the .py's None. */
static void find_led(pump_t *pump)
{
    bento_gpio_board_info_t info;
    const char *name = NULL;
    uint32_t i;

    bento_gpio_board_info_init(&info);
    pump->has_led =
        (bento_gpio_board_info(&info) == BENTO_OK) && (info.leds > 0U);
    pump->led = 0U;
    for (i = 0U; pump->has_led && (i < info.leds); i++) {
        if ((bento_gpio_led_name(i, &name) == BENTO_OK) &&
            (name != NULL) && (strcmp(name, "RGB_RED") == 0)) {
            pump->led = i;
        }
    }
}

static bento_status_t set_led(const pump_t *pump, bool on)
{
    bento_status_t status = BENTO_OK;

    if (pump->has_led) {
        status = on ? bento_gpio_led_on(pump->led)
                    : bento_gpio_led_off(pump->led);
    }
    return status;
}

/* The total acceleration of the three axes (m/s^2); false when the
 * IMU cannot be read, never a guessed value. */
static bool read_magnitude(bento_real_t *magnitude)
{
    bento_sensors_imu6_t imu;
    bool read = false;

    bento_sensors_imu6_init(&imu);
    if (bento_sensors_bmi270_motion(&imu) == BENTO_OK) {
        const bento_real_t ax = (bento_real_t)imu.ax;
        const bento_real_t ay = (bento_real_t)imu.ay;
        const bento_real_t az = (bento_real_t)imu.az;
        const bento_real_t sum = (ax * ax) + (ay * ay) + (az * az);

        *magnitude = (bento_real_t)sqrt((double)sum);
        read = true;
    }
    return read;
}

/* ---- Edge AI ---- */

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

static char lower_ascii(char letter)
{
    static const char upper_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char lower_letters[] = "abcdefghijklmnopqrstuvwxyz";
    char lower = letter;
    size_t i;

    for (i = 0U; upper_letters[i] != '\0'; i++) {
        if (letter == upper_letters[i]) {
            lower = lower_letters[i];
        }
    }
    return lower;
}

/* key.lower() in name.lower() */
static bool name_has(const char *name, const char *key)
{
    const size_t key_length = strlen(key);
    bool match = false;
    size_t start;

    for (start = 0U; (name[start] != '\0') && !match; start++) {
        size_t i = 0U;

        while ((i < key_length) && (name[start + i] != '\0') &&
               (lower_ascii(name[start + i]) == lower_ascii(key[i]))) {
            i++;
        }
        match = i == key_length;
    }
    return match;
}

static void read_labels(model_t *model, uint32_t class_count)
{
    uint32_t length = 0U;
    uint32_t i;

    model->class_count = (class_count > BENTO_EDGEAI_MAX_CLASSES)
                             ? BENTO_EDGEAI_MAX_CLASSES
                             : class_count;
    for (i = 0U; i < model->class_count; i++) {
        if (bento_edgeai_label(model->index, i, model->labels[i],
                               BENTO_EDGEAI_LABEL_LEN,
                               &length) != BENTO_OK) {
            model->labels[i][0] = '\0';
        }
    }
}

/* Starts row index; a select the engine refuses leaves the model
 * unfound so the next row is tried. Only Stop is an error. */
static bento_status_t start_model(model_t *model, uint32_t index,
                                  const bento_edgeai_model_info_t *info)
{
    bento_status_t status = bento_edgeai_select_start(index);

    if (status == BENTO_OK) {
        status = finish_op();
    }
    model->found = status == BENTO_OK;
    if (model->found) {
        model->index = index;
        (void)memcpy(model->name, info->name, sizeof(info->name));
        read_labels(model, info->class_count);
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* Tries every model whose name holds key; the first one that starts
 * is kept. */
static bento_status_t try_key(model_t *model, const char *key,
                              int32_t count)
{
    bento_edgeai_model_info_t info;
    bento_status_t status = BENTO_OK;
    int32_t i;

    for (i = 0; (i < count) && (status == BENTO_OK) && !model->found;
         i++) {
        bento_edgeai_model_info_init(&info);
        if ((bento_edgeai_model_info((uint32_t)i, &info) == BENTO_OK) &&
            name_has(info.name, key)) {
            status = start_model(model, (uint32_t)i, &info);
        }
    }
    return status;
}

/* By name, in MODEL_KEYS order: the order on the board can change
 * after a reboot. No link, or no list, means no model. */
static bento_status_t find_model(model_t *model, bool *no_link)
{
    const uint32_t keys =
        (uint32_t)(sizeof(MODEL_KEYS) / sizeof(MODEL_KEYS[0]));
    const bento_status_t link = bento_edgeai_init();
    bool has_link = false;
    int32_t count = 0;
    bento_status_t status = BENTO_OK;
    uint32_t i;

    /* a reTerminal answers init the same way but says "no link" to
     * the query; only the BENTO Emulator's C runtime refuses both */
    *no_link = false;
    if (link == BENTO_E_UNSUPPORTED) {
        *no_link =
            bento_edgeai_supported(&has_link) == BENTO_E_UNSUPPORTED;
    }
    if ((link != BENTO_OK) ||
        (bento_edgeai_count(&count) != BENTO_OK)) {
        count = 0;
    }
    for (i = 0U; (i < keys) && (status == BENTO_OK) && !model->found;
         i++) {
        status = try_key(model, MODEL_KEYS[i], count);
    }
    return status;
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

static const char *label_of(const model_t *model, uint32_t index)
{
    return (index < model->class_count) ? model->labels[index] : "";
}

/* The runner-up: the highest score after the winner, the lower
 * index first on a tie, as a stable sort. */
static void find_second(verdict_t *ai, const model_t *model,
                        const bento_edgeai_result_t *result)
{
    uint32_t best = 0U;
    uint32_t second = 1U;
    uint32_t i;

    if (result->score_pct[second] > result->score_pct[best]) {
        best = 1U;
        second = 0U;
    }
    for (i = 2U; i < result->class_count; i++) {
        if (result->score_pct[i] > result->score_pct[best]) {
            second = best;
            best = i;
        } else if (result->score_pct[i] > result->score_pct[second]) {
            second = i;
        } else {
            /* a lower score, or a tie the earlier index keeps */
        }
    }
    if (second < model->class_count) {
        (void)snprintf(ai->second, sizeof(ai->second), "%s",
                       model->labels[second]);
    } else {
        (void)snprintf(ai->second, sizeof(ai->second), "%" PRIu32,
                       second);
    }
    ai->second_conf = percent_of(result->score_pct[second]);
}

/* ---- the brain ---- */

static bento_real_t vib_limit(const vib_t *vib)
{
    return vib->has_base ? (vib->base * K_VIB) : (bento_real_t)0;
}

static bool vib_on(const vib_t *vib)
{
    return vib->has_base && (vib->fails < WIN) &&
           (vib->vib >= vib_limit(vib));
}

/* The spread of the window; the first LEARN_MS learn the normal
 * level. */
static void work_out_vib(vib_t *vib, uint32_t now_ms)
{
    const bento_real_t window = (bento_real_t)WIN;
    const bento_real_t min_base = MIN_BASE;
    bento_real_t sum = (bento_real_t)0;
    bento_real_t spread = (bento_real_t)0;
    bento_real_t average;
    bento_real_t mean_square;
    uint32_t i;

    for (i = 0U; i < (uint32_t)WIN; i++) {
        sum += vib->samples[i];
    }
    average = sum / window;
    for (i = 0U; i < (uint32_t)WIN; i++) {
        const bento_real_t away = vib->samples[i] - average;

        spread += away * away;
    }
    mean_square = spread / window;
    vib->vib = (bento_real_t)sqrt((double)mean_square);
    if (!vib->has_base) {
        vib->total += vib->vib;
        vib->learnt++;
        if (bento_time_ticks_diff(now_ms, vib->start_ms) >= LEARN_MS) {
            const bento_real_t mean =
                vib->total / (bento_real_t)vib->learnt;

            vib->base = (mean > min_base) ? mean : min_base;
            vib->has_base = true;
        }
    }
}

static void vib_push(vib_t *vib, bool read, bento_real_t magnitude,
                     uint32_t now_ms)
{
    if (!read) {
        vib->fails++;
    } else {
        vib->fails = 0;
        if (vib->count == (uint32_t)WIN) {
            (void)memmove(&vib->samples[0], &vib->samples[1],
                          sizeof(vib->samples) -
                              sizeof(vib->samples[0]));
            vib->count--;
        }
        vib->samples[vib->count] = magnitude;
        vib->count++;
        if (vib->count == (uint32_t)WIN) {
            work_out_vib(vib, now_ms);
        }
    }
}

/* As sf3_04: trouble from CONFIRM_N in the last WINDOW_N results; it
 * clears only when the whole window is safe. */
static void alert_rule(verdict_t *ai, bool danger)
{
    uint32_t hits = 0U;
    uint32_t i;

    if (ai->history_count == (uint32_t)WINDOW_N) {
        (void)memmove(&ai->history[0], &ai->history[1],
                      sizeof(ai->history) - sizeof(ai->history[0]));
        ai->history_count--;
    }
    ai->history[ai->history_count] = danger ? 1U : 0U;
    ai->history_count++;
    for (i = 0U; i < ai->history_count; i++) {
        hits += ai->history[i];
    }
    ai->on = (hits >= (uint32_t)CONFIRM_N) || (ai->on && (hits > 0U));
}

static uint32_t history_hits(const verdict_t *ai)
{
    uint32_t hits = 0U;
    uint32_t i;

    for (i = 0U; i < ai->history_count; i++) {
        hits += ai->history[i];
    }
    return hits;
}

static bool is_danger(const char *label)
{
    const uint32_t count =
        (uint32_t)(sizeof(DANGER) / sizeof(DANGER[0]));
    bool danger = false;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        danger = danger || (strcmp(label, DANGER[i]) == 0);
    }
    return danger;
}

static agree_t agreement(bool ai_on, bool rule_on)
{
    agree_t agree = AGREE_NONE;

    if (ai_on && rule_on) {
        agree = AGREE_BOTH;
    } else if (ai_on) {
        agree = AGREE_AI;
    } else if (rule_on) {
        agree = AGREE_RULE;
    } else {
        /* nobody sees trouble */
    }
    return agree;
}

static int32_t level_of(agree_t agree)
{
    int32_t level = 1;

    if (agree == AGREE_BOTH) {
        level = ALARM_LEVEL;
    } else if (agree == AGREE_NONE) {
        level = 0;
    } else {
        /* one side only */
    }
    return level;
}

static bool should_send(bool changed, int32_t since_ms)
{
    return (since_ms >= GAP_MS) &&
           (changed || (since_ms >= HEARTBEAT_MS));
}

/* A new result: its label, its confidence, the runner-up, the
 * latency, and the window of trouble flags. */
static void take_result(pump_t *pump,
                        const bento_edgeai_result_t *result)
{
    verdict_t *ai = &pump->ai;
    const float ms_per_us = (float)MS_PER_US;
    const float latency_ms = (float)result->latency_us / ms_per_us;
    const char *label = label_of(&pump->model, result->top);

    ai->has_sequence = true;
    ai->sequence = result->seq;
    (void)snprintf(ai->label, sizeof(ai->label), "%s",
                   (label[0] != '\0') ? label : "-");
    ai->conf = percent_of(result->score_pct[result->top]);
    if (result->class_count < 2U) {
        (void)snprintf(ai->second, sizeof(ai->second), "-");
        ai->second_conf = 0;
    } else {
        find_second(ai, &pump->model, result);
    }
    ai->latency_ms = (int32_t)latency_ms;
    alert_rule(ai, is_danger(ai->label) && (ai->conf >= CONF_MIN));
}

static void read_ai(pump_t *pump)
{
    bento_edgeai_result_t result;

    bento_edgeai_result_init(&result);
    /* edge_ai.result() raising OSError is the .py's None */
    if ((bento_edgeai_result(&result) == BENTO_OK) &&
        result.has_verdict &&
        (!pump->ai.has_sequence || (result.seq != pump->ai.sequence))) {
        take_result(pump, &result);
    }
}

/* ---- the network (sf3_06, short) ---- */

/* team01 style: "team", two digits, not team00. */
static bool team_ok(void)
{
    const char *digits = &TEAM[TEAM_PREFIX];
    bool good = (strlen(TEAM) == (size_t)TEAM_CHARS) &&
                (strncmp(TEAM, "team", (size_t)TEAM_PREFIX) == 0);

    good = good && (digits[0] >= '0') && (digits[0] <= '9') &&
           (digits[1] >= '0') && (digits[1] <= '9');
    return good && (strcmp(TEAM, "team00") != 0);
}

static bool ssid_unset(void)
{
    return (WIFI_SSID[0] == '<') || (WIFI_SSID[0] == '\0');
}

static bool wifi_connected(void)
{
    bool connected = false;

    if (bento_wifi_is_connected(&connected) != BENTO_OK) {
        connected = false;
    }
    return connected;
}

/* Asks the started join every WIFI_SLICE_MS until it ends. */
static bento_status_t wait_join(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = bento_wifi_connect_poll();
        if (status == BENTO_E_IN_PROGRESS) {
            status = wait_ms(WIFI_SLICE_MS);
            status =
                (status == BENTO_OK) ? BENTO_E_IN_PROGRESS : status;
        }
    }
    if (status == BENTO_E_CANCELLED) {
        (void)bento_wifi_connect_cancel(); /* answers BENTO_OK always */
    }
    return status;
}

/* Joined with an address; only Stop is an error. */
static bento_status_t wifi_up(bool *up)
{
    bento_wifi_connect_params_t params;
    uint32_t address = 0U;
    bento_status_t status = BENTO_OK;

    *up = wifi_connected();
    if (!*up && !ssid_unset()) {
        bento_wifi_connect_params_init(&params);
        params.ssid = WIFI_SSID;
        params.ssid_len = (uint32_t)strlen(WIFI_SSID);
        params.pass = WIFI_PASS;
        params.pass_len = (uint32_t)strlen(WIFI_PASS);
        params.flags = BENTO_WIFI_F_SAVE_CREDS; /* as the binding */
        status = bento_wifi_connect_start(&params);
        if (status == BENTO_OK) {
            status = wait_join();
        }
        *up = status == BENTO_OK;
    }
    if (*up) {
        *up =
            (bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) == BENTO_OK) &&
            (address != 0U);
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* Asks the started connect every MQTT_SLICE_MS until it ends. */
static bento_status_t wait_broker(uint32_t broker)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = wait_ms(MQTT_SLICE_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(broker);
        }
    }
    return status;
}

/* A fresh client id each time, so the broker never kicks a twin. */
static bento_status_t open_broker(link_t *link)
{
    bento_mqtt_config_t config;
    char client_id[NAME_BYTES];
    bento_status_t status;

    (void)snprintf(
        client_id, sizeof(client_id), "bento-ai-%s-%04" PRIx32, TEAM,
        bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK);
    bento_mqtt_config_init(&config);
    config.host = BROKER;
    config.host_len = (uint32_t)strlen(BROKER);
    config.port = (uint16_t)PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    status = bento_mqtt_open_start(&config, &link->broker);
    link->has_broker = status == BENTO_OK;
    if (link->has_broker) {
        status = wait_broker(link->broker);
    }
    link->online = status == BENTO_OK;
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

static void close_broker(link_t *link)
{
    if (link->has_broker) {
        /* the .py ignores an OSError from disconnect() */
        (void)bento_mqtt_close(link->broker);
        link->has_broker = false;
    }
}

/* Wi-Fi, then the broker; offline when either fails. */
static bento_status_t go_online(link_t *link)
{
    bool up = false;
    bento_status_t status = wifi_up(&up);

    close_broker(link);
    link->online = false;
    if ((status == BENTO_OK) && up) {
        status = open_broker(link);
    }
    return status;
}

static bool broker_alive(const link_t *link)
{
    uint32_t state = BENTO_MQTT_STATE_CLOSED;

    return (bento_mqtt_state(link->broker, &state) == BENTO_OK) &&
           (state == BENTO_MQTT_STATE_CONNECTED);
}

/* Prints the line, then publishes it when online. */
static bool send(const link_t *link, const char *line)
{
    bool delivered = false;

    (void)printf("%s\n", line);
    if (link->online) {
        delivered = bento_mqtt_publish(link->broker, link->topic,
                                       (uint32_t)strlen(link->topic),
                                       (const uint8_t *)line,
                                       (uint32_t)strlen(line), 0U,
                                       0U) == BENTO_OK;
    }
    return delivered;
}

/* round(x, 2), then the number as MicroPython prints it. */
static void format_hundredths(char *text, uint32_t capacity,
                              bento_real_t value)
{
    const bento_real_t scale = (bento_real_t)PERCENT;
    const bento_real_t scaled = value * scale;
    const bento_real_t rounded =
        (bento_real_t)nearbyint((double)scaled) / scale;
    uint32_t length = 0U;

    if (bento_fmt_real(text, capacity, rounded, &length) != BENTO_OK) {
        text[0] = '\0'; /* BENTO_FMT_BUF_SIZE always holds it */
    }
}

/* The message of contract item 3.11 (m = "pump", rk = "vib"). */
static void write_payload(pump_t *pump, agree_t agree, int32_t level)
{
    static const char *const agree_keys[] = {"none", "ai", "rule",
                                             "both"};
    char rule_vib[BENTO_FMT_BUF_SIZE];
    char rule_limit[BENTO_FMT_BUF_SIZE];
    const verdict_t *ai = &pump->ai;

    format_hundredths(rule_vib, sizeof(rule_vib), pump->vib.vib);
    format_hundredths(rule_limit, sizeof(rule_limit),
                      vib_limit(&pump->vib));
    (void)snprintf(
        pump->payload, sizeof(pump->payload),
        "{\"id\": \"%s\", \"n\": %" PRId32 ", \"m\": \"pump\", "
        "\"model\": \"%s\", \"label\": \"%s\", \"conf\": %" PRId32
        ", \"p2\": \"%s\", \"c2\": %" PRId32 ", \"ms\": %" PRId32
        ", \"rk\": \"vib\", \"rule\": %d, \"rv\": %s, \"rl\": %s, "
        "\"agree\": \"%s\", \"lvl\": %" PRId32 ", \"alarm\": %d}",
        TEAM, pump->link.sent, pump->model.name, ai->label, ai->conf,
        ai->second, ai->second_conf, ai->latency_ms,
        vib_on(&pump->vib) ? 1 : 0, rule_vib, rule_limit,
        agree_keys[agree], level, pump->alarm ? 1 : 0);
}

/* Sends when the key (label, level, alarm) changed, or as a
 * heartbeat, never closer than GAP_MS. */
static void report(pump_t *pump, agree_t agree, int32_t level,
                   uint32_t now_ms)
{
    link_t *link = &pump->link;
    const bool changed = !link->has_key || (link->key_level != level) ||
                         (link->key_alarm != pump->alarm) ||
                         (strcmp(link->key_label, pump->ai.label) != 0);
    bool delivered;

    if (should_send(changed,
                    bento_time_ticks_diff(bento_time_ticks_ms(),
                                          link->sent_ms))) {
        link->sent++;
        write_payload(pump, agree, level);
        delivered = send(link, pump->payload);
        link->delivered += delivered ? 1 : 0;
        link->sent_ms = bento_time_ticks_ms();
        link->has_key = true;
        (void)snprintf(link->key_label, sizeof(link->key_label), "%s",
                       pump->ai.label);
        link->key_level = level;
        link->key_alarm = pump->alarm;
        if (link->online && !delivered) {
            link->online = false;
            link->retry_ms = now_ms;
        }
    }
}

/* A lost broker goes offline; offline tries again every RETRY_MS. */
static bento_status_t keep_link(pump_t *pump, uint32_t now_ms)
{
    link_t *link = &pump->link;
    bento_status_t status = BENTO_OK;

    if (link->online && !broker_alive(link)) {
        link->online = false;
        link->retry_ms = now_ms;
    }
    if (link->can_net && !link->online &&
        (bento_time_ticks_diff(now_ms, link->retry_ms) >= RETRY_MS)) {
        show_text(pump, SLOT_NOTE, "กำลังต่อใหม่...");
        poll_display(pump->ui);
        status = go_online(link);
        link->retry_ms = bento_time_ticks_ms();
        show_text(pump, SLOT_NOTE,
                  link->online ? "ต่อใหม่สำเร็จ" : "ยังต่อไม่ได้ จะลองใหม่เอง");
    }
    return status;
}

/* ---- the screen ---- */

/* One widget of the screen's table. */
typedef struct {
    kind_t kind;
    slot_t slot;
    const char *text;
    int16_t x;
    int16_t y;
    int16_t size; /* a label's font, else the width and height */
    uint32_t color;
} part_t;

static bento_status_t add_part(pump_t *pump, const part_t *part)
{
    bento_ui_widget_args_t args;
    bento_ui_id_t made = 0U;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = part->x;
    args.y = part->y;
    args.color = part->color;
    if (part->kind == KIND_LABEL) {
        args.text = part->text;
        args.text_len = (uint32_t)strlen(part->text);
        args.font_px = part->size;
        status = bento_ui_label_create(pump->ui, &args, &made);
    } else {
        args.w = part->size;
        args.h = part->size;
        args.font_px = 0;
        if (part->kind == KIND_ARC) {
            args.max_val = PERCENT;
            status = bento_ui_arc_create(pump->ui, &args, &made);
        } else {
            status = bento_ui_led_create(pump->ui, &args, &made);
        }
    }
    if (part->slot != SLOT_NONE) {
        pump->ids[part->slot] = made;
    }
    return status;
}

static bento_status_t build_screen(pump_t *pump)
{
    static const part_t parts[] = {
        {KIND_LABEL, SLOT_NONE, "ปั๊มสั่นผิดปกติ: กฎ กับ AI", 12, 6, 24,
         COLOR_TEXT},
        {KIND_LABEL, SLOT_MODEL, "หาโมเดล...", 12, 44, 20, COLOR_INFO},
        {KIND_LABEL, SLOT_NONE, "AI", 12, 90, 20, COLOR_INFO},
        {KIND_ARC, SLOT_ARC, "", 12, 120, 150, 0U},
        {KIND_LABEL, SLOT_AI, "-", 180, 130, 24, COLOR_TEXT},
        {KIND_LABEL, SLOT_SECOND, " ", 180, 170, 20, COLOR_DIM},
        {KIND_LABEL, SLOT_HISTORY, " ", 180, 200, 20, COLOR_DIM},
        {KIND_LABEL, SLOT_NONE, "กฎ (IMU)", 420, 90, 20, COLOR_INFO},
        {KIND_LABEL, SLOT_RULE, "กำลังเรียนค่าปกติ: วางนิ่ง", 420, 130, 20,
         COLOR_TEXT},
        {KIND_LABEL, SLOT_VERDICT, " ", 420, 170, 20, COLOR_DIM},
        {KIND_LED, SLOT_LED, "", 12, 290, 36, COLOR_BAD},
        {KIND_LABEL, SLOT_AGREE, "-", 60, 296, 20, COLOR_TEXT},
        {KIND_LED, SLOT_MQTT, "", 12, 350, 24, COLOR_OK},
        {KIND_LABEL, SLOT_SENT, "ยังไม่ได้ส่ง", 44, 352, 20, COLOR_DIM},
        {KIND_LABEL, SLOT_NOTE, " ", 12, 400, 20, COLOR_WARN}};
    const uint32_t count = (uint32_t)(sizeof(parts) / sizeof(parts[0]));
    bento_status_t status = bento_ui_screen_default(pump->ui);
    uint32_t i;

    if (status == BENTO_OK) {
        status = wait_ms(SETTLE_MS);
    }
    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_part(pump, &parts[i]);
    }
    if (status == BENTO_OK) {
        poll_display(pump->ui);
    }
    return status;
}

/* "%.2f" as the board's MicroPython writes it. */
static void format_two(char *text, uint32_t capacity,
                       bento_real_t value)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, capacity, value, 2, &length) !=
        BENTO_OK) {
        text[0] = '\0'; /* BENTO_FMT_BUF_SIZE always holds it */
    }
}

static void draw_ai(const pump_t *pump)
{
    const verdict_t *ai = &pump->ai;
    char text[TEXT_BYTES];

    (void)bento_ui_set_value(pump->ui, pump->ids[SLOT_ARC], ai->conf);
    show_color(pump, SLOT_ARC, ai->on ? COLOR_BAD : COLOR_OK);
    (void)snprintf(text, sizeof(text), "%s %" PRId32 " %%", ai->label,
                   ai->conf);
    show_text(pump, SLOT_AI, text);
    (void)snprintf(text, sizeof(text), "อันดับสอง %s %" PRId32 " %%",
                   ai->second, ai->second_conf);
    show_text(pump, SLOT_SECOND, text);
    (void)snprintf(text, sizeof(text),
                   "เหตุ %" PRIu32 " ใน %" PRIu32 " ผลล่าสุด",
                   history_hits(ai), ai->history_count);
    show_text(pump, SLOT_HISTORY, text);
}

static void draw_rule(const pump_t *pump)
{
    const vib_t *vib = &pump->vib;
    const bool on = vib_on(vib);
    char vib_text[BENTO_FMT_BUF_SIZE];
    char limit_text[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];

    if (vib->fails >= WIN) {
        show_text(pump, SLOT_RULE, "อ่าน IMU ไม่ได้");
    } else if (vib->has_base) {
        format_two(vib_text, sizeof(vib_text), vib->vib);
        format_two(limit_text, sizeof(limit_text), vib_limit(vib));
        (void)snprintf(text, sizeof(text), "สั่น %s / เกณฑ์ %s", vib_text,
                       limit_text);
        show_text(pump, SLOT_RULE, text);
    } else {
        /* still learning: the first text stays */
    }
    show_color(pump, SLOT_RULE, on ? COLOR_BAD : COLOR_TEXT);
    show_text(pump, SLOT_VERDICT, on ? "กฎบอก: เกิดเหตุ" : "กฎบอก: ปกติ");
}

static void draw_link(const pump_t *pump, agree_t agree)
{
    static const char *const agree_texts[] = {
        "ปกติ", "AI เห็นฝ่ายเดียว: ให้คนไปดู", "กฎเห็นฝ่ายเดียว: ให้คนไปดู",
        "ยืนยันสองทาง: เตือน!"};
    static const uint32_t agree_colors[] = {COLOR_OK, COLOR_WARN,
                                            COLOR_WARN, COLOR_BAD};
    const link_t *link = &pump->link;
    char text[TEXT_BYTES];

    show_text(pump, SLOT_AGREE, agree_texts[agree]);
    show_color(pump, SLOT_AGREE, agree_colors[agree]);
    (void)bento_ui_set_value(pump->ui, pump->ids[SLOT_MQTT],
                             link->online ? 1 : 0);
    (void)snprintf(
        text, sizeof(text), "ส่งได้ %" PRId32 " จาก %" PRId32 " ข้อความ%s",
        link->delivered, link->sent, link->online ? "" : " (ออฟไลน์)");
    show_text(pump, SLOT_SENT, text);
}

/* ---- the main program ---- */

/* Both judges agree, or stop agreeing: the LED, the matrix and the
 * tones follow. */
static bento_status_t follow_alarm(pump_t *pump, int32_t level)
{
    const bool alarm = level == ALARM_LEVEL;
    bento_status_t status = BENTO_OK;

    if (alarm != pump->alarm) {
        pump->alarm = alarm;
        status = set_led(pump, pump->alarm);
        if (status == BENTO_OK) {
            matrix_scroll(pump->alarm ? "PUMP ALARM" : "PUMP OK",
                          pump->alarm ? BENTO_RGBMATRIX_RED
                                      : BENTO_RGBMATRIX_GREEN);
            status = pump->alarm
                         ? beep(pump, NOTE_HIGH, NOTE_ALARM_LOW)
                         : beep(pump, NOTE_CLEAR_LOW, NOTE_HIGH);
        }
    }
    return status;
}

/* One pass every SAMPLE_MS: the rule, the AI, the comparison, the
 * report, the link and the screen. */
static bento_status_t run_pass(pump_t *pump)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bento_real_t magnitude = (bento_real_t)0;
    const bool read = read_magnitude(&magnitude);
    agree_t agree;
    int32_t level;
    bento_status_t status;

    vib_push(&pump->vib, read, magnitude, now_ms);
    read_ai(pump);
    agree = agreement(pump->ai.on, vib_on(&pump->vib));
    level = level_of(agree);
    status = follow_alarm(pump, level);
    if (status == BENTO_OK) {
        report(pump, agree, level, now_ms);
        status = keep_link(pump, now_ms);
    }
    if ((status == BENTO_OK) &&
        (bento_time_ticks_diff(now_ms, pump->draw_ms) >= DRAW_MS)) {
        draw_ai(pump);
        draw_rule(pump);
        draw_link(pump, agree);
        pump->draw_ms = now_ms;
    }
    if (status == BENTO_OK) {
        poll_display(pump->ui);
        status = wait_ms(SAMPLE_MS);
    }
    return status;
}

static bento_status_t run_loop(pump_t *pump)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    pump->vib.start_ms = start_ms;
    pump->link.sent_ms = start_ms;
    pump->link.retry_ms = start_ms;
    pump->draw_ms = start_ms;
    (void)snprintf(pump->ai.label, sizeof(pump->ai.label), "-");
    (void)snprintf(pump->ai.second, sizeof(pump->ai.second), "-");
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_pass(pump);
    }
    return status;
}

/* Stopped halfway or not: the AI core is released, the LED goes
 * off, the matrix clears and the link closes. */
static bento_status_t finish(pump_t *pump, bento_status_t run_status)
{
    bento_status_t status;

    if (bento_edgeai_stop_start() == BENTO_OK) {
        (void)finish_op(); /* the .py ignores an OSError from stop() */
    }
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
    status = set_led(pump, false);
    (void)bento_rgbmatrix_clear(); /* an OSError is ignored there */
    close_broker(&pump->link);
    status = (run_status != BENTO_OK) ? run_status : status;
    if (status == BENTO_OK) {
        show_text(pump, SLOT_NOTE,
                  "ครบเวลา - กด Program to Device เพื่อเล่นใหม่");
        poll_display(pump->ui);
    }
    return status;
}

/* Edge AI from C is not in the BENTO Emulator yet: one line on the
 * screen, clear of everything the .py still shows, and the Console. */
static bento_status_t show_emulator_gap(pump_t *pump)
{
    static const char gap[] = "Edge AI จาก C ยังไม่มีใน BENTO Emulator "
                              "ให้รันบน Dev Kit หรือ Eva Kit";
    static const part_t line = {KIND_LABEL, SLOT_NONE, gap,       180,
                                240,        20,        COLOR_WARN};
    const bento_status_t status = add_part(pump, &line);

    if (status == BENTO_OK) {
        poll_display(pump->ui);
        (void)printf("%s / Edge AI from C is not in the BENTO Emulator "
                     "yet: run it on a Dev Kit or Eva Kit.\n",
                     gap);
    }
    return status;
}

static void report_no_model(const pump_t *pump)
{
    show_color(pump, SLOT_NOTE, COLOR_BAD);
    show_text(pump, SLOT_NOTE, "ต้อง Deploy โมเดลจาก Edge AI Store ก่อน");
    poll_display(pump->ui);
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
    (void)printf("ไม่พบโมเดล ('%s', '%s')\n", MODEL_KEYS[0],
                 MODEL_KEYS[1]);
}

/* The model found: the network, the LED, then the run. */
static bento_status_t run_with_model(pump_t *pump)
{
    char text[TEXT_BYTES];
    link_t *link = &pump->link;
    bento_status_t status = BENTO_OK;

    (void)snprintf(text, sizeof(text), "โมเดล: %s", pump->model.name);
    show_text(pump, SLOT_MODEL, text);
    (void)snprintf(link->topic, sizeof(link->topic), "bento-aiot/%s/ai",
                   TEAM);
    link->can_net = team_ok() && !(ssid_unset() && !wifi_connected());
    if (!link->can_net) {
        show_text(pump, SLOT_NOTE, "แก้ TEAM และ Hotspot ก่อน: ออฟไลน์");
    } else {
        status = go_online(link);
    }
    if (status == BENTO_OK) {
        find_led(pump);
        status = finish(pump, run_loop(pump));
    }
    return status;
}

int main(void)
{
    pump_t *pump = &g_pump;
    bool no_link = false;
    bento_status_t status;

    (void)memset(pump, 0, sizeof(*pump));
    pump->ui = bento_runtime_get_ui();
    status = bento_ui_sfx_volume_set(pump->ui, SPEAKER);
    if (status == BENTO_E_UNSUPPORTED) {
        status = BENTO_OK; /* no volume control: the .py's hasattr() */
    }
    if (status == BENTO_OK) {
        status = build_screen(pump);
    }
    if (status == BENTO_OK) {
        status = find_model(&pump->model, &no_link);
    }
    if ((status == BENTO_OK) && no_link) {
        status = show_emulator_gap(pump);
    }
    if ((status == BENTO_OK) && !pump->model.found) {
        report_no_model(pump);
    } else if (status == BENTO_OK) {
        status = run_with_model(pump);
    } else {
        /* Stop or a failure ends the run */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
