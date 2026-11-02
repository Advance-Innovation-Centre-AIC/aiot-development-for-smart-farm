/*
 * sf3_08_farm_ai_missions.c -- ภารกิจ AI ในฟาร์ม: กฎที่เขียนเอง + AI
 * บนบอร์ด แล้วดูว่าเห็นตรงกันไหม
 *
 * เลือกภารกิจด้วย MISSION บรรทัดเดียว (pump siren barn night voice
 * worker drill) บอร์ดตัดสินสองทางพร้อมกัน: กฎที่อ่านออก เช่น
 * "ค่าสั่นเกินค่าปกติ x 3" และโมเดล Edge AI (ป้ายของภารกิจชนะ >= 60 %
 * ใน 3 ของ 5 ผลล่าสุด) แล้วบอกว่าเห็นตรงกันไหมในตาราง 2x2 ระดับ 2
 * (ทั้งคู่) = เตือนจริง: เสียง ไฟแดง และจอไฟ RGB ซ้ำจนมีคนรับทราบ
 * (SW6 หรือแอปส่ง ack) ระดับ 1 (ฝ่ายเดียว) = ให้คนไปดู SW5 = พัก /
 * ทำต่อ worker: นิ่งครบ 30 วิ บอร์ดถามว่าโอเคไหม ไม่ตอบใน 30 วิ =
 * เตือน voice: พูด go แล้วพูดซ้ำหรือกด SW6 ภายใน 3 วิ = เปิดปั๊ม
 * (ไม่มีโมเดลใช้ปุ่มแทนเสียง) สถานะส่งขึ้น bento-aiot/<TEAM>/ai
 * เหตุการณ์ขึ้น .../event และรับคำสั่งจาก .../ai/cmd แก้ MISSION
 * WIFI_SSID WIFI_PASS และ TEAM ก่อนรัน
 *
 * Pick a mission with the one MISSION line (pump siren barn night
 * voice worker drill). The board judges two ways at once: a rule you
 * can read, such as "vibration over normal x 3", and an Edge AI model
 * (the mission's label wins >= 60 % in 3 of the last 5 results), and
 * shows in a 2x2 table whether they agree. Level 2 (both) is a real
 * alarm: a tone, the red LED and the RGB matrix, repeated until
 * someone acknowledges it (SW6, or an ack from the app). Level 1 (one
 * side) sends someone to look. SW5 pauses and resumes. worker: still
 * for 30 s, the board asks whether all is well; no answer in 30 s
 * raises the alarm. voice: say go, then say it again or press SW6
 * within 3 s to start the pump (without the model, the buttons stand
 * in for the voice). The state goes to bento-aiot/<TEAM>/ai, events
 * to .../event, and commands come from .../ai/cmd. Set MISSION,
 * WIFI_SSID, WIFI_PASS and TEAM first.
 *
 * Twin of sf3_08_farm_ai_missions.py
 * (aiot-development-for-smart-farm/s3/sf3_08_farm_ai_missions.py).
 * Style: CLEAN_C.md.
 * Differs from the original:
 *   - mic.* has no C API yet: the twin takes the .py's own path for
 *     a firmware without the mic module, so the loud rule of siren,
 *     night and drill always reads "cannot read" (rule = 0).
 *   - json.dumps becomes snprintf into buffers sized for every field;
 *     the keys are written in the .py's order (MicroPython's dict may
 *     print them in another order). Model names and labels are
 *     written as the board gives them: they hold no quote.
 *   - json.loads of ai/cmd is a small field finder: it reads the
 *     top-level "cmd" and "model" strings of an object; it does not
 *     check the rest of the text the way json.loads does.
 *   - "%g" is MicroPython's str() of the number without a trailing
 *     ".0", the same text for the values a student sets.
 *   - select() failures print the C status number in place of the
 *     OSError text.
 *   - barn keeps the times of at most COUGHS_KEPT coughs in the last
 *     10 minutes (the .py's list has no limit).
 *   - edge_ai.select(), wifi.connect() and mqtt.connect() wait inside
 *     the binding; here they are started and polled, so Stop ends a
 *     wait at once. A failed broker try closes its client before the
 *     next one, and the program closes the module's Edge AI link
 *     (bento_edgeai_deinit) after the final stop.
 *   - A button read that fails ends the run, as an exception from
 *     buttons.pressed() does. A board without the model link
 *     (reTerminal) runs rule-only, as an empty edge_ai.models().
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_edgeai.h"
#include "bento_fmt.h"
#include "bento_gpio.h"
#include "bento_mqtt.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* Edit before running: the mission (see MISSIONS below), the team's
 * hotspot and the team. TEAM still teamXX = offline, every message
 * goes to the console. */
static const char MISSION[] = "pump";
static const char WIFI_SSID[] = "<ชื่อ Hotspot ของกลุ่ม>";
static const char TEAM[] = "teamXX";
/* The hotspot's password is in wifi_up(), the broker in
 * open_broker(). */
/* Where the other .py settings live:
 * SENSOR_MIC: now BENTO_EDGEAI_SENSOR_MIC in ai_select().
 * K_VIB: now K_VIB_TENTHS (3.0 = 30) below, read by judge_vib().
 * MOVE_VIB: now MOVE_VIB_TENTHS (0.3 = 3) below, read by read_imu().
 * ACK_REARM_MS: now ACKED_REARM_MS below, read by alarm_update().
 * LOOK_TEXT: now the text "LOOK" in wanted_picture().
 * CHECK_TEXT: now the text "OK? SW6" in wanted_picture().
 * MIC_SENS: not used in C because mic.* has no C API (header).
 * QUIET_GAP_MS: not used in C because only the mic's loud rule
 * reads it, and mic.* has no C API (header). */

/* The AI side (counted as sf3_04). */
enum {
    CONF_MIN = 60,   /* below this many % a result is not trouble */
    CONFIRM_N = 3,   /* the label must win this many times ... */
    WINDOW_N = 5,    /* ... in this many latest results */
    STALL_MS = 8000, /* no new result this long = "AI silent" */
    MUTE_MS = 800    /* after the board's own tone, no listening */
};

/* The rule side. */
enum {
    SAMPLE_MS = 50, /* one pass of the loop */
    WIN = 20,       /* vibration over this many samples; misses */
    LEARN_MS = 5000,
    MIN_BASE_HUNDREDTHS = 5, /* pump: lowest normal, 0.05 m/s^2 */
    K_VIB_TENTHS = 30,       /* pump: over normal x 3.0 = trouble */
    COUGH_MAX = 10,          /* barn: this many coughs ... */
    RATE_MS = 600000,        /* ... in 10 minutes */
    TAP_S = 30,              /* barn: tap heard this long = left on */
    MOVE_VIB_TENTHS = 3,     /* worker: moving above 0.3 m/s^2 */
    STILL_S = 30,            /* worker: still this long = ask */
    CHECKIN_S = 30,          /* worker: no answer this long = alarm */
    LOUD_LV = 60             /* mic.level() from here on = loud */
};

/* The voice mission: the board is the PLC's gateway (item 4.2). */
enum {
    PUMP_SEC = 10,
    PUMP_MAX_S = 30,
    GO_CONFIRM_MS = 3000, /* go must be confirmed within this */
    PLC_GAP_MS = 3000     /* plc/cmd at most this often (not stop) */
};

/* MQTT and timing. */
enum {
    HEARTBEAT_MS = 2000,
    GAP_MS = 200,
    MAX_BYTES = 255, /* one state message at most */
    CMD_MAX = 200,   /* a longer ai/cmd is ignored */
    CMD_GAP_MS = 3000,
    ALARM_REPEAT_MS = 10000, /* 0 = sound once */
    ACKED_REARM_MS = 10000,
    DRAW_MS = 500,
    MATRIX_MS = 3000, /* send the matrix picture again this often */
    SCROLL_MS = 80,
    VOICE_SHOW_MS = 5000,
    RUN_MS = 1800000,
    VOLUME = 25,
    SPEAKER = 40,
    SW6 = 0, /* button 0 is SW6 (top), 1 is SW5 (bottom) */
    SW5 = 1
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

/* The link, the waits, the tones and the sizes of what is built. */
enum {
    PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CLIENT_SUFFIX_MASK = 0xFFFF, /* four hex digits of the clock */
    WIFI_SLICE_MS = 50,
    MQTT_SLICE_MS = 20,
    OP_POLL_MS = 20,
    BUTTON_SLICE_MS = 20,
    SETTLE_MS = 200, /* let the cleared screen settle */
    TONE_MS = 120,
    NOTES_MAX = 3,
    MATRIX_BAR_MAX = 8,
    BASE_DIGITS = 3, /* the learnt normal level, "%.3f" */
    ASCII_KEEP = 20, /* the matrix shows at most this many */
    ALARM_LEVEL = 2,
    PERCENT = 100,
    TENTHS = 10,
    MS_PER_S = 1000,
    MS_PER_MINUTE = 60000,
    SUFFIX_CHARS = 4, /* "_out" of wood_out and plastic_out */
    FOUND_MAX = 8,
    COUGHS_KEPT = 256,
    FAIL_PRINT_EVERY = 100,
    NAME_BYTES = 48,  /* the client id or a topic */
    TEXT_BYTES = 128, /* a label's text: a SET_TEXT holds 126 */
    TAIL_BYTES = 160, /* the keys an event adds */
    PAYLOAD_BYTES = 512,
    TEAM_CHARS = 6, /* "team" and two digits */
    TEAM_PREFIX = 4
};

/* The rule each mission pairs with the AI (rk). */
typedef enum {
    RULE_VIB = 0,
    RULE_LOUD,
    RULE_RATE,
    RULE_DURATION,
    RULE_BUTTON,
    RULE_UNSET
} rule_kind_t;

/* The missions, in the order of MISSIONS. */
typedef enum {
    MISSION_PUMP = 0,
    MISSION_SIREN,
    MISSION_BARN,
    MISSION_NIGHT,
    MISSION_VOICE,
    MISSION_WORKER,
    MISSION_DRILL
} mission_id_t;

/* One mission: its Thai name, the farm's problem, the models tried
 * in order, the labels that count as trouble, the rule, the
 * matrix's alarm text, the loud seconds and the no-model note. */
typedef struct {
    const char *key;
    const char *thai;
    const char *problem;
    const char *store_model;
    const char *builtin_model; /* "" = none */
    const char *hits;          /* the labels, one space between */
    rule_kind_t rule;
    const char *alarm_text;
    int32_t loud_s;
    const char *no_model;
} mission_t;

static const mission_t MISSIONS[] = {
    {"pump", "หมอปั๊มน้ำ", "ปั๊มสั่นผิดปกติ ซ่อมก่อนพัง", "AnomalousVibration",
     "Motion Detection", "anomaly shaking", RULE_VIB, "STOP PUMP", 2,
     "ไม่มีโมเดล: ใช้กฎอย่างเดียว"},
    {"siren", "ยามฟังไซเรน", "รถฉุกเฉินมา ต้องเปิดประตู", "SirenDetection",
     "Siren Detection", "sirens", RULE_LOUD, "OPEN GATE", 2,
     "ไม่มีโมเดล: ใช้กฎอย่างเดียว"},
    {"barn", "หูโรงเรือน", "หมูไอถี่ / ก๊อกน้ำเปิดทิ้ง", "HomeSounds",
     "Cough Detection", "cough water_tap baby_cry", RULE_RATE,
     "CHECK BARN", 2, "ไม่มีโมเดล: ใช้กฎอย่างเดียว"},
    {"night", "ยามกลางคืน", "เลื่อยยนต์ ไฟไหม้ สุนัข ตอนดึก",
     "Environment Sounds", "Alarm Detection",
     "chainsaw crackling_fire dog alarm", RULE_LOUD, "CHECK FARM", 2,
     "ไม่มีโมเดล: ใช้กฎอย่างเดียว"},
    {"voice", "สั่งปั๊มด้วยเสียง", "พูด go / stop สั่งปั๊มผ่าน PLC",
     "Voice Commands", "", "go stop", RULE_BUTTON, "PUMP", 2,
     "ไม่มีโมเดล: ปุ่มแทนเสียง (ไม่ใช่ AI)"},
    {"worker", "คนงานปลอดภัย", "คนงานคนเดียว ล้มหรือหมดสติ", "HumanActivity",
     "", "sitting standing", RULE_DURATION, "SOS", 2,
     "ไม่มีโมเดล: ใช้ IMU อย่างเดียว"},
    {"drill", "ช่างซ่อมโรงเรือน", "สว่านทะลุชิ้นงาน (ผู้สอนสาธิต)",
     "DrillMaterialMic", "", "wood_out plastic_out", RULE_LOUD,
     "STOP DRILL", 1, "ไม่มีโมเดล: ใช้ความดังอย่างเดียว"}};

/* "model" when the board has no model of the mission. */
static const char RULE_ONLY[] = "rule-only";

/* Who sees trouble; the screen's cells and the level follow it. */
typedef enum {
    AGREE_NONE = 0,
    AGREE_AI,
    AGREE_RULE,
    AGREE_BOTH
} agree_t;

static const char *const AGREE_KEYS[] = {"none", "ai", "rule", "both"};

/* The tunes the board plays; each one mutes the ears for MUTE_MS. */
typedef enum {
    TUNE_ALARM = 0, /* 84 76 */
    TUNE_CLEAR,     /* 79 84 */
    TUNE_ASK,       /* 72 79 */
    TUNE_STOP,      /* 79 72 */
    TUNE_NO,        /* 69 */
    TUNE_CLICK,     /* 72 */
    TUNE_OK         /* 72 79 84 */
} tune_t;

/* The widgets the program changes, in the order they are made. */
typedef enum {
    SLOT_MQTT = 0,
    SLOT_MQTT_TEXT,
    SLOT_MODEL,
    SLOT_ARC,
    SLOT_LABEL,
    SLOT_MEANING,
    SLOT_CONF,
    SLOT_AI,
    SLOT_RULE_DESC,
    SLOT_RULE_VALUE,
    SLOT_RULE_BAR,
    SLOT_RULE_STATE,
    SLOT_RULE_NOTE,
    SLOT_CELL_BOTH,
    SLOT_CELL_AI,
    SLOT_CELL_RULE,
    SLOT_CELL_NONE,
    SLOT_LEVEL_LED,
    SLOT_LEVEL,
    SLOT_ALARM,
    SLOT_NOTE,
    SLOT_COUNT,
    SLOT_NONE
} slot_t;

typedef enum {
    KIND_LABEL = 0,
    KIND_ARC,
    KIND_BAR,
    KIND_LED,
    KIND_PANEL
} kind_t;

/* A number of the state message: the .py sends an int or a float. */
typedef struct {
    bool real;
    bento_real_t value;
} number_t;

/* What a widget shows now, so it is written only when it changes. */
typedef struct {
    bool has_text;
    char text[TEXT_BYTES];
    bool has_color;
    uint32_t color;
    bool has_value;
    int32_t value;
} shown_t;

/* The last WINDOW_N flags and the alert they give (sf3_04). */
typedef struct {
    uint8_t flags[WINDOW_N];
    uint32_t count;
    bool on;
} window_t;

/* The AI side: the models found, the one running, its latest
 * result and the windows of trouble (and of rain, at night). */
typedef struct {
    uint32_t found_count;
    uint32_t found_index[FOUND_MAX];
    char found_name[FOUND_MAX][BENTO_EDGEAI_NAME_LEN];
    int32_t index; /* -1: no model */
    char name[BENTO_EDGEAI_NAME_LEN];
    uint32_t class_count;
    char labels[BENTO_EDGEAI_MAX_CLASSES][BENTO_EDGEAI_LABEL_LEN];
    bool mic;
    bool running;
    bool has_sequence;
    uint32_t sequence;
    window_t history;
    bool stalled;
    window_t rain;
    char label[BENTO_EDGEAI_LABEL_LEN];
    int32_t conf;
    char second[BENTO_EDGEAI_LABEL_LEN];
    int32_t second_conf;
    int32_t latency_ms;
    char hit[BENTO_EDGEAI_LABEL_LEN];
    int32_t hit_conf;
    uint32_t last_new_ms;
} ai_t;

/* pump and worker: the spread of the last WIN magnitudes. */
typedef struct {
    bento_real_t samples[WIN];
    uint32_t count;
    bento_real_t vib;
    bool has_base;
    bento_real_t base;
    bento_real_t total;
    int32_t learnt;
    uint32_t learn_ms;
} vib_t;

/* The rule side: what it reads, how often reading failed, and the
 * clocks of each rule. */
typedef struct {
    rule_kind_t kind;
    int32_t fails;
    int32_t bad_run;
    bool mic_on;
    vib_t vib;
    uint32_t coughs[COUGHS_KEPT];
    uint32_t cough_first;
    uint32_t cough_count;
    bool has_tap;
    uint32_t tap_ms;
    uint32_t still_ms;
} rules_t;

/* One verdict of the rule: 0/1, its key, the value and the limit. */
typedef struct {
    int32_t rule;
    rule_kind_t kind;
    number_t value;
    number_t limit;
} judged_t;

/* The alarm: on, acknowledged, how many, and since when calm. */
typedef struct {
    bool on;
    bool acked;
    int32_t count;
    bool has_calm;
    uint32_t calm_ms;
} alarm_t;

typedef enum {
    WORD_GO = 0,
    WORD_STOP,
    WORD_NONE
} word_t;

typedef enum {
    WHY_DONE = 0, /* "" */
    WHY_WAIT,
    WHY_UNCONFIRMED,
    WHY_GAP
} why_t;

/* What the voice gate decided; ok -1 = heard go, waiting. */
typedef struct {
    bool has;
    word_t command;
    int32_t ok;
    why_t why;
    bool has_plc;
} gate_result_t;

/* voice: the go waiting for a confirm, the last plc/cmd, the last
 * confirm and the last word the AI heard. */
typedef struct {
    bool has_wait;
    uint32_t wait_ms;
    bool has_plc;
    uint32_t plc_ms;
    word_t last;
    bool has_ok;
    uint32_t ok_ms;
    bool has_ai;
    uint32_t ai_ms;
} gate_t;

/* What the matrix shows: a scrolling text or the AI's bar. */
typedef struct {
    bool has;
    bool say;
    const char *text;
    int32_t bar;
    int32_t color;
} picture_t;

/* Everything the program remembers about the run and the link. */
typedef struct {
    bool paused;
    bool ai_off;
    bool online;
    bool has_broker;
    uint32_t broker;
    char base[NAME_BYTES];
    int32_t sent;
    bool has_key;
    char key_label[BENTO_EDGEAI_LABEL_LEN];
    int32_t key_level;
    bool key_alarm;
    bool force;
    uint32_t sent_ms;
    uint32_t command_ms;
    uint32_t quiet_ms;
    uint32_t beep_ms;
    judged_t judged;
    agree_t agree;
    int32_t level;
    alarm_t alarm;
    gate_t gate;
    bool has_asked;
    uint32_t asked_ms;
    bool checkin_failed;
    int32_t holes[2]; /* drill: wood, plastic */
    char voice_text[TEXT_BYTES];
    bool has_say;
    const char *say_text;
    int32_t say_color;
    uint32_t say_ms;
    picture_t picture;
    uint32_t picture_ms;
} state_t;

typedef struct {
    bento_ui_t *ui;
    const mission_t *mission;
    mission_id_t id;
    bento_ui_id_t ids[SLOT_COUNT];
    shown_t shown[SLOT_COUNT];
    bool has_led;
    uint32_t led;
    bool down[2];
    bool clicked[2];
    ai_t ai;
    rules_t rules;
    state_t state;
    char payload[PAYLOAD_BYTES];
    uint8_t inbox[CMD_MAX + 1];
} farm_t;

/* Too big for the 8 KB program stack: file scope, zeroed by the
 * loader and set again at the start of main() (CLEAN_C.md C9,
 * deviation BENTO-MISRA-E03/E04). */
extern farm_t g_farm;
farm_t g_farm;

/* ---- hardware ---- */

/* Sleeps, and turns Stop into BENTO_E_CANCELLED. */
static bento_status_t nap(uint32_t duration_ms)
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

/* A label's text, written only when it changes. MicroPython's
 * widget setters ignore a refusal (modui.c). */
static void put_text(farm_t *farm, slot_t slot, const char *text)
{
    shown_t *shown = &farm->shown[slot];

    if (!shown->has_text || (strcmp(shown->text, text) != 0)) {
        shown->has_text = true;
        (void)snprintf(shown->text, sizeof(shown->text), "%s", text);
        (void)bento_ui_set_text(farm->ui, farm->ids[slot], text,
                                (uint32_t)strlen(text));
    }
}

static void put_color(farm_t *farm, slot_t slot, uint32_t color)
{
    shown_t *shown = &farm->shown[slot];

    if (!shown->has_color || (shown->color != color)) {
        shown->has_color = true;
        shown->color = color;
        (void)bento_ui_set_color(farm->ui, farm->ids[slot], color);
    }
}

static void put(farm_t *farm, slot_t slot, const char *text,
                uint32_t color)
{
    put_text(farm, slot, text);
    put_color(farm, slot, color);
}

/* An Arc, Bar or Led value, written only when it changes. */
static void put_value(farm_t *farm, slot_t slot, int32_t value)
{
    shown_t *shown = &farm->shown[slot];

    if (!shown->has_value || (shown->value != value)) {
        shown->has_value = true;
        shown->value = value;
        (void)bento_ui_set_value(farm->ui, farm->ids[slot], value);
    }
}

static void note(farm_t *farm, const char *text, uint32_t color)
{
    put(farm, SLOT_NOTE, text, color);
}

/* MIDI notes of TONE_MS each, then MUTE_MS of deaf ears: the board's
 * own tone is no event on the farm. */
static bento_status_t sound(farm_t *farm, tune_t tune)
{
    typedef struct {
        uint32_t count;
        uint8_t notes[NOTES_MAX];
    } notes_t;
    static const notes_t tunes[] = {
        {2U, {84U, 76U, 0U}}, {2U, {79U, 84U, 0U}},
        {2U, {72U, 79U, 0U}}, {2U, {79U, 72U, 0U}},
        {1U, {69U, 0U, 0U}},  {1U, {72U, 0U, 0U}},
        {3U, {72U, 79U, 84U}}};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < tunes[tune].count) && (status == BENTO_OK); i++) {
        /* a tone's result is not read in MicroPython either */
        (void)bento_ui_sfx_tone(farm->ui, tunes[tune].notes[i],
                                BENTO_UI_WAVE_SINE, (uint8_t)VOLUME,
                                (uint16_t)TONE_MS);
        status = nap(TONE_MS);
    }
    farm->state.quiet_ms =
        bento_time_ticks_add(bento_time_ticks_ms(), MUTE_MS);
    return status;
}

/* By name, not number: LED1/LED2 of the Dev Kit sit on the SoM where
 * no one sees them. None when the board cannot say. */
static void find_led(farm_t *farm)
{
    bento_gpio_board_info_t info;
    const char *name = NULL;
    uint32_t i;

    bento_gpio_board_info_init(&info);
    farm->has_led =
        (bento_gpio_board_info(&info) == BENTO_OK) && (info.leds > 0U);
    farm->led = 0U;
    for (i = 0U; farm->has_led && (i < info.leds); i++) {
        if ((bento_gpio_led_name(i, &name) == BENTO_OK) &&
            (name != NULL) && (strcmp(name, "RGB_RED") == 0)) {
            farm->led = i;
        }
    }
    farm->has_led =
        farm->has_led && (bento_gpio_led_off(farm->led) == BENTO_OK);
}

static bento_status_t set_led(const farm_t *farm, bool on)
{
    bento_status_t status = BENTO_OK;

    if (farm->has_led) {
        status = on ? bento_gpio_led_on(farm->led)
                    : bento_gpio_led_off(farm->led);
    }
    return status;
}

/* Samples both buttons while it waits, so a short press is never
 * missed; clicked = a press since the last look. */
static bento_status_t sample_buttons(farm_t *farm)
{
    static const uint32_t order[] = {SW6, SW5};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < 2U) && (status == BENTO_OK); i++) {
        bool now_down = false;

        status = bento_buttons_pressed(order[i], &now_down);
        if (status == BENTO_OK) {
            farm->clicked[i] =
                farm->clicked[i] || (now_down && !farm->down[i]);
            farm->down[i] = now_down;
        }
    }
    return status;
}

static bool pressed_now(farm_t *farm, uint32_t which)
{
    const bool fired = farm->clicked[which];

    farm->clicked[which] = false;
    return fired;
}

static bento_status_t wait_sampling(farm_t *farm, uint32_t duration_ms)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            (int32_t)duration_ms)) {
        status = sample_buttons(farm);
        if (status == BENTO_OK) {
            status = nap(BUTTON_SLICE_MS);
        }
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

/* The matrix only draws English letters, digits and signs. */
static void ascii_only(const char *text, char *out, uint32_t capacity)
{
    uint32_t used = 0U;
    uint32_t i;

    for (i = 0U; (text[i] != '\0') && (used < (uint32_t)ASCII_KEEP) &&
                 ((used + 1U) < capacity);
         i++) {
        if ((text[i] >= ' ') && (text[i] <= '~')) {
            out[used] = text[i];
            used++;
        }
    }
    out[used] = '\0';
}

static void matrix_scroll(const char *text, int32_t color)
{
    char shown[ASCII_KEEP + 1];

    ascii_only(text, shown, sizeof(shown));
    /* the .py skips a picture the matrix does not take (OSError) */
    (void)bento_rgbmatrix_scroll(shown, (uint32_t)strlen(shown), color,
                                 SCROLL_MS);
}

/* ---- numbers as MicroPython writes them ---- */

static bento_real_t round_to(bento_real_t value, int32_t scale)
{
    const bento_real_t factor = (bento_real_t)scale;
    const bento_real_t scaled = value * factor;

    return (bento_real_t)nearbyint((double)scaled) / factor;
}

static number_t whole(int32_t value)
{
    const number_t number = {false, (bento_real_t)value};

    return number;
}

static number_t real_rounded(bento_real_t value, int32_t scale)
{
    const number_t number = {true, round_to(value, scale)};

    return number;
}

/* str() of an int or a float. */
static void format_number(char *text, uint32_t capacity,
                          number_t number)
{
    uint32_t length = 0U;

    if (!number.real) {
        (void)snprintf(text, capacity, "%" PRId32,
                       (int32_t)number.value);
    } else if (bento_fmt_real(text, capacity, number.value, &length) !=
               BENTO_OK) {
        text[0] = '\0'; /* BENTO_FMT_BUF_SIZE always holds it */
    } else {
        /* the board's own text */
    }
}

/* "%g": str() without a trailing ".0" (header). */
static void format_short(char *text, uint32_t capacity, number_t number)
{
    size_t length;

    format_number(text, capacity, number);
    length = strlen(text);
    if ((length > 2U) && (strcmp(&text[length - 2U], ".0") == 0)) {
        text[length - 2U] = '\0';
    }
}

/* "%.<digits>f" as the board's MicroPython writes it. */
static void format_fixed(char *text, uint32_t capacity,
                         bento_real_t value, int32_t digits)
{
    uint32_t length = 0U;

    if (bento_fmt_real_precision(text, capacity, value, digits,
                                 &length) != BENTO_OK) {
        text[0] = '\0'; /* BENTO_FMT_BUF_SIZE always holds it */
    }
}

/* Whole seconds left, rounded up. */
static int32_t seconds_left(int32_t left_ms)
{
    const int32_t positive = (left_ms > 0) ? left_ms : 0;

    return (positive + (MS_PER_S - 1)) / MS_PER_S;
}

/* ---- the AI side ---- */

/* Waits for a started select or stop; Stop cancels it. */
static bento_status_t finish_op(void)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = nap(OP_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_edgeai_op_poll(NULL);
        } else {
            (void)bento_edgeai_op_cancel(); /* Stop wins */
        }
    }
    return status;
}

/* edge_ai.stop(): an OSError is ignored there; only Stop is kept. */
static bento_status_t stop_engine(void)
{
    bento_status_t status = bento_edgeai_stop_start();

    if (status == BENTO_OK) {
        status = finish_op();
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* str.lower() of one ASCII letter; any other byte as it is. */
static char lower_ascii(char letter)
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

/* int(pct / 100 * 100) in the board's float, as the .py works it. */
static int32_t percent_of(uint8_t pct)
{
    const float hundred = 100.0F;
    const float score = (float)pct / hundred;
    const bento_real_t scaled =
        (bento_real_t)score * (bento_real_t)PERCENT;

    return (int32_t)scaled;
}

/* Remembers one flag; trouble from CONFIRM_N of the last WINDOW_N,
 * clear only when the whole window is safe (no flicker). */
static void window_push(window_t *window, bool danger)
{
    uint32_t hits = 0U;
    uint32_t i;

    if (window->count == (uint32_t)WINDOW_N) {
        (void)memmove(&window->flags[0], &window->flags[1],
                      sizeof(window->flags) - sizeof(window->flags[0]));
        window->count--;
    }
    window->flags[window->count] = danger ? 1U : 0U;
    window->count++;
    for (i = 0U; i < window->count; i++) {
        hits += window->flags[i];
    }
    window->on =
        (hits >= (uint32_t)CONFIRM_N) || (window->on && (hits > 0U));
}

static uint32_t window_hits(const window_t *window)
{
    uint32_t hits = 0U;
    uint32_t i;

    for (i = 0U; i < window->count; i++) {
        hits += window->flags[i];
    }
    return hits;
}

static void ai_forget_result(ai_t *ai)
{
    (void)memset(&ai->history, 0, sizeof(ai->history));
    (void)snprintf(ai->label, sizeof(ai->label), "-");
    ai->conf = 0;
    (void)snprintf(ai->second, sizeof(ai->second), "-");
    ai->second_conf = 0;
    ai->latency_ms = 0;
}

static void ai_clear(ai_t *ai, uint32_t now_ms)
{
    ai->has_sequence = false;
    ai->stalled = false;
    ai_forget_result(ai);
    (void)memset(&ai->rain, 0, sizeof(ai->rain));
    (void)snprintf(ai->hit, sizeof(ai->hit), "-");
    ai->hit_conf = 0;
    ai->last_new_ms = now_ms;
}

static void read_labels(ai_t *ai, uint32_t class_count)
{
    uint32_t length = 0U;
    uint32_t i;

    ai->class_count = (class_count > BENTO_EDGEAI_MAX_CLASSES)
                          ? BENTO_EDGEAI_MAX_CLASSES
                          : class_count;
    for (i = 0U; i < ai->class_count; i++) {
        if (bento_edgeai_label((uint32_t)ai->index, i, ai->labels[i],
                               BENTO_EDGEAI_LABEL_LEN,
                               &length) != BENTO_OK) {
            ai->labels[i][0] = '\0';
        }
    }
}

/* Loads and starts a model (up to 15 s on the board). Only Stop is
 * an error; a refusal leaves *selected false. */
static bento_status_t ai_select(ai_t *ai, uint32_t index,
                                const char *name, bool *selected)
{
    bento_edgeai_model_info_t info;
    bool known;
    bento_status_t status = bento_edgeai_select_start(index);

    if (status == BENTO_OK) {
        status = finish_op();
    }
    *selected = status == BENTO_OK;
    if (*selected) {
        ai->index = (int32_t)index;
        (void)snprintf(ai->name, sizeof(ai->name), "%s", name);
        bento_edgeai_model_info_init(&info);
        known = bento_edgeai_model_info(index, &info) == BENTO_OK;
        /* the model's sensor: an unreadable row is not the mic */
        ai->mic = known && (info.sensor == BENTO_EDGEAI_SENSOR_MIC);
        read_labels(ai, known ? info.class_count : 0U);
        ai->running = true;
        ai_clear(ai, bento_time_ticks_ms());
    } else if (status != BENTO_E_CANCELLED) {
        (void)printf("select: %s %d\n", name, (int)status);
    } else {
        /* Stop ends the run */
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* Every model whose name holds key. */
static void find_key(ai_t *ai, const char *key, int32_t count)
{
    bento_edgeai_model_info_t info;
    int32_t i;

    for (i = 0; (i < count) && (ai->found_count < (uint32_t)FOUND_MAX);
         i++) {
        bento_edgeai_model_info_init(&info);
        if ((bento_edgeai_model_info((uint32_t)i, &info) == BENTO_OK) &&
            name_has(info.name, key)) {
            ai->found_index[ai->found_count] = (uint32_t)i;
            (void)snprintf(ai->found_name[ai->found_count],
                           BENTO_EDGEAI_NAME_LEN, "%s", info.name);
            ai->found_count++;
        }
    }
}

/* Every model whose name holds one of the mission's keys, key by
 * key: the order on a board can change after a reboot. */
static void ai_find(ai_t *ai, const mission_t *mission)
{
    int32_t count = 0;

    if ((bento_edgeai_init() != BENTO_OK) ||
        (bento_edgeai_count(&count) != BENTO_OK)) {
        count = 0; /* the AI core does not answer: none found */
    }
    find_key(ai, mission->store_model, count);
    if (mission->builtin_model[0] != '\0') {
        find_key(ai, mission->builtin_model, count);
    }
}
/* The Store's model first; none, or none that starts, then the
 * built-in one. */
static bento_status_t ai_start(ai_t *ai, const mission_t *mission,
                               bool *started)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    *started = false;
    ai_find(ai, mission);
    for (i = 0U;
         (i < ai->found_count) && (status == BENTO_OK) && !*started;
         i++) {
        status = ai_select(ai, ai->found_index[i], ai->found_name[i],
                           started);
    }
    return status;
}

static bento_status_t ai_stop(ai_t *ai)
{
    bento_status_t status = BENTO_OK;

    if (ai->running) {
        status = stop_engine();
    }
    ai->running = false;
    ai_clear(ai, bento_time_ticks_ms());
    return status;
}

static bento_status_t ai_resume(ai_t *ai, bool *resumed)
{
    char name[BENTO_EDGEAI_NAME_LEN];
    bento_status_t status = BENTO_OK;

    *resumed = false;
    if (ai->index >= 0) {
        (void)snprintf(name, sizeof(name), "%s", ai->name);
        status = ai_select(ai, (uint32_t)ai->index, name, resumed);
    }
    return status;
}

static const char *label_of(const ai_t *ai, uint32_t index)
{
    return (index < ai->class_count) ? ai->labels[index] : "";
}

/* The runner-up besides the top class, and its %; ("-", 0) when
 * there is none. */
static void find_second(ai_t *ai, const bento_edgeai_result_t *result)
{
    int32_t second = -1;
    uint32_t i;

    for (i = 0U; i < result->class_count; i++) {
        if ((i != result->top) &&
            ((second < 0) ||
             (result->score_pct[i] > result->score_pct[second]))) {
            second = (int32_t)i;
        }
    }
    if (second < 0) {
        (void)snprintf(ai->second, sizeof(ai->second), "-");
        ai->second_conf = 0;
    } else {
        (void)snprintf(ai->second, sizeof(ai->second), "%s",
                       ((uint32_t)second < ai->class_count)
                           ? ai->labels[second]
                           : "-");
        ai->second_conf = percent_of(result->score_pct[second]);
    }
}

/* The thinking time rounded up to whole ms: 0.4 ms -> 1 (0 means
 * "unknown" in the contract). */
static int32_t ceil_ms(uint32_t latency_us)
{
    const float ms_per_us = (float)MS_PER_S;
    const float latency_ms = (float)latency_us / ms_per_us;
    const int32_t whole_ms = (int32_t)latency_ms;

    return whole_ms + ((latency_ms > (float)whole_ms) ? 1 : 0);
}

/* Is label one of the mission's trouble labels (space-separated)? */
static bool is_hit(const mission_t *mission, const char *label)
{
    const char *hits = mission->hits;
    const size_t length = strlen(label);
    bool hit = false;
    size_t start = 0U;

    while (!hit && (hits[start] != '\0')) {
        size_t end = start;

        while ((hits[end] != '\0') && (hits[end] != ' ')) {
            end++;
        }
        hit = ((end - start) == length) && (length > 0U) &&
              (strncmp(&hits[start], label, length) == 0);
        start = (hits[end] == ' ') ? (end + 1U) : end;
    }
    return hit;
}

/* A new result read in: the label, the runner-up, the time, and
 * the windows of trouble and rain. */
static void ai_take(farm_t *farm, const bento_edgeai_result_t *result)
{
    ai_t *ai = &farm->ai;
    const char *label = label_of(ai, result->top);
    bool hit;

    (void)snprintf(ai->label, sizeof(ai->label), "%s",
                   (label[0] != '\0') ? label : "-");
    ai->conf = percent_of(result->score_pct[result->top]);
    find_second(ai, result);
    ai->latency_ms = ceil_ms(result->latency_us);
    hit = is_hit(farm->mission, ai->label) && (ai->conf >= CONF_MIN);
    if (hit) {
        (void)snprintf(ai->hit, sizeof(ai->hit), "%s", ai->label);
        ai->hit_conf = ai->conf;
    }
    window_push(&ai->history, hit);
    if (farm->id == MISSION_NIGHT) { /* rain: an event, not an alarm */
        window_push(&ai->rain, (strcmp(ai->label, "rain") == 0) &&
                                   (ai->conf >= CONF_MIN));
    }
}

/* Reads only a new result of the running model. None for STALL_MS:
 * the old one is no longer used ("-" = no result). */
static void ai_poll(farm_t *farm, uint32_t now_ms, bool quiet)
{
    ai_t *ai = &farm->ai;
    bento_edgeai_result_t result;
    bool fresh = false;

    bento_edgeai_result_init(&result);
    if (ai->running) {
        /* edge_ai.result() raising OSError is the .py's None */
        fresh = (bento_edgeai_result(&result) == BENTO_OK) &&
                result.has_verdict &&
                (!ai->has_sequence || (result.seq != ai->sequence)) &&
                ((int32_t)result.index == ai->index);
    }
    if (ai->running && !fresh && !ai->stalled &&
        (bento_time_ticks_diff(now_ms, ai->last_new_ms) > STALL_MS)) {
        ai->stalled = true;
        ai_forget_result(ai);
    }
    if (fresh) {
        ai->has_sequence = true;
        ai->sequence = result.seq;
        ai->last_new_ms = now_ms;
        ai->stalled = false;
    }
    /* the board's own speaker in the mic: not trusted for a while */
    if (fresh && !(ai->mic && quiet)) {
        ai_take(farm, &result);
    }
}

/* ---- the rule side ---- */

static void rules_reset(rules_t *rules, uint32_t now_ms)
{
    /* pump learns its normal level again at every start or resume */
    (void)memset(&rules->vib, 0, sizeof(rules->vib));
    rules->vib.learn_ms = now_ms;
    rules->cough_first = 0U;
    rules->cough_count = 0U;
    rules->has_tap = false;
    rules->still_ms = now_ms;
}

/* A reading that failed: counted, and printed the first time and
 * every FAIL_PRINT_EVERY. */
static void rules_failed(rules_t *rules, const char *what)
{
    rules->fails++;
    rules->bad_run++;
    if ((rules->fails == 1) ||
        ((rules->fails % FAIL_PRINT_EVERY) == 0)) {
        (void)printf("กฎอ่านค่าไม่ได้: %s รวม %" PRId32 " ครั้ง\n", what,
                     rules->fails);
    }
}

/* The mic opens after the model; with no mic module the loud rule
 * cannot read (header). */
static void rules_init(rules_t *rules, rule_kind_t kind)
{
    rules->kind = kind;
    rules->mic_on = false;
    if (kind == RULE_LOUD) {
        rules_failed(rules, "mic.start");
    }
    rules_reset(rules, bento_time_ticks_ms());
}

static bool rules_broken(const rules_t *rules)
{
    return (rules->bad_run >= WIN) ||
           ((rules->kind == RULE_LOUD) && !rules->mic_on);
}

/* pump: ms left until the normal level is learnt (0 = learnt). */
static int32_t rules_learning(const rules_t *rules, uint32_t now_ms)
{
    int32_t left = 0;

    if ((rules->kind == RULE_VIB) && !rules->vib.has_base) {
        left = LEARN_MS -
               bento_time_ticks_diff(now_ms, rules->vib.learn_ms);
        left = (left > 0) ? left : 0;
    }
    return left;
}

/* The standard deviation of the window: the vibration (sf3_03). */
static bento_real_t spread(const bento_real_t *samples, uint32_t count)
{
    const bento_real_t size = (bento_real_t)count;
    bento_real_t sum = (bento_real_t)0;
    bento_real_t squares = (bento_real_t)0;
    bento_real_t mean;
    bento_real_t variance;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        sum += samples[i];
    }
    mean = sum / size;
    for (i = 0U; i < count; i++) {
        const bento_real_t away = samples[i] - mean;

        squares += away * away;
    }
    variance = squares / size;
    return (bento_real_t)sqrt((double)variance);
}

/* True when the window is full and the vibration is new; the first
 * LEARN_MS learn the normal level (never below MIN_BASE). */
static bool vib_push(vib_t *vib, bento_real_t magnitude,
                     uint32_t now_ms)
{
    const bento_real_t min_base =
        (bento_real_t)MIN_BASE_HUNDREDTHS / (bento_real_t)PERCENT;
    const bool full = (vib->count + 1U) >= (uint32_t)WIN;

    if (vib->count == (uint32_t)WIN) {
        (void)memmove(&vib->samples[0], &vib->samples[1],
                      sizeof(vib->samples) - sizeof(vib->samples[0]));
        vib->count--;
    }
    vib->samples[vib->count] = magnitude;
    vib->count++;
    if (full) {
        vib->vib = spread(vib->samples, vib->count);
    }
    if (full && !vib->has_base) {
        vib->total += vib->vib;
        vib->learnt++;
        if (bento_time_ticks_diff(now_ms, vib->learn_ms) >= LEARN_MS) {
            const bento_real_t mean =
                vib->total / (bento_real_t)vib->learnt;

            vib->base = (mean > min_base) ? mean : min_base;
            vib->has_base = true;
        }
    }
    return full;
}

/* pump and worker: one IMU reading; a worker that moves starts the
 * stillness count again. */
static void read_imu(rules_t *rules, uint32_t now_ms)
{
    const bento_real_t move_vib =
        (bento_real_t)MOVE_VIB_TENTHS / (bento_real_t)TENTHS;
    bento_real_t magnitude = (bento_real_t)0;

    if (!read_magnitude(&magnitude)) {
        rules_failed(rules, "IMU");
    } else {
        rules->bad_run = 0;
        if (vib_push(&rules->vib, magnitude, now_ms) &&
            (rules->kind == RULE_DURATION) &&
            (rules->vib.vib > move_vib)) {
            rules->still_ms = now_ms;
        }
    }
}

/* One reading per pass. The loud rule has no reader (header). */
static void rules_read(rules_t *rules, uint32_t now_ms)
{
    if ((rules->kind == RULE_VIB) || (rules->kind == RULE_DURATION)) {
        read_imu(rules, now_ms);
    }
}
static bento_real_t seconds_since(uint32_t now_ms, uint32_t start_ms)
{
    return (bento_real_t)bento_time_ticks_diff(now_ms, start_ms) /
           (bento_real_t)MS_PER_S;
}

static void forget_old_coughs(rules_t *rules, uint32_t now_ms)
{
    while ((rules->cough_count > 0U) &&
           (bento_time_ticks_diff(
                now_ms, rules->coughs[rules->cough_first]) > RATE_MS)) {
        rules->cough_first = (rules->cough_first + 1U) % COUGHS_KEPT;
        rules->cough_count--;
    }
}

/* One more confirmed cough; the oldest goes when the list is full. */
static void add_cough(rules_t *rules, uint32_t now_ms)
{
    if (rules->cough_count == (uint32_t)COUGHS_KEPT) {
        rules->cough_first = (rules->cough_first + 1U) % COUGHS_KEPT;
        rules->cough_count--;
    }
    rules->coughs[(rules->cough_first + rules->cough_count) %
                  COUGHS_KEPT] = now_ms;
    rules->cough_count++;
}

/* barn. rate: confirmed coughs (the rising edge of 3 in 5) within
 * RATE_MS. dur: water_tap heard without a break; while the tap is
 * on, the screen and the message show the dur rule instead. */
static judged_t rules_barn(rules_t *rules, const ai_t *ai,
                           uint32_t now_ms, bool edge)
{
    judged_t judged;
    bool busy;

    if (edge && (strcmp(ai->hit, "cough") == 0)) {
        add_cough(rules, now_ms);
    }
    forget_old_coughs(rules, now_ms);
    if (ai->history.on && (strcmp(ai->hit, "water_tap") == 0)) {
        rules->tap_ms = rules->has_tap ? rules->tap_ms : now_ms;
        rules->has_tap = true;
    } else {
        rules->has_tap = false;
    }
    busy = rules->cough_count >= (uint32_t)COUGH_MAX;
    if (rules->has_tap) {
        const bento_real_t seconds =
            seconds_since(now_ms, rules->tap_ms);

        judged.rule =
            ((seconds >= (bento_real_t)TAP_S) || busy) ? 1 : 0;
        judged.kind = RULE_DURATION;
        judged.value = real_rounded(seconds, TENTHS);
        judged.limit = whole(TAP_S);
    } else {
        judged.rule = busy ? 1 : 0;
        judged.kind = RULE_RATE;
        judged.value = whole((int32_t)rules->cough_count);
        judged.limit = whole(COUGH_MAX);
    }
    return judged;
}
/* A rule that cannot read says 0, with the limit it would use. */
static judged_t judge_broken(const rules_t *rules,
                             const mission_t *mission)
{
    judged_t judged = {0,
                       rules->kind,
                       {false, (bento_real_t)0},
                       {false, (bento_real_t)0}};

    if (rules->kind == RULE_LOUD) {
        judged.limit = whole(mission->loud_s);
    } else if (rules->kind == RULE_DURATION) {
        judged.limit = whole(STILL_S);
    } else if (rules->kind == RULE_RATE) {
        judged.limit = whole(COUGH_MAX);
    } else {
        /* no limit */
    }
    return judged;
}

static judged_t judge_vib(const vib_t *vib)
{
    const bento_real_t k_vib =
        (bento_real_t)K_VIB_TENTHS / (bento_real_t)TENTHS;
    judged_t judged = {0, RULE_VIB, real_rounded(vib->vib, PERCENT),
                       whole(0)};

    if (vib->has_base) {
        const bento_real_t limit = vib->base * k_vib;

        judged.rule = (vib->vib > limit) ? 1 : 0;
        judged.limit = real_rounded(limit, PERCENT);
    }
    return judged;
}

/* The rule's verdict: (rule 0/1, rk, rv the value, rl the limit). */
static judged_t rules_judge(farm_t *farm, uint32_t now_ms, bool edge)
{
    rules_t *rules = &farm->rules;
    judged_t judged = {0, rules->kind, whole(0), whole(1)};

    if (rules_broken(rules)) {
        judged = judge_broken(rules, farm->mission);
    } else if (rules->kind == RULE_VIB) {
        judged = judge_vib(&rules->vib);
    } else if (rules->kind == RULE_DURATION) {
        const bento_real_t seconds =
            seconds_since(now_ms, rules->still_ms);

        judged.rule = (seconds >= (bento_real_t)STILL_S) ? 1 : 0;
        judged.value = real_rounded(seconds, TENTHS);
        judged.limit = whole(STILL_S);
    } else if (rules->kind == RULE_RATE) {
        judged = rules_barn(rules, &farm->ai, now_ms, edge);
    } else {
        /* a rule with nothing to read */
    }
    return judged;
}

/* ---- the alarm and the voice gate ---- */

typedef enum {
    ACTION_NONE = 0,
    ACTION_START,
    ACTION_CLEAR
} action_t;

/* On when wanted and not acknowledged; off by itself when gone.
 * Once acknowledged, it may start again only after ACKED_REARM_MS of
 * calm, so a flickering label does not call the person back. */
static action_t alarm_update(alarm_t *alarm, bool want, bool gone,
                             uint32_t now_ms)
{
    action_t action = ACTION_NONE;

    if (alarm->acked && want) {
        alarm->has_calm = false;
    } else if (alarm->acked && !alarm->has_calm) {
        alarm->has_calm = true;
        alarm->calm_ms = now_ms;
    } else if (alarm->acked &&
               (bento_time_ticks_diff(now_ms, alarm->calm_ms) >=
                ACKED_REARM_MS)) {
        alarm->acked = false;
    } else {
        /* not acknowledged, or still counting the calm */
    }
    if (!alarm->on && want && !alarm->acked) {
        alarm->on = true;
        alarm->count++;
        action = ACTION_START;
    } else if (alarm->on && gone) {
        alarm->on = false;
        action = ACTION_CLEAR;
    } else {
        /* nothing changes */
    }
    return action;
}

static bool alarm_acknowledge(alarm_t *alarm)
{
    const bool was_on = alarm->on;

    if (was_on) {
        alarm->on = false;
        alarm->acked = true;
        alarm->has_calm = false;
    }
    return was_on;
}

static gate_result_t gate_result(word_t command, int32_t ok, why_t why,
                                 bool has_plc)
{
    const gate_result_t result = {true, command, ok, why, has_plc};

    return result;
}

/* go: at most one plc/cmd per PLC_GAP_MS. */
static gate_result_t gate_go(gate_t *gate, uint32_t now_ms)
{
    gate_result_t result = gate_result(WORD_GO, 0, WHY_GAP, false);

    if (!gate->has_plc ||
        (bento_time_ticks_diff(now_ms, gate->plc_ms) >= PLC_GAP_MS)) {
        gate->has_plc = true;
        gate->plc_ms = now_ms;
        gate->last = WORD_GO;
        result = gate_result(WORD_GO, 1, WHY_DONE, true);
    }
    return result;
}

/* stop: always at once; a repeat inside the gap is not sent again
 * to the PLC's inbox. */
static gate_result_t gate_stop(gate_t *gate, uint32_t now_ms)
{
    gate_result_t result = gate_result(WORD_STOP, 1, WHY_DONE, false);

    if ((gate->last != WORD_STOP) || !gate->has_plc ||
        (bento_time_ticks_diff(now_ms, gate->plc_ms) >= PLC_GAP_MS)) {
        gate->has_plc = true;
        gate->plc_ms = now_ms;
        gate->last = WORD_STOP;
        result.has_plc = true;
    }
    return result;
}

/* A word heard: stop acts now; go waits for a confirm, and a second
 * go in time is the confirm. */
static gate_result_t gate_heard(gate_t *gate, word_t word,
                                uint32_t now_ms, bool by_ai)
{
    gate_result_t result;

    if (by_ai) {
        gate->has_ai = true;
        gate->ai_ms = now_ms;
    }
    if (word == WORD_STOP) {
        gate->has_wait = false;
        result = gate_stop(gate, now_ms);
    } else if (gate->has_wait &&
               (bento_time_ticks_diff(now_ms, gate->wait_ms) <=
                GO_CONFIRM_MS)) {
        gate->has_wait = false;
        gate->has_ok = true;
        gate->ok_ms = now_ms;
        result = gate_go(gate, now_ms);
    } else {
        gate->has_wait = true;
        gate->wait_ms = now_ms;
        result = gate_result(WORD_GO, -1, WHY_WAIT, false);
    }
    return result;
}

/* SW6 confirms the go that waits; none waits: no result. */
static gate_result_t gate_confirm(gate_t *gate, uint32_t now_ms)
{
    gate_result_t result = gate_result(WORD_NONE, 0, WHY_DONE, false);

    result.has = false;
    if (gate->has_wait) {
        gate->has_wait = false;
        gate->has_ok = true;
        gate->ok_ms = now_ms;
        result = gate_go(gate, now_ms);
    }
    return result;
}

static gate_result_t gate_expire(gate_t *gate, uint32_t now_ms)
{
    gate_result_t result = gate_result(WORD_NONE, 0, WHY_DONE, false);

    result.has = false;
    if (gate->has_wait &&
        (bento_time_ticks_diff(now_ms, gate->wait_ms) >
         GO_CONFIRM_MS)) {
        gate->has_wait = false;
        result = gate_result(WORD_GO, 0, WHY_UNCONFIRMED, false);
    }
    return result;
}

/* Happened within GO_CONFIRM_MS (1/0). */
static int32_t gate_recent(bool has, uint32_t at_ms, uint32_t now_ms)
{
    return (has &&
            (bento_time_ticks_diff(now_ms, at_ms) < GO_CONFIRM_MS))
               ? 1
               : 0;
}

/* ---- the brain ---- */

/* The 2x2 table: both = level 2, one side = level 1, nobody = 0. */
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

/* Far enough from the last one, and something changed or the
 * heartbeat is due (sf3_06). */
static bool should_send(bool changed, int32_t since_ms)
{
    return (since_ms >= GAP_MS) &&
           (changed || (since_ms >= HEARTBEAT_MS));
}

/* ---- MQTT ---- */

/* team01 style: "team", two digits, not team00; anything else would
 * publish over another team's topics. */
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
            status = nap(WIFI_SLICE_MS);
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
    /* WIFI_PASS: at least 8 characters; never share a file with it */
    static const char wifi_pass[] = "<รหัส Hotspot ของกลุ่ม>";
    bento_wifi_connect_params_t params;
    uint32_t address = 0U;
    bento_status_t status = BENTO_OK;

    *up = wifi_connected();
    if (!*up) {
        bento_wifi_connect_params_init(&params);
        params.ssid = WIFI_SSID;
        params.ssid_len = (uint32_t)strlen(WIFI_SSID);
        params.pass = wifi_pass;
        params.pass_len = (uint32_t)strlen(wifi_pass);
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
        status = nap(MQTT_SLICE_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(broker);
        }
    }
    return status;
}

static void close_broker(state_t *state)
{
    if (state->has_broker) {
        /* the .py ignores an OSError from disconnect() */
        (void)bento_mqtt_close(state->broker);
        state->has_broker = false;
    }
}

/* One try, with a new client id, so an old id never collides. */
static bento_status_t open_broker(state_t *state, bool *linked)
{
    /* BROKER; the spare is test.mosquitto.org (change the app too) */
    static const char broker[] = "broker.hivemq.com";
    bento_mqtt_config_t config;
    char client_id[NAME_BYTES];
    bento_status_t status;

    (void)snprintf(
        client_id, sizeof(client_id), "bento-ai-%s-%04" PRIx32, TEAM,
        bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK);
    bento_mqtt_config_init(&config);
    config.host = broker;
    config.host_len = (uint32_t)strlen(broker);
    config.port = (uint16_t)PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    status = bento_mqtt_open_start(&config, &state->broker);
    state->has_broker = status == BENTO_OK;
    if (state->has_broker) {
        status = wait_broker(state->broker);
    }
    *linked = status == BENTO_OK;
    if (!*linked) {
        close_broker(state);
    }
    return (status == BENTO_E_CANCELLED) ? status : BENTO_OK;
}

/* A public broker sometimes does not answer: three tries (sf3_06). */
static bento_status_t connect_broker(farm_t *farm, bool *linked)
{
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;
    int32_t attempt;

    *linked = false;
    for (attempt = 1;
         (attempt <= BROKER_TRIES) && (status == BENTO_OK) && !*linked;
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(text, sizeof(text),
                           "ลองต่อ broker ใหม่ %" PRId32 "/3", attempt);
            note(farm, text, COLOR_WARN);
            poll_display(farm->ui);
        }
        status = open_broker(&farm->state, linked);
    }
    return status;
}

/* The broker is up: listen to ai/cmd. Refused = send only. */
static void listen_commands(farm_t *farm)
{
    char topic[NAME_BYTES];
    char text[TEXT_BYTES];
    uint8_t granted_qos = 0U;
    bool ok;

    (void)snprintf(topic, sizeof(topic), "%sai/cmd", farm->state.base);
    ok = bento_mqtt_subscribe(farm->state.broker, topic,
                              (uint32_t)strlen(topic), 0U,
                              &granted_qos) == BENTO_OK;
    (void)snprintf(text, sizeof(text), "%s%sai",
                   ok ? "ออนไลน์ " : "ส่งได้แต่รับคำสั่งไม่ได้ ",
                   farm->state.base);
    note(farm, text, ok ? COLOR_OK : COLOR_WARN);
}

/* Wi-Fi, the broker, then ai/cmd. A step that fails leaves the run
 * offline (the console instead). Only Stop is an error. */
static bento_status_t go_online(farm_t *farm)
{
    char text[TEXT_BYTES];
    bool up = false;
    bool linked = false;
    bento_status_t status = BENTO_OK;

    if (!team_ok()) {
        note(farm, "แก้ TEAM ก่อน (team01-team99): ออฟไลน์", COLOR_WARN);
    } else if (ssid_unset() && !wifi_connected()) {
        note(farm, "ตั้งชื่อ Hotspot ก่อน: ออฟไลน์", COLOR_WARN);
    } else {
        note(farm, "ต่อ WiFi...", COLOR_WARN);
        poll_display(farm->ui); /* the label is up before the wait */
        status = wifi_up(&up);
        if ((status == BENTO_OK) && up) {
            status = connect_broker(farm, &linked);
        }
        if ((status == BENTO_OK) && linked) {
            listen_commands(farm);
        } else if (status == BENTO_OK) {
            (void)snprintf(text, sizeof(text),
                           "ต่อ %s ไม่ได้: ออฟไลน์ (ทำงานต่อ)",
                           up ? "broker" : "WiFi");
            note(farm, text, COLOR_WARN);
        } else {
            /* Stop ends the run */
        }
    }
    farm->state.online = linked;
    return status;
}

static bool broker_alive(const state_t *state)
{
    uint32_t mqtt_state = BENTO_MQTT_STATE_CLOSED;

    return (bento_mqtt_state(state->broker, &mqtt_state) == BENTO_OK) &&
           (mqtt_state == BENTO_MQTT_STATE_CONNECTED);
}

/* JSON (QoS 0) to the base topic + suffix; offline it is only
 * printed. True when it reached the broker. */
static bool send(const state_t *state, const char *suffix,
                 const char *line)
{
    char topic[NAME_BYTES];
    bool delivered = false;

    (void)printf("%s\n", line);
    if (state->online) {
        (void)snprintf(topic, sizeof(topic), "%s%s", state->base,
                       suffix);
        delivered = bento_mqtt_publish(
                        state->broker, topic, (uint32_t)strlen(topic),
                        (const uint8_t *)line, (uint32_t)strlen(line),
                        0U, 0U) == BENTO_OK;
    }
    return delivered;
}

/* An event: id, event and m, then the keys in tail (no n: an event
 * is sent once, when it happens). */
static void send_event(farm_t *farm, const char *name, const char *tail)
{
    (void)snprintf(
        farm->payload, sizeof(farm->payload),
        "{\"id\": \"%s\", \"event\": \"%s\", \"m\": \"%s\", %s}", TEAM,
        name, MISSION, tail);
    (void)send(&farm->state, "event", farm->payload);
}

/* "" = paused (item 3.10); rule-only = no model of the mission. */
static const char *model_name(const farm_t *farm)
{
    const ai_t *ai = &farm->ai;

    return (farm->state.paused || ((ai->index >= 0) && !ai->running))
               ? ""
               : ai->name;
}

/* The state of item 3.11, without p2/c2 and ms when drop says so. */
static void write_status(farm_t *farm, const char *model, int32_t drop)
{
    static const char *const rule_keys[] = {"vib", "loud", "rate",
                                            "dur", "btn",  ""};
    const ai_t *ai = &farm->ai;
    const state_t *state = &farm->state;
    char second[TAIL_BYTES];
    char latency[TAIL_BYTES];
    char value[BENTO_FMT_BUF_SIZE];
    char limit[BENTO_FMT_BUF_SIZE];

    (void)snprintf(second, sizeof(second),
                   "\"p2\": \"%s\", \"c2\": %" PRId32 ", ", ai->second,
                   ai->second_conf);
    (void)snprintf(latency, sizeof(latency), "\"ms\": %" PRId32 ", ",
                   ai->latency_ms);
    format_number(value, sizeof(value), state->judged.value);
    format_number(limit, sizeof(limit), state->judged.limit);
    (void)snprintf(
        farm->payload, sizeof(farm->payload),
        "{\"id\": \"%s\", \"n\": %" PRId32
        ", \"m\": \"%s\", \"model\": "
        "\"%s\", \"label\": \"%s\", \"conf\": %" PRId32 ", %s%s\"rk\": "
        "\"%s\", \"rule\": %" PRId32 ", \"rv\": %s, \"rl\": %s, "
        "\"agree\": \"%s\", \"lvl\": %" PRId32 ", \"alarm\": %d}",
        TEAM, state->sent, MISSION, model, ai->label, ai->conf,
        (drop >= 1) ? "" : second, (drop >= 2) ? "" : latency,
        rule_keys[state->judged.kind], state->judged.rule, value, limit,
        AGREE_KEYS[state->agree], state->level,
        state->alarm.on ? 1 : 0);
}

/* At most MAX_BYTES, measured on the text itself: p2/c2 go first,
 * then ms, then the model's name loses letters from its end. */
static void fit_status(farm_t *farm)
{
    const uint8_t top_bits = 0xC0U;
    const uint8_t continuation = 0x80U;
    char model[BENTO_EDGEAI_NAME_LEN];
    size_t length;
    int32_t drop = 0;

    (void)snprintf(model, sizeof(model), "%s", model_name(farm));
    write_status(farm, model, drop);
    while ((strlen(farm->payload) > (size_t)MAX_BYTES) && (drop < 2)) {
        drop++;
        write_status(farm, model, drop);
    }
    length = strlen(model);
    while ((strlen(farm->payload) > (size_t)MAX_BYTES) &&
           (length > 0U)) {
        length--;
        while ((length > 0U) &&
               (((uint8_t)model[length] & top_bits) == continuation)) {
            length--; /* one whole character */
        }
        model[length] = '\0';
        write_status(farm, model, drop);
    }
}

/* Every HEARTBEAT_MS, and at once when label, level or alarm
 * change, never closer than GAP_MS. The clock is read when sending:
 * a tone in this pass may take half a second. */
static void report(farm_t *farm)
{
    state_t *state = &farm->state;
    const uint32_t now_ms = bento_time_ticks_ms();
    const bool changed =
        !state->has_key || (state->key_level != state->level) ||
        (state->key_alarm != state->alarm.on) ||
        (strcmp(state->key_label, farm->ai.label) != 0);
    char text[TEXT_BYTES];
    bool delivered;

    if (should_send(state->force || changed,
                    bento_time_ticks_diff(now_ms, state->sent_ms))) {
        state->sent++;
        fit_status(farm);
        delivered = send(state, "ai", farm->payload);
        state->sent_ms = now_ms;
        state->has_key = true;
        (void)snprintf(state->key_label, sizeof(state->key_label), "%s",
                       farm->ai.label);
        state->key_level = state->level;
        state->key_alarm = state->alarm.on;
        state->force = false;
        if (delivered) {
            (void)snprintf(text, sizeof(text), "MQTT: ส่งแล้ว %" PRId32,
                           state->sent);
        } else {
            (void)snprintf(text, sizeof(text),
                           "ออฟไลน์: Console %" PRId32, state->sent);
        }
        put_text(farm, SLOT_MQTT_TEXT, text);
    }
}

/* ---- commands from the app (ai/cmd) ---- */

typedef enum {
    COMMAND_NONE = 0,
    COMMAND_ACKNOWLEDGE,
    COMMAND_STOP,
    COMMAND_RUN,
    COMMAND_SELECT
} command_kind_t;

/* What the field finder took from one ai/cmd: the top-level "cmd"
 * and "model" when they are strings. */
typedef struct {
    bool has_command;
    char command[NAME_BYTES];
    bool has_model;
    char model[BENTO_EDGEAI_NAME_LEN];
} command_t;

typedef struct {
    const uint8_t *text;
    uint32_t length;
    uint32_t at;
} cursor_t;

static void skip_space(cursor_t *cursor)
{
    while ((cursor->at < cursor->length) &&
           ((cursor->text[cursor->at] == (uint8_t)' ') ||
            (cursor->text[cursor->at] == (uint8_t)'\t') ||
            (cursor->text[cursor->at] == (uint8_t)'\n') ||
            (cursor->text[cursor->at] == (uint8_t)'\r'))) {
        cursor->at++;
    }
}

static bool next_is(const cursor_t *cursor, char letter)
{
    return (cursor->at < cursor->length) &&
           (cursor->text[cursor->at] == (uint8_t)letter);
}

/* The next byte of a string: an escape stands for the letter after
 * the backslash. */
static uint8_t string_byte(cursor_t *cursor, uint8_t byte)
{
    uint8_t taken = byte;

    if ((byte == (uint8_t)'\\') && (cursor->at < cursor->length)) {
        taken = cursor->text[cursor->at];
        cursor->at++;
    }
    return taken;
}

/* A string at the cursor, cut to fit out. False when it never
 * closes. */
static bool read_string(cursor_t *cursor, char *out, uint32_t capacity)
{
    uint32_t used = 0U;
    bool closed = false;

    cursor->at++; /* the opening quote */
    while ((cursor->at < cursor->length) && !closed) {
        const uint8_t byte = cursor->text[cursor->at];

        cursor->at++;
        closed = byte == (uint8_t)'"';
        if (!closed && ((used + 1U) < capacity)) {
            out[used] = (char)string_byte(cursor, byte);
            used++;
        } else if (!closed) {
            (void)string_byte(cursor, byte); /* past what fits */
        } else {
            /* the closing quote */
        }
    }
    out[used] = '\0';
    return closed;
}
/* +1 for an opening bracket, -1 for a closing one. */
static int32_t depth_change(uint8_t byte)
{
    int32_t change = 0;

    if ((byte == (uint8_t)'{') || (byte == (uint8_t)'[')) {
        change = 1;
    } else if ((byte == (uint8_t)'}') || (byte == (uint8_t)']')) {
        change = -1;
    } else {
        /* any other byte */
    }
    return change;
}

/* Skips one value that is not kept: up to the ',' or '}' that ends
 * it, strings and nested brackets included. */
static bool skip_value(cursor_t *cursor)
{
    char ignored[2];
    int32_t depth = 0;
    bool ok = true;
    bool done = false;

    while (ok && !done && (cursor->at < cursor->length)) {
        const uint8_t byte = cursor->text[cursor->at];

        done = (depth == 0) &&
               ((byte == (uint8_t)',') || (byte == (uint8_t)'}'));
        if (byte == (uint8_t)'"') {
            ok = read_string(cursor, ignored, sizeof(ignored));
        } else if (!done) {
            depth += depth_change(byte);
            cursor->at++;
        } else {
            /* the end of the value */
        }
    }
    return ok && done && (depth == 0);
}
/* "key": value. A later equal key wins, as in a dict. */
static bool read_member(cursor_t *cursor, command_t *command)
{
    char key[NAME_BYTES];
    bool ok =
        next_is(cursor, '"') && read_string(cursor, key, sizeof(key));
    bool *has = NULL;
    char *out = NULL;
    uint32_t capacity = 0U;

    skip_space(cursor);
    ok = ok && next_is(cursor, ':');
    if (ok) {
        cursor->at++;
        skip_space(cursor);
        if (strcmp(key, "cmd") == 0) {
            has = &command->has_command;
            out = command->command;
            capacity = sizeof(command->command);
        } else if (strcmp(key, "model") == 0) {
            has = &command->has_model;
            out = command->model;
            capacity = sizeof(command->model);
        } else {
            /* a key the program does not read */
        }
    }
    if (ok && (has != NULL) && next_is(cursor, '"')) {
        *has = read_string(cursor, out, capacity);
        ok = *has;
    } else if (ok) {
        if (has != NULL) {
            *has = false; /* not a string */
        }
        ok = skip_value(cursor);
    } else {
        /* not a member */
    }
    return ok;
}

/* json.loads as far as the program asks: an object, its "cmd" and
 * its "model". False for anything that is not an object. */
static bool read_command(const uint8_t *payload, uint32_t length,
                         command_t *command)
{
    cursor_t cursor = {payload, length, 0U};
    bool ok;
    bool closed = false;

    (void)memset(command, 0, sizeof(*command));
    skip_space(&cursor);
    ok = next_is(&cursor, '{');
    cursor.at++;
    while (ok && !closed) {
        skip_space(&cursor);
        closed = next_is(&cursor, '}');
        if (!closed) {
            ok = read_member(&cursor, command);
            skip_space(&cursor);
            closed = ok && next_is(&cursor, '}');
            ok = ok && (closed || next_is(&cursor, ','));
        }
        cursor.at++;
    }
    skip_space(&cursor);
    return ok && (cursor.at == length);
}

/* ack, stop, run, or select with a model name; else nothing. */
static command_kind_t command_kind(const command_t *command)
{
    command_kind_t kind = COMMAND_NONE;

    if (!command->has_command) {
        kind = COMMAND_NONE;
    } else if (strcmp(command->command, "ack") == 0) {
        kind = COMMAND_ACKNOWLEDGE;
    } else if (strcmp(command->command, "stop") == 0) {
        kind = COMMAND_STOP;
    } else if (strcmp(command->command, "run") == 0) {
        kind = COMMAND_RUN;
    } else if ((strcmp(command->command, "select") == 0) &&
               command->has_model) {
        kind = COMMAND_SELECT;
    } else {
        /* unknown: ignored */
    }
    return kind;
}

/* ---- the screen ---- */

/* One widget of the screen's table. text NULL = the mission's hint;
 * a panel takes the card colour with a dim border. */
typedef struct {
    kind_t kind;
    slot_t slot;
    const char *text;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint32_t color;
    int32_t value; /* a label's font size, else its first value */
} part_t;

static const char *hint_text(const farm_t *farm)
{
    const char *hint = "SW6 = รับทราบการเตือน | SW5 = พัก/ทำต่อ";

    if (farm->id == MISSION_VOICE) {
        hint = "SW5 = แทนเสียง go | SW6 = ยืนยัน go / stop";
    } else if (farm->id == MISSION_WORKER) {
        hint = "SW6 = ฉันโอเค / รับทราบ | SW5 = พัก";
    } else {
        /* the common hint */
    }
    return hint;
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                const part_t *part, bento_ui_id_t *made)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = part->x;
    args.y = part->y;
    args.color = part->color;
    args.font_px = part->value;
    return bento_ui_label_create(ui, &args, made);
}

static bento_status_t add_shape(bento_ui_t *ui, const part_t *part,
                                bento_ui_id_t *made)
{
    const int32_t card_radius = 12;
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = part->x;
    args.y = part->y;
    args.w = part->w;
    args.h = part->h;
    args.color = part->color;
    args.font_px = part->value;
    if (part->kind == KIND_PANEL) {
        args.min_val = COLOR_DIM;
        args.max_val = card_radius;
        status = bento_ui_panel_create(ui, &args, made);
    } else if (part->kind == KIND_LED) {
        status = bento_ui_led_create(ui, &args, made);
    } else {
        args.max_val = PERCENT;
        status = (part->kind == KIND_ARC)
                     ? bento_ui_arc_create(ui, &args, made)
                     : bento_ui_bar_create(ui, &args, made);
    }
    return status;
}

static bento_status_t add_part(farm_t *farm, const part_t *part)
{
    bento_ui_id_t made = 0U;
    bento_status_t status;

    if (part->kind == KIND_LABEL) {
        status = add_label(farm->ui,
                           (part->text != NULL) ? part->text
                                                : hint_text(farm),
                           part, &made);
    } else {
        status = add_shape(farm->ui, part, &made);
    }
    if (part->slot != SLOT_NONE) {
        farm->ids[part->slot] = made;
    }
    return status;
}

static bento_status_t add_parts(farm_t *farm, const part_t *parts,
                                uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_part(farm, &parts[i]);
    }
    return status;
}

/* The MQTT light and the two upper cards: the AI and the rule. */
static bento_status_t add_upper_cards(farm_t *farm)
{
    static const part_t parts[] = {
        {KIND_LED, SLOT_MQTT, "", 606, 12, 18, 18, COLOR_OK, 0},
        {KIND_LABEL, SLOT_MQTT_TEXT, "MQTT: ออฟไลน์", 632, 10, 0, 0,
         COLOR_DIM, 16},
        {KIND_PANEL, SLOT_NONE, "", 12, 64, 380, 172, COLOR_CARD, 1},
        {KIND_LABEL, SLOT_NONE, "AI ตอบว่า", 24, 70, 0, 0, COLOR_INFO,
         16},
        {KIND_LABEL, SLOT_MODEL, "กำลังหาโมเดล...", 24, 88, 0, 0,
         COLOR_DIM, 14},
        {KIND_ARC, SLOT_ARC, "", 300, 92, 80, 80, 0U, 0},
        {KIND_LABEL, SLOT_LABEL, "-", 24, 108, 0, 0, COLOR_TEXT, 28},
        {KIND_LABEL, SLOT_MEANING, " ", 24, 146, 0, 0, COLOR_INFO, 16},
        {KIND_LABEL, SLOT_CONF, " ", 24, 176, 0, 0, COLOR_DIM, 14},
        {KIND_LABEL, SLOT_AI, " ", 24, 204, 0, 0, COLOR_DIM, 16},
        {KIND_PANEL, SLOT_NONE, "", 402, 64, 378, 172, COLOR_CARD, 1},
        {KIND_LABEL, SLOT_NONE, "กฎที่เขียนเอง", 414, 70, 0, 0, COLOR_INFO,
         16},
        {KIND_LABEL, SLOT_RULE_DESC, " ", 414, 88, 0, 0, COLOR_DIM, 14},
        {KIND_LABEL, SLOT_RULE_VALUE, "-", 414, 108, 0, 0, COLOR_TEXT,
         28},
        {KIND_BAR, SLOT_RULE_BAR, "", 414, 148, 354, 14, 0U, 0},
        {KIND_LABEL, SLOT_RULE_STATE, " ", 414, 174, 0, 0, COLOR_DIM,
         16},
        {KIND_LABEL, SLOT_RULE_NOTE, " ", 414, 204, 0, 0, COLOR_DIM,
         14}};
    return add_parts(farm, parts,
                     (uint32_t)(sizeof(parts) / sizeof(parts[0])));
}

/* The two lower cards: the 2x2 table and the level, then the note. */
static bento_status_t add_lower_cards(farm_t *farm)
{
    static const part_t parts[] = {
        {KIND_PANEL, SLOT_NONE, "", 12, 244, 380, 100, COLOR_CARD, 1},
        {KIND_LABEL, SLOT_NONE, "AI กับกฎ เห็นตรงกันไหม", 24, 250, 0, 0,
         COLOR_INFO, 16},
        {KIND_LABEL, SLOT_NONE, "กฎ: ใช่", 130, 266, 0, 0, COLOR_DIM,
         14},
        {KIND_LABEL, SLOT_NONE, "กฎ: ไม่", 270, 266, 0, 0, COLOR_DIM,
         14},
        {KIND_LABEL, SLOT_NONE, "AI: ใช่", 24, 290, 0, 0, COLOR_DIM, 14},
        {KIND_LABEL, SLOT_NONE, "AI: ไม่", 24, 316, 0, 0, COLOR_DIM, 14},
        {KIND_LED, SLOT_CELL_BOTH, "", 130, 288, 18, 18, COLOR_BAD, 0},
        {KIND_LABEL, SLOT_NONE, "both", 156, 288, 0, 0, COLOR_TEXT, 16},
        {KIND_LED, SLOT_CELL_AI, "", 270, 288, 18, 18, COLOR_WARN, 0},
        {KIND_LABEL, SLOT_NONE, "ai", 296, 288, 0, 0, COLOR_TEXT, 16},
        {KIND_LED, SLOT_CELL_RULE, "", 130, 314, 18, 18, COLOR_WARN, 0},
        {KIND_LABEL, SLOT_NONE, "rule", 156, 314, 0, 0, COLOR_TEXT, 16},
        {KIND_LED, SLOT_CELL_NONE, "", 270, 314, 18, 18, COLOR_OK, 0},
        {KIND_LABEL, SLOT_NONE, "none", 296, 314, 0, 0, COLOR_TEXT, 16},
        {KIND_PANEL, SLOT_NONE, "", 402, 244, 378, 100, COLOR_CARD, 1},
        {KIND_LABEL, SLOT_NONE, "ระดับและการเตือน", 414, 250, 0, 0,
         COLOR_INFO, 16},
        {KIND_LED, SLOT_LEVEL_LED, "", 414, 274, 34, 34, COLOR_OK, 1},
        {KIND_LABEL, SLOT_LEVEL, " ", 460, 270, 0, 0, COLOR_TEXT, 20},
        {KIND_LABEL, SLOT_ALARM, " ", 460, 298, 0, 0, COLOR_DIM, 16},
        {KIND_LABEL, SLOT_NONE, NULL, 414, 322, 0, 0, COLOR_DIM, 14},
        {KIND_LABEL, SLOT_NOTE, "กำลังเริ่ม...", 12, 352, 0, 0, COLOR_WARN,
         16}};
    return add_parts(farm, parts,
                     (uint32_t)(sizeof(parts) / sizeof(parts[0])));
}

/* Everything below the two title lines, in the .py's order. */
static bento_status_t add_cards(farm_t *farm)
{
    bento_status_t status = add_upper_cards(farm);

    if (status == BENTO_OK) {
        status = add_lower_cards(farm);
    }
    return status;
}

static bento_status_t build_screen(farm_t *farm)
{
    static const part_t title = {KIND_LABEL, SLOT_NONE,  "", 12, 6, 0,
                                 0,          COLOR_TEXT, 24};
    static const part_t problem = {KIND_LABEL, SLOT_NONE, "", 12, 38, 0,
                                   0,          COLOR_DIM, 16};
    char text[TEXT_BYTES];
    bento_ui_id_t made = 0U;
    bento_status_t status = bento_ui_screen_default(farm->ui);

    if (status == BENTO_OK) {
        status = nap(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "ภารกิจ AI: %s",
                       farm->mission->thai);
        status = add_label(farm->ui, text, &title, &made);
    }
    if (status == BENTO_OK) {
        (void)snprintf(text, sizeof(text), "%s | %s", MISSION,
                       farm->mission->problem);
        status = add_label(farm->ui, text, &problem, &made);
    }
    if (status == BENTO_OK) {
        status = add_cards(farm);
    }
    if (status == BENTO_OK) {
        poll_display(farm->ui);
    }
    return status;
}

/* The MQTT light: green = on the broker, the app sees the messages;
 * off = offline, the messages go to the console. */
static void show_link(farm_t *farm)
{
    const bool online = farm->state.online;

    put_value(farm, SLOT_MQTT, online ? 1 : 0);
    put(farm, SLOT_MQTT_TEXT,
        online ? "MQTT: เชื่อมต่อแล้ว" : "MQTT: ออฟไลน์",
        online ? COLOR_OK : COLOR_BAD);
}

static void show_model(farm_t *farm)
{
    char text[TEXT_BYTES];

    if (farm->ai.index < 0) {
        (void)snprintf(text, sizeof(text),
                       "โมเดล: %s (บอร์ดไม่มีโมเดลของภารกิจ)", RULE_ONLY);
        put(farm, SLOT_MODEL, text, COLOR_WARN);
    } else {
        (void)snprintf(text, sizeof(text), "โมเดล: %s%s", farm->ai.name,
                       farm->ai.mic ? "  (ฟังไมค์)" : "");
        put(farm, SLOT_MODEL, text, COLOR_DIM);
    }
}

/* The model's label in farm words (the label must match the board's). */
static const char *meaning_of(const char *label)
{
    static const char *const meanings[][2] = {
        {"-", "ยังไม่มีผล"},
        {"unlabeled", "ปกติ / ไม่มีเหตุ"},
        {"unlabelled", "ปกติ / ไม่มีเหตุ"},
        {"anomaly", "สั่นผิดปกติ!"},
        {"idle", "นิ่ง"},
        {"circle", "หมุนเป็นจังหวะ"},
        {"shaking", "สั่นแรงผิดปกติ!"},
        {"sirens", "เสียงไซเรน!"},
        {"cough", "เสียงไอ"},
        {"water_tap", "ก๊อกน้ำเปิดอยู่"},
        {"baby_cry", "เสียงร้องแบบเด็ก"},
        {"alarm", "เสียงสัญญาณเตือน"},
        {"chainsaw", "เลื่อยยนต์!"},
        {"crackling_fire", "ไฟไหม้!"},
        {"dog", "สุนัขเห่า"},
        {"rain", "ฝนตก"},
        {"background", "เสียงพื้นหลัง"},
        {"crying_baby", "เด็กร้อง"},
        {"sneezing", "จาม"},
        {"helicopter", "เฮลิคอปเตอร์"},
        {"go", "go = เปิดปั๊ม"},
        {"stop", "stop = ปิดปั๊ม"},
        {"left", "ซ้าย"},
        {"right", "ขวา"},
        {"up", "ขึ้น"},
        {"down", "ลง"},
        {"sitting", "นั่งนิ่ง"},
        {"standing", "ยืนนิ่ง"},
        {"walking", "เดิน"},
        {"running", "วิ่ง"},
        {"jumping", "กระโดด"},
        {"air", "สว่านหมุนลอย"},
        {"wood", "กำลังเจาะไม้"},
        {"plastic", "กำลังเจาะพลาสติก"},
        {"wood_out", "ทะลุไม้แล้ว!"},
        {"plastic_out", "ทะลุพลาสติกแล้ว!"}};
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

static void draw_ai_status(farm_t *farm)
{
    const ai_t *ai = &farm->ai;
    char text[TEXT_BYTES];

    if (farm->state.paused) {
        put(farm, SLOT_AI, "พักอยู่: กด SW5 ทำต่อ", COLOR_WARN);
    } else if (ai->index < 0) {
        put(farm, SLOT_AI, farm->mission->no_model, COLOR_WARN);
    } else if (!ai->running) {
        put(farm, SLOT_AI, "หยุดโมเดลอยู่ (แอปส่ง run)", COLOR_WARN);
    } else if (ai->stalled) {
        put(farm, SLOT_AI, "AI เงียบ / ไม่มีผล", COLOR_BAD);
    } else if (!ai->has_sequence) {
        put(farm, SLOT_AI, "รอผลแรกจาก AI...", COLOR_DIM);
    } else {
        (void)snprintf(text, sizeof(text),
                       "AI: %s  (ป้ายเหตุ %" PRIu32 "/%" PRIu32 ")",
                       ai->history.on ? "เกิดเหตุ" : "ปกติ",
                       window_hits(&ai->history), ai->history.count);
        put(farm, SLOT_AI, text, ai->history.on ? COLOR_BAD : COLOR_OK);
    }
}

static void draw_ai(farm_t *farm)
{
    const ai_t *ai = &farm->ai;
    char text[TEXT_BYTES];

    put_text(farm, SLOT_LABEL, ai->label);
    put(farm, SLOT_MEANING, meaning_of(ai->label),
        ai->history.on ? COLOR_BAD : COLOR_INFO);
    if (ai->running) {
        (void)snprintf(text, sizeof(text),
                       "มั่นใจ %" PRId32 " %% | อันดับสอง %s %" PRId32
                       " %%",
                       ai->conf, ai->second, ai->second_conf);
    } else {
        (void)snprintf(text, sizeof(text), " ");
    }
    put_text(farm, SLOT_CONF, text);
    put_value(farm, SLOT_ARC, ai->conf);
    put_color(farm, SLOT_ARC,
              (ai->conf >= CONF_MIN) ? COLOR_OK : COLOR_WARN);
    draw_ai_status(farm);
}

/* The rule in plain words. */
static void rule_text(const farm_t *farm, char *text, uint32_t capacity)
{
    const rule_kind_t kind = farm->state.judged.kind;
    char number[BENTO_FMT_BUF_SIZE];

    if (kind == RULE_VIB) {
        const number_t k_vib = {true, (bento_real_t)K_VIB_TENTHS /
                                          (bento_real_t)TENTHS};

        format_short(number, sizeof(number), k_vib);
        (void)snprintf(text, capacity, "กฎ: ค่าสั่น > ค่าปกติ x %s", number);
    } else if (kind == RULE_LOUD) {
        (void)snprintf(text, capacity,
                       "กฎ: ดัง >= %d ต่อเนื่อง %" PRId32 " วิ", LOUD_LV,
                       farm->mission->loud_s);
    } else if (kind == RULE_RATE) {
        (void)snprintf(text, capacity,
                       "กฎ: ไอ >= %d ครั้งใน %" PRId32 " นาที", COUGH_MAX,
                       (int32_t)RATE_MS / (int32_t)MS_PER_MINUTE);
    } else if ((kind == RULE_DURATION) && (farm->id == MISSION_BARN)) {
        (void)snprintf(text, capacity, "กฎ: ก๊อกเปิดต่อเนื่อง >= %d วิ",
                       TAP_S);
    } else if (kind == RULE_DURATION) {
        (void)snprintf(text, capacity, "กฎ: ไม่ขยับ >= %d วิ แล้วถาม",
                       STILL_S);
    } else {
        (void)snprintf(text, capacity,
                       "กฎ: SW6 ยืนยัน go ภายใน %" PRId32 " วิ",
                       (int32_t)GO_CONFIRM_MS / (int32_t)MS_PER_S);
    }
}

/* The rule's value against its limit, with its unit. */
static void value_text(const judged_t *judged, char *text,
                       uint32_t capacity)
{
    char value[BENTO_FMT_BUF_SIZE];
    char limit[BENTO_FMT_BUF_SIZE];

    if (judged->kind == RULE_VIB) {
        format_fixed(value, sizeof(value), judged->value.value, 2);
        if (judged->limit.value != (bento_real_t)0) {
            format_fixed(limit, sizeof(limit), judged->limit.value, 2);
        } else {
            (void)snprintf(limit, sizeof(limit), "-");
        }
        (void)snprintf(text, capacity, "%s / %s m/s^2", value, limit);
    } else if ((judged->kind == RULE_LOUD) ||
               (judged->kind == RULE_DURATION)) {
        format_fixed(value, sizeof(value), judged->value.value, 1);
        format_short(limit, sizeof(limit), judged->limit);
        (void)snprintf(text, capacity, "%s / %s วิ", value, limit);
    } else if (judged->kind == RULE_RATE) {
        (void)snprintf(text, capacity, "%" PRId32 " / %" PRId32 " ครั้ง",
                       (int32_t)judged->value.value,
                       (int32_t)judged->limit.value);
    } else {
        (void)snprintf(text, capacity, "ยืนยัน %" PRId32 " / 1",
                       (int32_t)judged->value.value);
    }
}

static void rule_state(farm_t *farm, uint32_t now_ms)
{
    const state_t *state = &farm->state;
    const int32_t left_ms = rules_learning(&farm->rules, now_ms);
    char text[TEXT_BYTES];

    if (state->paused) {
        put(farm, SLOT_RULE_STATE, "พัก", COLOR_WARN);
    } else if ((farm->id != MISSION_VOICE) &&
               rules_broken(&farm->rules)) {
        put(farm, SLOT_RULE_STATE, "กฎ: อ่านค่าไม่ได้ (rule = 0)",
            COLOR_BAD);
    } else if (left_ms != 0) {
        (void)snprintf(text, sizeof(text),
                       "วางนิ่ง เรียนค่าปกติ อีก %" PRId32 " วิ",
                       seconds_left(left_ms));
        put(farm, SLOT_RULE_STATE, text, COLOR_WARN);
    } else if ((farm->id == MISSION_VOICE) && state->gate.has_wait) {
        (void)snprintf(text, sizeof(text),
                       "ได้ยิน go: รอ SW6 / go ซ้ำ %" PRId32 " วิ",
                       seconds_left(GO_CONFIRM_MS -
                                    bento_time_ticks_diff(
                                        now_ms, state->gate.wait_ms)));
        put(farm, SLOT_RULE_STATE, text, COLOR_WARN);
    } else {
        put(farm, SLOT_RULE_STATE,
            (state->judged.rule != 0) ? "กฎ: เกิดเหตุ" : "กฎ: ปกติ",
            (state->judged.rule != 0) ? COLOR_BAD : COLOR_OK);
    }
}

static void rule_note(farm_t *farm)
{
    const rules_t *rules = &farm->rules;
    const bool shows = farm->id != MISSION_VOICE; /* voice: blank */
    char text[TEXT_BYTES];
    char number[BENTO_FMT_BUF_SIZE];

    if (shows && rules_broken(rules)) {
        (void)snprintf(text, sizeof(text),
                       "กฎอ่านค่าไม่ได้ระหว่างโมเดลรัน (%" PRId32 ")",
                       rules->fails);
        put(farm, SLOT_RULE_NOTE, text, COLOR_BAD);
    } else if (shows && (rules->fails != 0)) {
        (void)snprintf(text, sizeof(text), "อ่านพลาด %" PRId32 " ครั้ง",
                       rules->fails);
        put(farm, SLOT_RULE_NOTE, text, COLOR_WARN);
    } else if (shows && (rules->kind == RULE_VIB) &&
               rules->vib.has_base) {
        format_fixed(number, sizeof(number), rules->vib.base,
                     BASE_DIGITS);
        (void)snprintf(text, sizeof(text), "ค่าปกติที่เรียนมา %s", number);
        put(farm, SLOT_RULE_NOTE, text, COLOR_DIM);
    } else {
        put(farm, SLOT_RULE_NOTE, " ", COLOR_DIM);
    }
}

static void draw_rule(farm_t *farm, uint32_t now_ms)
{
    const judged_t *judged = &farm->state.judged;
    const bool on = judged->rule != 0;
    char text[TEXT_BYTES];
    int32_t bar = 0;

    rule_text(farm, text, sizeof(text));
    put_text(farm, SLOT_RULE_DESC, text);
    value_text(judged, text, sizeof(text));
    put(farm, SLOT_RULE_VALUE, text, on ? COLOR_BAD : COLOR_TEXT);
    if (judged->limit.value != (bento_real_t)0) {
        const bento_real_t scaled =
            judged->value.value * (bento_real_t)PERCENT;
        const bento_real_t ratio = scaled / judged->limit.value;
        const int32_t share = (int32_t)ratio;

        bar = (share < PERCENT) ? share : PERCENT;
    }
    put_value(farm, SLOT_RULE_BAR, bar);
    put_color(farm, SLOT_RULE_BAR, on ? COLOR_BAD : COLOR_INFO);
    rule_state(farm, now_ms);
    rule_note(farm);
}

/* The alarm line: the voice command, the check-in question, the
 * alarm, or how many there were. */
static void alarm_line(farm_t *farm, uint32_t now_ms)
{
    const state_t *state = &farm->state;
    char text[TEXT_BYTES];

    if (farm->id == MISSION_VOICE) {
        put(farm, SLOT_ALARM, state->voice_text, COLOR_INFO);
    } else if (state->has_asked) {
        (void)snprintf(
            text, sizeof(text), "โอเคไหม? กด SW6 (อีก %" PRId32 " วิ)",
            seconds_left(
                ((int32_t)CHECKIN_S * (int32_t)MS_PER_S) -
                bento_time_ticks_diff(now_ms, state->asked_ms)));
        put(farm, SLOT_ALARM, text, COLOR_WARN);
    } else if (state->alarm.on) {
        put(farm, SLOT_ALARM, "เตือนอยู่! กด SW6 รับทราบ", COLOR_BAD);
    } else if (state->alarm.acked) {
        (void)snprintf(text, sizeof(text),
                       "รับทราบแล้ว (เตือนใหม่หลังสงบ %" PRId32 " วิ)",
                       (int32_t)ACKED_REARM_MS / (int32_t)MS_PER_S);
        put(farm, SLOT_ALARM, text, COLOR_WARN);
    } else {
        (void)snprintf(text, sizeof(text),
                       "ไม่มีการเตือน (เตือนแล้ว %" PRId32 ")",
                       state->alarm.count);
        put(farm, SLOT_ALARM, text, COLOR_DIM);
    }
}

static void draw_level(farm_t *farm, uint32_t now_ms)
{
    static const slot_t cells[] = {SLOT_CELL_BOTH, SLOT_CELL_AI,
                                   SLOT_CELL_RULE, SLOT_CELL_NONE};
    static const agree_t answers[] = {AGREE_BOTH, AGREE_AI, AGREE_RULE,
                                      AGREE_NONE};
    static const char *const level_texts[] = {
        "ระดับ 0: ปกติ", "ระดับ 1: ฝ่ายเดียว ไปดู", "ระดับ 2: ยืนยันสองทาง!"};
    static const uint32_t level_colors[] = {COLOR_OK, COLOR_WARN,
                                            COLOR_BAD};
    const state_t *state = &farm->state;
    uint32_t i;

    for (i = 0U; i < (uint32_t)(sizeof(cells) / sizeof(cells[0]));
         i++) {
        put_value(farm, cells[i], (state->agree == answers[i]) ? 1 : 0);
    }
    put_value(farm, SLOT_LEVEL_LED, 1);
    put_color(farm, SLOT_LEVEL_LED, level_colors[state->level]);
    put(farm, SLOT_LEVEL, level_texts[state->level],
        level_colors[state->level]);
    alarm_line(farm, now_ms);
}

/* What the matrix should show now: the alarm in red, the question
 * in yellow, a voice command, LOOK at level 1, else the AI's bar. */
static picture_t wanted_picture(const farm_t *farm, uint32_t now_ms)
{
    const state_t *state = &farm->state;
    picture_t want = {true, true, NULL, 0, BENTO_RGBMATRIX_YELLOW};

    if (state->alarm.on) {
        want.text = farm->mission->alarm_text;
        want.color = BENTO_RGBMATRIX_RED;
    } else if (state->has_asked) {
        want.text = "OK? SW6";
    } else if (state->has_say &&
               (bento_time_ticks_diff(now_ms, state->say_ms) <
                VOICE_SHOW_MS)) {
        want.text = state->say_text;
        want.color = state->say_color;
    } else if ((state->level >= 1) && (farm->id != MISSION_VOICE)) {
        want.text = "LOOK";
    } else {
        const int32_t bar = (farm->ai.conf * MATRIX_BAR_MAX) / PERCENT;

        want.say = false;
        want.bar = (bar < MATRIX_BAR_MAX) ? bar : MATRIX_BAR_MAX;
        want.color = BENTO_RGBMATRIX_GREEN;
    }
    return want;
}

static bool same_picture(const picture_t *left, const picture_t *right)
{
    bool same = left->has && right->has && (left->say == right->say) &&
                (left->color == right->color);

    if (same && left->say) {
        same = strcmp(left->text, right->text) == 0;
    } else if (same) {
        same = left->bar == right->bar;
    } else {
        /* different already */
    }
    return same;
}

static void send_picture(const state_t *state, const picture_t *want)
{
    if (want->say) {
        matrix_scroll(want->text, want->color);
    } else {
        if (!state->picture.has || state->picture.say) {
            (void)bento_rgbmatrix_clear(); /* an OSError is skipped */
        }
        (void)bento_rgbmatrix_bar(want->bar, MATRIX_BAR_MAX,
                                  want->color);
    }
}

/* A marquee runs by itself on the board: sent once. The bar is sent
 * again every MATRIX_MS in case a picture was lost. */
static void draw_matrix(farm_t *farm, uint32_t now_ms)
{
    state_t *state = &farm->state;
    const picture_t want = wanted_picture(farm, now_ms);
    const bool due =
        bento_time_ticks_diff(now_ms, state->picture_ms) >= MATRIX_MS;

    if (!same_picture(&want, &state->picture) || (!want.say && due)) {
        send_picture(state, &want);
        state->picture = want;
        state->picture_ms = now_ms;
    }
}
static void draw(farm_t *farm, uint32_t now_ms)
{
    draw_ai(farm);
    draw_rule(farm, now_ms);
    draw_level(farm, now_ms);
    draw_matrix(farm, now_ms);
    poll_display(farm->ui);
}

/* ---- the main program ---- */

static bento_status_t end_alarm(farm_t *farm, const char *by)
{
    char tail[TAIL_BYTES];
    bento_status_t status;

    (void)snprintf(tail, sizeof(tail), "\"by\": \"%s\"", by);
    send_event(farm, "ai_alarm_end", tail);
    status = sound(farm, TUNE_CLEAR);
    if (status == BENTO_OK) {
        status = set_led(farm, false);
    }
    farm->state.force = true;
    return status;
}

/* SW6, or {"cmd": "ack"} from the app. */
static bento_status_t acknowledge(farm_t *farm, uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;

    if (!alarm_acknowledge(&farm->state.alarm)) {
        note(farm, "ไม่มีการเตือนให้รับทราบ", COLOR_DIM);
    } else {
        status = end_alarm(farm, "ack");
        farm->state.checkin_failed = false;
        farm->rules.still_ms = now_ms; /* worker: count again */
        note(farm, "รับทราบแล้ว", COLOR_OK);
    }
    return status;
}

static bento_status_t start_alarm(farm_t *farm, uint32_t now_ms)
{
    state_t *state = &farm->state;
    const ai_t *ai = &farm->ai;
    const char *label = ai->history.on ? ai->hit : "-";
    const int32_t conf = ai->history.on ? ai->hit_conf : 0;
    char text[TAIL_BYTES];
    bento_status_t status;

    (void)snprintf(
        text, sizeof(text),
        "\"model\": \"%s\", \"label\": \"%s\", \"conf\": %" PRId32
        ", \"lvl\": %" PRId32,
        model_name(farm), label, conf, state->level);
    send_event(farm, "ai_alarm_start", text);
    status = sound(farm, TUNE_ALARM);
    if (status == BENTO_OK) {
        status = set_led(farm, true);
    }
    state->beep_ms = now_ms;
    state->force = true;
    (void)snprintf(text, sizeof(text), "เตือน! %s - กด SW6 รับทราบ",
                   AGREE_KEYS[state->agree]);
    note(farm, text, COLOR_BAD);
    (void)printf("เตือน: %s %s %s %" PRId32 "\n", MISSION,
                 AGREE_KEYS[state->agree], label, conf);
    return status;
}

/* Usually: alarm at level 2, gone at level 0. worker: alarm when the
 * question got no answer, whatever the level. */
static bento_status_t alarm_step(farm_t *farm, uint32_t now_ms)
{
    const int32_t repeat_ms = ALARM_REPEAT_MS;
    state_t *state = &farm->state;
    bento_status_t status = BENTO_OK;
    action_t action;

    if (farm->id == MISSION_WORKER) {
        action = alarm_update(&state->alarm, state->checkin_failed,
                              !state->checkin_failed, now_ms);
    } else {
        action =
            alarm_update(&state->alarm, state->level == ALARM_LEVEL,
                         state->level == 0, now_ms);
    }
    if (action == ACTION_START) {
        status = start_alarm(farm, now_ms);
    } else if (action == ACTION_CLEAR) {
        status = end_alarm(farm, "clear");
        note(farm, "เหตุหายไปเอง", COLOR_OK);
    } else if (state->alarm.on && (repeat_ms > 0) &&
               (bento_time_ticks_diff(now_ms, state->beep_ms) >=
                repeat_ms)) {
        status = sound(farm, TUNE_ALARM); /* nobody answered: again */
        state->beep_ms = now_ms;
    } else {
        /* nothing to sound */
    }
    return status;
}

/* The event voice_cmd and the plc/cmd of a gate result. */
static void send_voice(farm_t *farm, const gate_result_t *result)
{
    static const char *const words[] = {"go", "stop"};
    static const char *const whys[] = {"", "wait", "unconfirmed",
                                       "gap"};
    char tail[TAIL_BYTES];

    (void)snprintf(
        tail, sizeof(tail),
        "\"cmd\": \"%s\", \"ok\": %" PRId32 ", \"why\": \"%s\"",
        words[result->command], result->ok, whys[result->why]);
    send_event(farm, "voice_cmd", tail);
    if (result->has_plc && (result->command == WORD_GO)) {
        (void)snprintf(tail, sizeof(tail), "{\"pump\": 1, \"sec\": %d}",
                       (PUMP_SEC < PUMP_MAX_S) ? PUMP_SEC : PUMP_MAX_S);
        (void)send(&farm->state, "plc/cmd", tail);
    } else if (result->has_plc) {
        (void)send(&farm->state, "plc/cmd", "{\"pump\": 0}");
    } else {
        /* nothing for the PLC */
    }
}

static void say(state_t *state, const char *text, int32_t color,
                uint32_t now_ms)
{
    state->has_say = true;
    state->say_text = text;
    state->say_color = color;
    state->say_ms = now_ms;
}

/* A command done or refused: the text under the level, the
 * matrix and the tone. */
static bento_status_t voice_outcome(farm_t *farm,
                                    const gate_result_t *result,
                                    uint32_t now_ms)
{
    state_t *state = &farm->state;
    bento_status_t status;

    if ((result->ok != 0) && (result->command == WORD_GO)) {
        (void)snprintf(state->voice_text, sizeof(state->voice_text),
                       "go: เปิดปั๊ม %d วิ",
                       (PUMP_SEC < PUMP_MAX_S) ? PUMP_SEC : PUMP_MAX_S);
        say(state, "PUMP ON", BENTO_RGBMATRIX_GREEN, now_ms);
        status = sound(farm, TUNE_ASK);
    } else if (result->ok != 0) {
        (void)snprintf(state->voice_text, sizeof(state->voice_text),
                       "stop: ปิดปั๊มทันที");
        say(state, "PUMP OFF", BENTO_RGBMATRIX_RED, now_ms);
        status = sound(farm, TUNE_STOP);
    } else if (result->why == WHY_UNCONFIRMED) {
        (void)snprintf(state->voice_text, sizeof(state->voice_text),
                       "go ไม่ทำ: ยังไม่ยืนยัน");
        say(state, "NO", BENTO_RGBMATRIX_YELLOW, now_ms);
        status = sound(farm, TUNE_NO);
    } else {
        (void)snprintf(state->voice_text, sizeof(state->voice_text),
                       "go ไม่ทำ: สั่งถี่เกิน %" PRId32 " วิ",
                       (int32_t)PLC_GAP_MS / (int32_t)MS_PER_S);
        say(state, "NO", BENTO_RGBMATRIX_YELLOW, now_ms);
        status = sound(farm, TUNE_NO);
    }
    return status;
}

/* A gate result: the event, the PLC, the tone, the screen. */
static bento_status_t
voice_do(farm_t *farm, const gate_result_t *result, uint32_t now_ms)
{
    static const char *const words[] = {"go", "stop"};
    static const char *const whys[] = {"", "wait", "unconfirmed",
                                       "gap"};
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    if (result->has && (result->ok < 0)) {
        (void)snprintf(text, sizeof(text),
                       "ได้ยิน go: กด SW6 หรือ go ซ้ำ ภายใน %" PRId32 " วิ",
                       (int32_t)GO_CONFIRM_MS / (int32_t)MS_PER_S);
        note(farm, text, COLOR_WARN);
    } else if (result->has) {
        send_voice(farm, result);
        status = voice_outcome(farm, result, now_ms);
        (void)snprintf(
            text, sizeof(text), "voice_cmd %s ok %" PRId32 " %s",
            words[result->command], result->ok, whys[result->why]);
        note(farm, text, (result->ok != 0) ? COLOR_OK : COLOR_WARN);
    } else {
        /* no result */
    }
    return status;
}

/* Still for STILL_S: ask. No answer in CHECKIN_S: check_in ok 0, and
 * the alarm comes from the missing answer, even without a model. */
static bento_status_t worker_step(farm_t *farm, uint32_t now_ms)
{
    state_t *state = &farm->state;
    char text[TEXT_BYTES];
    bento_status_t status = BENTO_OK;

    if (state->paused || (state->judged.rule == 0)) {
        state->has_asked = false; /* moved (or paused): stop asking */
        state->checkin_failed = false;
    } else if (!state->has_asked && !state->checkin_failed) {
        state->has_asked = true;
        state->asked_ms = now_ms;
        status = sound(farm, TUNE_ASK);
        (void)snprintf(text, sizeof(text),
                       "นิ่งนาน %d วิ: โอเคไหม? กด SW6", STILL_S);
        note(farm, text, COLOR_WARN);
    } else if (state->has_asked &&
               (bento_time_ticks_diff(now_ms, state->asked_ms) >=
                ((int32_t)CHECKIN_S * (int32_t)MS_PER_S))) {
        state->has_asked = false;
        state->checkin_failed = true;
        send_event(farm, "check_in", "\"ok\": 0");
    } else {
        /* still waiting */
    }
    return status;
}

/* One hole done = the AI confirms the bit went through. Without a
 * model the material is unknown, so no drill_done is sent. */
static void drill_step(farm_t *farm, bool edge)
{
    static const char *const materials[] = {"wood", "plastic"};
    static const char *const thai[] = {"ไม้", "พลาสติก"};
    const char *hit = farm->ai.hit;
    char text[TEXT_BYTES];
    uint32_t i;

    for (i = 0U; edge && (i < 2U); i++) {
        const size_t length = strlen(materials[i]);

        if ((strncmp(hit, materials[i], length) == 0) &&
            (strcmp(&hit[length], "_out") == 0)) {
            farm->state.holes[i]++;
            (void)snprintf(text, sizeof(text),
                           "\"mat\": \"%s\", \"holes\": %" PRId32,
                           materials[i], farm->state.holes[i]);
            send_event(farm, "drill_done", text);
            (void)snprintf(text, sizeof(text),
                           "ทะลุ%s แล้ว %" PRId32 " รู", thai[i],
                           farm->state.holes[i]);
            note(farm, text, COLOR_WARN);
        }
    }
}

/* voice: the words the AI confirmed (3 in 5) go through the gate;
 * the AI side counts a word heard in the last GO_CONFIRM_MS. */
static bento_status_t judge_voice(farm_t *farm, uint32_t now_ms,
                                  bool edge, bool *ai_on)
{
    const ai_t *ai = &farm->ai;
    gate_t *gate = &farm->state.gate;
    gate_result_t result;
    bento_status_t status = BENTO_OK;

    if (edge && (strcmp(ai->hit, "go") == 0)) {
        result = gate_heard(gate, WORD_GO, now_ms, true);
        status = voice_do(farm, &result, now_ms);
    } else if (edge && (strcmp(ai->hit, "stop") == 0)) {
        result = gate_heard(gate, WORD_STOP, now_ms, true);
        status = voice_do(farm, &result, now_ms);
    } else {
        /* no new word */
    }
    if (status == BENTO_OK) {
        result = gate_expire(gate, now_ms);
        status = voice_do(farm, &result, now_ms);
    }
    *ai_on = ai->history.on ||
             (ai->running &&
              (gate_recent(gate->has_ai, gate->ai_ms, now_ms) != 0));
    return status;
}

/* The rule and the AI give agree and level; then each mission's own
 * step. */
static bento_status_t judge(farm_t *farm, uint32_t now_ms, bool edge,
                            bool was_rain)
{
    state_t *state = &farm->state;
    bool ai_on = farm->ai.history.on;
    bento_status_t status = BENTO_OK;

    if (farm->id == MISSION_VOICE) {
        status = judge_voice(farm, now_ms, edge, &ai_on);
    }
    if (state->paused) {
        const judged_t idle = {0, farm->rules.kind, whole(0), whole(0)};

        state->judged = idle;
    } else if (farm->id == MISSION_VOICE) {
        const int32_t on =
            gate_recent(state->gate.has_ok, state->gate.ok_ms, now_ms);
        const judged_t confirmed = {on, RULE_BUTTON, whole(on),
                                    whole(1)};

        state->judged = confirmed;
    } else {
        state->judged = rules_judge(farm, now_ms, edge);
    }
    state->agree = agreement(ai_on, state->judged.rule != 0);
    state->level = level_of(state->agree);
    if ((farm->id == MISSION_NIGHT) && farm->ai.rain.on && !was_rain) {
        char tail[TAIL_BYTES];

        (void)snprintf(tail, sizeof(tail), "\"conf\": %" PRId32,
                       farm->ai.conf);
        send_event(farm, "rain", tail);
    } else if ((status == BENTO_OK) && (farm->id == MISSION_WORKER)) {
        status = worker_step(farm, now_ms);
    } else if (farm->id == MISSION_DRILL) {
        drill_step(farm, edge);
    } else {
        /* nothing more for this mission */
    }
    return status;
}

static bento_status_t pause(farm_t *farm)
{
    state_t *state = &farm->state;
    bento_status_t status = BENTO_OK;

    state->paused = true;
    if (alarm_acknowledge(&state->alarm)) {
        status = end_alarm(farm, "ack"); /* a press on the board */
    }
    if (status == BENTO_OK) {
        status = ai_stop(&farm->ai);
    }
    state->has_asked = false;
    state->checkin_failed = false;
    note(farm, "พักอยู่: AI และกฎหยุด - กด SW5 ทำต่อ", COLOR_WARN);
    state->force = true;
    return status;
}

static bento_status_t resume(farm_t *farm)
{
    state_t *state = &farm->state;
    bool resumed = true;
    bento_status_t status = BENTO_OK;

    state->paused = false;
    if (!state->ai_off && (farm->ai.index >= 0)) {
        status = ai_resume(&farm->ai, &resumed);
    }
    if (!resumed) {
        note(farm, "โหลดโมเดลไม่สำเร็จ: ใช้กฎอย่างเดียว", COLOR_BAD);
    } else {
        note(farm, "ทำต่อ", COLOR_OK);
    }
    rules_reset(&farm->rules, bento_time_ticks_ms());
    state->force = true;
    return status;
}

/* voice: the buttons stand in for the voice. SW5 = go; SW6 confirms
 * the go that waits, or with none waiting means stop. */
static bento_status_t press_voice(farm_t *farm, bool sw6, bool sw5,
                                  uint32_t now_ms)
{
    gate_t *gate = &farm->state.gate;
    gate_result_t result;
    bento_status_t status = BENTO_OK;

    if (sw5) {
        result = gate_heard(gate, WORD_GO, now_ms, false);
        status = voice_do(farm, &result, now_ms);
    }
    if ((status == BENTO_OK) && sw6) {
        result = gate_confirm(gate, now_ms);
        if (!result.has) {
            result = gate_heard(gate, WORD_STOP, now_ms, false);
        }
        status = voice_do(farm, &result, now_ms);
    }
    return status;
}

/* SW6 for a worker: the answer to the question, or "still fine". */
static bento_status_t press_worker_ok(farm_t *farm, uint32_t now_ms)
{
    state_t *state = &farm->state;
    bento_status_t status = BENTO_OK;

    if (state->has_asked) {
        state->has_asked = false;
        farm->rules.still_ms = now_ms;
        send_event(farm, "check_in", "\"ok\": 1");
        status = sound(farm, TUNE_OK);
        note(farm, "โอเค - เริ่มนับใหม่", COLOR_OK);
    } else {
        farm->rules.still_ms = now_ms; /* pressing means fine too */
        note(farm, "โอเค - เริ่มนับใหม่", COLOR_OK);
    }
    return status;
}

/* SW5 pauses or resumes; SW6 answers the worker, or acknowledges. */
static bento_status_t press_board(farm_t *farm, bool sw6, bool sw5,
                                  uint32_t now_ms)
{
    bento_status_t status = BENTO_OK;

    if (sw5) {
        status = sound(farm, TUNE_CLICK);
    }
    if ((status == BENTO_OK) && sw5) {
        status = farm->state.paused ? resume(farm) : pause(farm);
    }
    if ((status == BENTO_OK) && sw6 && (farm->id == MISSION_WORKER) &&
        (farm->state.has_asked || !farm->state.alarm.on)) {
        status = press_worker_ok(farm, now_ms);
    } else if ((status == BENTO_OK) && sw6) {
        status = acknowledge(farm, now_ms);
    } else {
        /* no SW6 */
    }
    return status;
}

static bento_status_t press(farm_t *farm, bool sw6, bool sw5,
                            uint32_t now_ms)
{
    return (farm->id == MISSION_VOICE)
               ? press_voice(farm, sw6, sw5, now_ms)
               : press_board(farm, sw6, sw5, now_ms);
}
/* select: the found model with exactly that name; on failure the
 * old one comes back. */
static bento_status_t obey_select(farm_t *farm, const char *name,
                                  uint32_t now_ms)
{
    ai_t *ai = &farm->ai;
    char old_name[BENTO_EDGEAI_NAME_LEN];
    const uint32_t old_index = (uint32_t)ai->index;
    bool selected = false;
    bento_status_t status = BENTO_OK;
    uint32_t i;
    uint32_t pick = ai->found_count;

    for (i = ai->found_count; i > 0U; i--) {
        if (strcmp(ai->found_name[i - 1U], name) == 0) {
            pick = i - 1U;
        }
    }
    if (pick == ai->found_count) {
        note(farm, "ไม่มีโมเดลชื่อนี้ในภารกิจ: ไม่ทำ", COLOR_WARN);
    } else {
        (void)snprintf(old_name, sizeof(old_name), "%s", ai->name);
        status = ai_stop(ai);
        if (status == BENTO_OK) {
            status = ai_select(ai, ai->found_index[pick],
                               ai->found_name[pick], &selected);
        }
        if ((status == BENTO_OK) && !selected) {
            status = ai_select(ai, old_index, old_name, &selected);
        }
        if ((status == BENTO_OK) && !selected) {
            note(farm, "เปลี่ยนโมเดลไม่สำเร็จ", COLOR_BAD);
        }
        farm->state.ai_off = false;
        farm->state.paused = false;
        rules_reset(&farm->rules, now_ms);
        show_model(farm);
        farm->state.force = true;
    }
    return status;
}

/* stop, run or select: at most one per CMD_GAP_MS (item 3.10). */
static bento_status_t obey(farm_t *farm, command_kind_t kind,
                           const command_t *command, uint32_t now_ms)
{
    state_t *state = &farm->state;
    bool resumed = true;
    bento_status_t status = BENTO_OK;

    state->command_ms = now_ms;
    if (kind == COMMAND_STOP) {
        state->ai_off = true;
        status = ai_stop(&farm->ai);
        note(farm, "แอปสั่งหยุดโมเดล (กฎยังทำงาน)", COLOR_WARN);
    } else if ((kind == COMMAND_RUN) && state->paused) {
        state->ai_off = false;
        status = resume(farm);
    } else if (kind == COMMAND_RUN) {
        state->ai_off = false;
        if (!farm->ai.running) {
            status = ai_resume(&farm->ai, &resumed);
        }
        if (!resumed) {
            note(farm, "โหลดโมเดลไม่สำเร็จ", COLOR_BAD);
        }
    } else {
        status = obey_select(farm, command->model, now_ms);
    }
    /* the answer: the state goes out at once, its model confirms */
    state->force = state->force || (kind != COMMAND_SELECT);
    return status;
}

/* One waiting ai/cmd, if any: COMMAND_NONE for anything else (too
 * long, not an object, unknown). */
static command_kind_t read_inbox(farm_t *farm, command_t *command)
{
    bento_mqtt_msg_t message;
    char topic[BENTO_MQTT_TOPIC_MAX];
    char wanted[NAME_BYTES];
    command_kind_t kind = COMMAND_NONE;

    bento_mqtt_msg_init(&message);
    (void)snprintf(wanted, sizeof(wanted), "%sai/cmd",
                   farm->state.base);
    /* nothing waiting, or an OSError there: no command this pass */
    if ((bento_mqtt_poll(farm->state.broker, &message, topic,
                         sizeof(topic), farm->inbox,
                         sizeof(farm->inbox)) == BENTO_OK) &&
        (strcmp(topic, wanted) == 0) && (message.truncated == 0U) &&
        (message.payload_len <= (uint32_t)CMD_MAX) &&
        read_command(farm->inbox, message.payload_len, command)) {
        kind = command_kind(command);
    }
    return kind;
}

/* Commands from the app on ai/cmd (item 3.10, ack of item 3.11). */
static bento_status_t inbox(farm_t *farm, uint32_t now_ms)
{
    command_t command;
    char text[TEXT_BYTES];
    command_kind_t kind = COMMAND_NONE;
    bento_status_t status = BENTO_OK;

    if (farm->state.online) {
        kind = read_inbox(farm, &command);
    }
    if (kind == COMMAND_ACKNOWLEDGE) {
        status = acknowledge(farm, now_ms);
    } else if ((kind != COMMAND_NONE) && (farm->ai.index < 0)) {
        (void)snprintf(text, sizeof(text), "ไม่มีโมเดล: ไม่ทำคำสั่ง %s",
                       command.command);
        note(farm, text, COLOR_DIM);
    } else if ((kind != COMMAND_NONE) &&
               (bento_time_ticks_diff(now_ms, farm->state.command_ms) <
                CMD_GAP_MS)) {
        (void)snprintf(text, sizeof(text),
                       "คำสั่งถี่เกิน %" PRId32 " วิ: ไม่ทำ",
                       (int32_t)CMD_GAP_MS / (int32_t)MS_PER_S);
        note(farm, text, COLOR_WARN);
    } else if (kind != COMMAND_NONE) {
        status = obey(farm, kind, &command, now_ms);
    } else {
        /* no command */
    }
    return status;
}

/* 1) read the AI and the rule's sensor, 2) judge, 3) act. */
static bento_status_t sense_and_act(farm_t *farm, uint32_t now_ms)
{
    const bool quiet =
        bento_time_ticks_diff(now_ms, farm->state.quiet_ms) < 0;
    const bool was = farm->ai.history.on;
    const bool was_rain = farm->ai.rain.on;
    bool edge;
    bento_status_t status;

    if (!farm->state.paused) {
        ai_poll(farm, now_ms, quiet);
        rules_read(&farm->rules, now_ms);
    }
    edge = farm->ai.history.on && !was; /* the AI just confirmed */
    status = judge(farm, now_ms, edge, was_rain);
    if ((status == BENTO_OK) && (farm->id != MISSION_VOICE)) {
        status = alarm_step(farm, now_ms); /* voice has no alarm */
    }
    if (status == BENTO_OK) {
        const bool sw6 = pressed_now(farm, 0U);
        const bool sw5 = pressed_now(farm, 1U);

        status = press(farm, sw6, sw5, now_ms);
    }
    if (status == BENTO_OK) {
        status = inbox(farm, now_ms);
    }
    return status;
}

/* One pass every SAMPLE_MS: sense, judge, act, send, show. */
static bento_status_t run_pass(farm_t *farm, uint32_t *draw_ms)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bento_status_t status = sense_and_act(farm, now_ms);

    if (status == BENTO_OK) {
        report(farm);
        if (farm->state.online && !broker_alive(&farm->state)) {
            farm->state.online = false;
            show_link(farm);
            note(farm, "เน็ตหลุด ทำงานต่อแบบออฟไลน์", COLOR_WARN);
        }
        if (bento_time_ticks_diff(now_ms, *draw_ms) >= DRAW_MS) {
            *draw_ms = now_ms;
            draw(farm, now_ms);
        }
        status = wait_sampling(farm, SAMPLE_MS);
    }
    return status;
}

static bento_status_t watch(farm_t *farm)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t draw_ms = start_ms;
    bento_status_t status = BENTO_OK;

    rules_reset(&farm->rules, start_ms);
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = run_pass(farm, &draw_ms);
    }
    return status;
}

/* The model first (the mic opens after it), then the network, then
 * the watch. */
static bento_status_t run_mission(farm_t *farm)
{
    bool started = false;
    bento_status_t status;

    note(farm, "กำลังหาโมเดล (บนบอร์ดอาจนาน 15 วิ)...", COLOR_WARN);
    poll_display(farm->ui);
    status = ai_start(&farm->ai, farm->mission, &started);
    if ((status == BENTO_OK) && !started) {
        (void)printf("ไม่พบหรือโหลดโมเดลไม่ได้: %s%s%s - ใช้กฎอย่างเดียว\n",
                     farm->mission->store_model,
                     (farm->mission->builtin_model[0] != '\0') ? " / "
                                                               : "",
                     farm->mission->builtin_model);
    }
    if (status == BENTO_OK) {
        show_model(farm);
        rules_init(&farm->rules, farm->mission->rule);
        status = go_online(farm);
    }
    if (status == BENTO_OK) {
        show_link(farm);
        status = watch(farm);
    }
    if (status == BENTO_OK) {
        note(farm, "ครบเวลา - กด Program to Device เพื่อเล่นใหม่",
             COLOR_WARN);
    }
    return status;
}

/* Stopped halfway or not: the AI core, the LED, the matrix and the
 * broker are always released. */
static bento_status_t finish(farm_t *farm, bento_status_t run_status)
{
    bento_status_t status;

    (void)stop_engine(); /* the .py ignores an OSError from stop() */
    (void)bento_edgeai_deinit(); /* never fails: nothing is sent */
    status = set_led(farm, false);
    (void)bento_rgbmatrix_clear(); /* an OSError is skipped there */
    close_broker(&farm->state);
    (void)printf("ภารกิจ %s จบ: เตือน %" PRId32
                 " ครั้ง | กฎอ่านค่าไม่ได้ %" PRId32 " ครั้ง\n",
                 MISSION, farm->state.alarm.count, farm->rules.fails);
    poll_display(farm->ui);
    return (run_status != BENTO_OK) ? run_status : status;
}

/* The smallest mission name after previous, in byte order. */
static const char *name_after(const char *previous)
{
    const uint32_t count =
        (uint32_t)(sizeof(MISSIONS) / sizeof(MISSIONS[0]));
    const char *next = "";
    uint32_t i;

    for (i = 0U; i < count; i++) {
        if ((strcmp(MISSIONS[i].key, previous) > 0) &&
            ((next[0] == '\0') ||
             (strcmp(MISSIONS[i].key, next) < 0))) {
            next = MISSIONS[i].key;
        }
    }
    return next;
}

/* The names of MISSIONS in sorted order, each in quote, joined by
 * between, after open. */
static void sorted_names(char *text, uint32_t capacity,
                         const char *open, const char *between,
                         const char *quote)
{
    const uint32_t count =
        (uint32_t)(sizeof(MISSIONS) / sizeof(MISSIONS[0]));
    const char *name = "";
    size_t used;
    uint32_t round;

    (void)snprintf(text, capacity, "%s", open);
    for (round = 0U; round < count; round++) {
        name = name_after(name);
        used = strlen(text);
        (void)snprintf(&text[used], capacity - used, "%s%s%s%s",
                       (round == 0U) ? "" : between, quote, name,
                       quote);
    }
}
/* MISSION names no mission: say so, and which ones there are. */
static bento_status_t unknown_mission(farm_t *farm)
{
    static const part_t problem = {
        KIND_LABEL, SLOT_NONE, "", 12, 120, 0, 0, COLOR_BAD, 24};
    static const part_t choices = {
        KIND_LABEL, SLOT_NONE, "", 12, 170, 0, 0, COLOR_TEXT, 16};
    char name[ASCII_KEEP + 1];
    char text[TEXT_BYTES];
    bento_ui_id_t made = 0U;
    bento_status_t status = bento_ui_screen_default(farm->ui);

    if (status == BENTO_OK) {
        status = nap(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        ascii_only(MISSION, name, sizeof(name));
        (void)snprintf(text, sizeof(text), "ไม่รู้จัก MISSION นี้: %s", name);
        status = add_label(farm->ui, text, &problem, &made);
    }
    if (status == BENTO_OK) {
        sorted_names(text, sizeof(text), "เลือก: ", " ", "");
        status = add_label(farm->ui, text, &choices, &made);
    }
    if (status == BENTO_OK) {
        poll_display(farm->ui);
        sorted_names(text, sizeof(text), "[", ", ", "'");
        (void)printf("MISSION ต้องเป็นหนึ่งใน %s]\n", text);
    }
    return status;
}

/* The state at the start: a heartbeat and a command are due at
 * once, and the first state message is forced. */
static void start_state(farm_t *farm)
{
    state_t *state = &farm->state;
    const uint32_t now_ms = bento_time_ticks_ms();

    (void)snprintf(state->base, sizeof(state->base), "bento-aiot/%s/",
                   TEAM);
    state->force = true;
    state->sent_ms = bento_time_ticks_add(now_ms, -HEARTBEAT_MS);
    state->command_ms = bento_time_ticks_add(now_ms, -CMD_GAP_MS);
    state->quiet_ms = now_ms;
    state->beep_ms = now_ms;
    state->picture_ms = now_ms;
    state->judged.kind = RULE_UNSET;
    state->agree = AGREE_NONE;
    state->gate.last = WORD_NONE;
    (void)snprintf(state->voice_text, sizeof(state->voice_text), " ");
    farm->ai.index = -1;
    (void)snprintf(farm->ai.name, sizeof(farm->ai.name), "%s",
                   RULE_ONLY);
    ai_clear(&farm->ai, now_ms);
}

static const mission_t *find_mission(mission_id_t *id)
{
    const uint32_t count =
        (uint32_t)(sizeof(MISSIONS) / sizeof(MISSIONS[0]));
    const mission_t *mission = NULL;
    uint32_t i;

    for (i = 0U; i < count; i++) {
        if ((mission == NULL) &&
            (strcmp(MISSIONS[i].key, MISSION) == 0)) {
            mission = &MISSIONS[i];
            *id = (mission_id_t)i;
        }
    }
    return mission;
}

int main(void)
{
    farm_t *farm = &g_farm;
    bento_status_t status;

    (void)memset(farm, 0, sizeof(*farm));
    farm->ui = bento_runtime_get_ui();
    status = bento_ui_sfx_volume_set(farm->ui, SPEAKER);
    if (status == BENTO_E_UNSUPPORTED) {
        status = BENTO_OK; /* firmware 2.4.1 skips the volume line */
    }
    farm->mission = find_mission(&farm->id);
    if ((status == BENTO_OK) && (farm->mission == NULL)) {
        status = unknown_mission(farm);
    } else if (status == BENTO_OK) {
        status = build_screen(farm);
        if (status == BENTO_OK) {
            start_state(farm);
            find_led(farm);
            status = finish(farm, run_mission(farm));
        }
    } else {
        /* the volume failed: the run ends */
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
