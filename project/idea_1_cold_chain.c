/*
 * idea_1_cold_chain.c -- กล่องผลไม้ห้องเย็น: นับเวลาที่กล่องอุ่นเกินงบ
 * (A cold-chain fruit box: counting the seconds it runs warm)
 *
 * ต่อ WiFi และ broker แล้วเฝ้ากล่องผลไม้ 10 นาที อ่าน SHT40 ทุกวินาที
 * บวกความร้อนจากนอกกล่องที่หมุนจาก VR3 (0-10 C) จอมีปรอทแนวตั้ง (Bar)
 * ตัวเลขใหญ่ (Seg7) และวงงบความร้อน (Arc) ทุกวินาทีที่อุ่นเกินเกณฑ์กิน
 * งบที่ผลไม้ทนได้ ระดับเปลี่ยนเมื่อไรส่ง JSON ไปหัวข้อ event พร้อม
 * warm_s และ t_max แล้วเลื่อนคำบน RGB matrix และส่งเสียง ร้อนเกินมี
 * กล่องเตือนและเสียงซ้ำทุก 5 วินาทีจนคนขับกด SW5 หรือแอปส่ง
 * {"cmd":"ack"} คำสั่ง {"cmd":"set","t_hi":..} ตั้งเกณฑ์ร้อนใหม่
 * แก้ WIFI_SSID WIFI_PASS TEAM CROP และ TEMP_OFFSET ก่อนรัน
 * เกณฑ์เป็นค่าสาธิตในห้อง ไม่ใช่คำแนะนำทางเกษตร
 *
 * Joins Wi-Fi and the broker, then watches a fruit box for 10
 * minutes: the SHT40 is read every second, plus outside heat from
 * VR3 (0-10 C). The screen has a thermometer (Bar), big digits
 * (Seg7) and a heat budget ring (Arc): every second above the
 * limit uses up the seconds the fruit can stand. Each change of
 * level sends JSON to the event topic with warm_s and t_max,
 * scrolls a word on the RGB matrix and plays a tune; too hot also
 * opens a message box and beeps every 5 s until the driver presses
 * SW5 or the app sends {"cmd":"ack"}. {"cmd":"set","t_hi":..} sets
 * a new hot limit. Edit WIFI_SSID, WIFI_PASS, TEAM, CROP and
 * TEMP_OFFSET first. The limits are classroom demo values, not
 * farming advice.
 *
 * Twin of idea_1_cold_chain.py
 * (aiot-development-for-smart-farm/project/idea_1_cold_chain.py).
 * Style: CLEAN_C.md.
 * Differs from the .py:
 *   - json: the event is built with snprintf, keys in the order the
 *     .py writes them (MicroPython's dict may print them in another
 *     order; a JSON reader does not care). round(x, 1) and str() of
 *     a real number are bento_fmt's, as on the board.
 *   - json.loads: read_command() reads one standard JSON object and
 *     finds "cmd" and "t_hi" at its top level (the last of two equal
 *     keys wins). MicroPython also takes some broken texts (a comma
 *     or colon missing); here those are not JSON. Inside a nested
 *     list or object only strings and brackets are checked. A string
 *     is compared as written: an escaped letter is not decoded.
 *   - get_message() keeps one message, a newer one overwrites it;
 *     the C client keeps a small ring, so each round takes them all
 *     and keeps the newest. Payloads past 255 bytes are cut, as the
 *     board's binding cuts them.
 *   - wifi.connect(): a name or password the radio refuses outright
 *     counts as not joined (the .py's binding cuts it to fit).
 *   - send(): the .py catches only the OSError of a dropped link; a
 *     publish the client refuses still counts as sent, here too.
 *   - mqtt.connect() is start and poll here; a failed attempt closes
 *     its client so the next attempt has a free one.
 *   - On a board without VR3, SW5 or the RGB matrix (the .py cannot
 *     import them there) VR3 reads 0, SW5 is never down and the
 *     matrix is skipped; any other failure of them ends the program.
 *   - The wifi and mqtt modules' own console lines are not printed.
 *   - In the BENTO Emulator, MQTT from C is not available yet: the
 *     program says so and ends on the
 *     .py's broker-down screen.
 *   - Stop ends the program quietly, where MicroPython raises
 *     KeyboardInterrupt; the client is closed in both.
 */
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_fmt.h"
#include "bento_mqtt.h"
#include "bento_pots.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_sensors.h"
#include "bento_time.h"
#include "bento_wifi.h"

/* ---- 1) Settings: edit these before running ---- */
/* WIFI_SSID and WIFI_PASS: edit them at the top of join_wifi(). */
/* CROP: edit it at the top of run(). */
/* TEMP_OFFSET: edit it at the top of read_climate(). */
static const char TEAM[] = "teamXX";

/* Thai name, English code, temperature and humidity limits and
 * the heat budget. A 27 C room is normal; VR3 half way (+5) warms. */
typedef struct {
    const char *name;
    const char *code;
    int32_t temp_low_c;
    int32_t temp_high_c;
    int32_t rh_low;
    int32_t rh_high;
    int32_t budget_s; /* seconds the fruit stands above the limit */
} crop_t;

enum {
    CROP_COUNT = 3
};

static const crop_t CROPS[CROP_COUNT] = {
    {"มะม่วง", "mango", 15, 30, 20, 95, 180},
    {"สตรอว์เบอร์รี", "strawberry", 15, 28, 20, 95, 90},
    {"ทุเรียน", "durian", 15, 32, 20, 95, 240},
};

/* Times, limits and the hardware. */
enum {
    T_HI_MAX = 45, /* the highest limit "set" may give */
    READ_MS = 1000,
    BEEP_MS = 5000,
    POLL_MS = 20,
    RUN_MS = 600000,
    SETTLE_MS = 200, /* let the cleared screen settle */
    CONNECT_POLL_MS = 20,
    MS_PER_S = 1000,
    THERMO_MAX = 40, /* the thermometer shows 0-40 C */
    SPEAKER = 40,
    VOLUME = 25,
    NOTE_MS = 90,
    NOTE_GAP_MS = 100,
    MATRIX_MS = 80,
    BROKER_PORT = 1883,
    KEEPALIVE_S = 60,
    BROKER_TRIES = 3,
    CLIENT_SUFFIX_MASK = 0xFFFF,
    SUN_POT = 2, /* VR3 */
    SUN_MAX_C = 10,
    POT_FULL = 4095,
    ANSWER_BUTTON = 1, /* SW5 */
    FAR_C = 3,         /* this far past a limit is level 2 */
    FAR_RH = 10,
    PERCENT = 100,
    BUDGET_GOOD_PCT = 50,
    TEAM_LENGTH = 6,
    TEAM_PREFIX_LENGTH = 4
};

/* The ice theme, 0xRRGGBB. */
enum {
    COL_TEXT = 0xEAF6FF,
    COL_DIM = 0x8FA9BF,
    COL_CARD = 0x0E1A2B,
    COL_EDGE = 0x2C4A66,
    COL_ICE = 0x7FD3FF,
    COL_WARN = 0xFFB547,
    COL_BAD = 0xFF5A5F,
    COL_OK = 0x3DD6C6
};

/* The layout: the thermometer, its scale and the message box. */
enum {
    THERMO_X = 44,
    THERMO_Y = 84,
    THERMO_W = 34,
    THERMO_H = 236,
    SCALE_X = THERMO_X + THERMO_W + 8,
    MARK_LIFT = 8,
    BOX_X = 196,
    BOX_Y = 96,
    BOX_W = 380,
    BOX_H = 150,
    CARD_RADIUS = 10,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    CARD_TITLE_PX = 20,
    AUTO = -1 /* the factory's own size */
};

/* Where the emulator's note goes: free space on this screen. */
enum {
    GAP_X = 316,
    GAP_Y = 346,
    GAP_W = 360, /* the note itself breaks into two lines */
    GAP_PX = 20
};

/* Buffer sizes, in bytes. */
enum {
    TEXT_BYTES = 128, /* a label or box text: 126 at most */
    WHY_BYTES = 64,   /* two Thai problems and " + " */
    TOPIC_BYTES = 48,
    CLIENT_BYTES = 32,
    PAYLOAD_BYTES = 256, /* the binding keeps 255 and a NUL */
    TOPIC_KEEP = 64,     /* the program never reads the topic */
    JSON_BYTES = 256
};

/* Levels: 0 cool, 1 warming, 2 too hot (far past a limit). */
enum {
    LEVEL_NONE = -1,
    LEVEL_OK = 0,
    LEVEL_WARM = 1,
    LEVEL_BAD = 2,
    LEVEL_COUNT = 3
};

/* The tunes, by name. */
enum {
    TUNE_TAP = 0,
    TUNE_GOOD,
    TUNE_BAD,
    TUNE_EMPTY,
    TUNE_COUNT
};
/* The longest tune, in notes. */
enum {
    TUNE_NOTES_MAX = 3
};

/* How each level looks and sounds. */
typedef struct {
    uint32_t color;
    const char *mood; /* the first words of the mood line */
    const char *matrix;
    int32_t matrix_color;
    int32_t tune;
} level_look_t;

static const level_look_t LEVELS[LEVEL_COUNT] = {
    {COL_ICE, "เย็นดี", "COOL", BENTO_RGBMATRIX_CYAN, TUNE_GOOD},
    {COL_WARN, "เริ่มอุ่น: ", "WARM", BENTO_RGBMATRIX_YELLOW, TUNE_TAP},
    {COL_BAD, "ร้อนเกิน!: ", "HOT", BENTO_RGBMATRIX_RED, TUNE_EMPTY},
};

/* The widgets the program changes after drawing them. */
enum {
    SLOT_THERMO = 0,
    SLOT_MARK,
    SLOT_CROP,
    SLOT_SEG,
    SLOT_RH,
    SLOT_MOOD,
    SLOT_PEAK,
    SLOT_ARC,
    SLOT_LEFT,
    SLOT_USED,
    SLOT_NOTICE,
    SLOT_LED_OK,
    SLOT_LED_WARM,
    SLOT_LED_BAD,
    SLOT_STATUS,
    SLOT_LINK_LED,
    SLOT_LINK_TEXT,
    SLOT_COUNT
};
/* A widget nobody changes. */
enum {
    SLOT_NONE = SLOT_COUNT
};

/* One widget as the factory takes it. */
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
} widget_spec_t;

/* A dark card with its title. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    const char *title;
} card_t;

typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t ids[SLOT_COUNT + 1]; /* + the SLOT_NONE scratch */
    bento_ui_id_t box;                 /* 0 when no box is open */
} screen_t;

typedef struct {
    int32_t temp_low_c;
    int32_t temp_high_c;
    int32_t rh_low;
    int32_t rh_high;
} limits_t;

/* The box being watched, and the link. */
typedef struct {
    const crop_t *crop;
    limits_t limits;
    int32_t level;
    int32_t alerts;
    int32_t budget_s;
    int32_t warm_s;     /* seconds above the limit so far */
    bento_real_t t_max; /* the highest since the start */
    bool has_reading;   /* false: read again at once */
    bool acknowledged;
    bool stopped;    /* the .py's raise Stop */
    bool no_mqtt;    /* the BENTO Emulator has no MQTT from C */
    uint32_t broker; /* the MQTT client, 0 when none */
    char topic_event[TOPIC_BYTES];
    char topic_cmd[TOPIC_BYTES];
} watch_t;

typedef struct {
    bento_real_t temp_c; /* with the offset and the outside heat */
    bento_real_t rh;
    int32_t sun_c;
} reading_t;

typedef struct {
    int32_t level;
    char why_thai[WHY_BYTES];
    char why_english[WHY_BYTES];
} verdict_t;

/* An incoming command: where its fields are in the payload. */
typedef struct {
    bool found;
    bool is_text;
    bool is_integer;
    uint32_t start; /* a text's bytes, inside the quotes */
    uint32_t end;
    int32_t number; /* above any limit when it does not fit */
} json_field_t;

typedef struct {
    const uint8_t *text;
    uint32_t length;
    uint32_t at;
    bool valid; /* false once the text is not JSON */
    json_field_t cmd;
    json_field_t t_hi;
} command_t;

/* ---- 2) Hardware ---- */

static bento_status_t rest(uint32_t wait_ms)
{
    bento_status_t status = bento_time_sleep_ms(wait_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* ui.poll() answers no events this program reads. */
static void drain_events(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

static bento_status_t beep(bento_ui_t *ui, int32_t tune)
{
    typedef struct {
        uint8_t notes[TUNE_NOTES_MAX];
        uint32_t count;
    } tune_t;
    static const tune_t tunes[TUNE_COUNT] = {
        {{76U, 0U, 0U}, 1U},
        {{72U, 79U, 84U}, 3U},
        {{84U, 76U, 0U}, 2U},
        {{84U, 76U, 69U}, 3U},
    };
    const tune_t *chosen = &tunes[tune];
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < chosen->count) && (status == BENTO_OK); i++) {
        /* ui.tone is fire-and-forget; a build without sound refuses */
        (void)bento_ui_sfx_tone(ui, chosen->notes[i],
                                (uint8_t)BENTO_UI_WAVE_SINE, VOLUME,
                                NOTE_MS);
        status = rest(NOTE_GAP_MS);
    }
    return status;
}

/* A board without the matrix just goes without it. */
static bento_status_t scroll_matrix(const char *text, int32_t color)
{
    bento_status_t status = bento_rgbmatrix_scroll(
        text, (uint32_t)strlen(text), color, MATRIX_MS);

    if (status == BENTO_E_UNSUPPORTED) {
        status = BENTO_OK;
    }
    return status;
}

/* VR3: heat from outside the box, 0-10 C. */
static bento_status_t read_sun(int32_t *sun_c)
{
    uint16_t raw = 0U;
    bento_status_t status = bento_pots_read(SUN_POT, &raw);

    *sun_c = ((int32_t)raw * SUN_MAX_C) / POT_FULL;
    if (status == BENTO_E_UNSUPPORTED) {
        *sun_c = 0;
        status = BENTO_OK;
    }
    return status;
}

/* Any failed read is "no reading": the next round tries again. */
static bool read_climate(reading_t *reading)
{
    /* the board reads above the room: an offset such as -7.0 */
    const bento_real_t temp_offset = (bento_real_t)0.0;
    float temp_c = 0.0F;
    float rh = 0.0F;
    bool read = bento_sensors_sht40_temperature(&temp_c) == BENTO_OK;

    read = read && (bento_sensors_sht40_humidity(&rh) == BENTO_OK);
    reading->temp_c = (bento_real_t)temp_c + temp_offset;
    reading->rh = (bento_real_t)rh;
    return read;
}

static bento_status_t read_button(bool *down)
{
    bento_status_t status = bento_buttons_pressed(ANSWER_BUTTON, down);

    if (status == BENTO_E_UNSUPPORTED) {
        *down = false;
        status = BENTO_OK;
    }
    return status;
}

/* ---- 3) The brain ---- */

static bool is_digit(uint8_t byte)
{
    return (byte >= (uint8_t)'0') && (byte <= (uint8_t)'9');
}

/* "team" and two digits, never the teacher's team00. */
static bool valid_team(const char *team)
{
    const uint32_t length = (uint32_t)strlen(team);
    bool valid = (length == (uint32_t)TEAM_LENGTH) &&
                 (strncmp(team, "team", TEAM_PREFIX_LENGTH) == 0);
    uint32_t i;

    for (i = TEAM_PREFIX_LENGTH; valid && (i < length); i++) {
        valid = is_digit((uint8_t)team[i]);
    }
    return valid && (strcmp(team, "team00") != 0);
}

static void join_word(char *text, uint32_t size, const char *separator,
                      const char *word)
{
    const uint32_t used = (uint32_t)strlen(text);

    if (used < size) {
        (void)snprintf(&text[used], size - used, "%s%s",
                       (used == 0U) ? "" : separator, word);
    }
}

/* Level 2 is far out: past a limit by 3 C or by 10 %RH. */
static void judge(const reading_t *reading, const limits_t *limits,
                  verdict_t *verdict)
{
    static const char *const problems[][2] = {
        {"เย็นจัด", "cold"},
        {"กล่องอุ่น", "hot"},
        {"แห้งไป", "dry"},
        {"ไอน้ำเกาะ", "wet"},
    };
    const bento_real_t temp_c = reading->temp_c;
    const bento_real_t rh = reading->rh;
    const bool found[] = {
        (temp_c < (bento_real_t)limits->temp_low_c),
        (temp_c > (bento_real_t)limits->temp_high_c),
        (rh < (bento_real_t)limits->rh_low),
        (rh > (bento_real_t)limits->rh_high),
    };
    const bool far =
        (temp_c < (bento_real_t)(limits->temp_low_c - FAR_C)) ||
        (temp_c > (bento_real_t)(limits->temp_high_c + FAR_C)) ||
        (rh < (bento_real_t)(limits->rh_low - FAR_RH)) ||
        (rh > (bento_real_t)(limits->rh_high + FAR_RH));
    uint32_t i;

    verdict->why_thai[0] = '\0';
    verdict->why_english[0] = '\0';
    for (i = 0U; i < (sizeof(found) / sizeof(found[0])); i++) {
        if (found[i]) {
            join_word(verdict->why_thai, WHY_BYTES, " + ",
                      problems[i][0]);
            join_word(verdict->why_english, WHY_BYTES, "+",
                      problems[i][1]);
        }
    }
    if (verdict->why_thai[0] == '\0') {
        verdict->level = LEVEL_OK;
        join_word(verdict->why_thai, WHY_BYTES, "", "เย็นดี");
        join_word(verdict->why_english, WHY_BYTES, "", "ok");
    } else {
        verdict->level = far ? LEVEL_BAD : LEVEL_WARM;
    }
}

/* round(x, 1) as MicroPython does it, in the board's float width. */
static bento_real_t round_tenth(bento_real_t value)
{
    const bento_real_t scale = (bento_real_t)10.0;
    const bento_real_t scaled = value * scale;

    return (bento_real_t)nearbyint((double)scaled) / scale;
}

/* str() of a real number, as json.dumps writes it. */
static void format_real(char *text, bento_real_t value)
{
    uint32_t length = 0U;

    /* a BENTO_FMT_BUF_SIZE buffer holds any real number */
    (void)bento_fmt_real(text, BENTO_FMT_BUF_SIZE, value, &length);
}

/* "%.1f" of a real number. */
static void format_tenths(char *text, bento_real_t value)
{
    uint32_t length = 0U;

    (void)bento_fmt_real_precision(text, BENTO_FMT_BUF_SIZE, value, 1,
                                   &length);
}

/* ---- 4) The network ---- */

/* wifi.connect(): any refusal is "not joined", Stop is Stop. */
static bento_status_t join_wifi(bool *joined)
{
    /* edit these two: short, English or digits; 8 or more, share no
     * real one */
    static const char wifi_ssid[] = "<ชื่อ Hotspot ของกลุ่ม>";
    static const char wifi_pass[] = "<รหัส Hotspot ของกลุ่ม>";
    bento_wifi_connect_params_t params;
    uint32_t address = 0U;
    bento_status_t status;

    bento_wifi_connect_params_init(&params);
    params.ssid = wifi_ssid;
    params.ssid_len = (uint32_t)strlen(wifi_ssid);
    params.pass = wifi_pass;
    params.pass_len = (uint32_t)strlen(wifi_pass);
    params.flags = BENTO_WIFI_F_SAVE_CREDS; /* the binding saves too */
    status = bento_wifi_connect_start(&params);
    if (status == BENTO_OK) {
        status = BENTO_E_IN_PROGRESS;
    }
    while (status == BENTO_E_IN_PROGRESS) {
        status = rest(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_wifi_connect_poll();
        } else { /* Stop between two polls: leave the radio alone */
            (void)bento_wifi_connect_cancel(); /* nothing to report */
        }
    }
    *joined = status == BENTO_OK;
    if (*joined &&
        (bento_wifi_ip(BENTO_WIFI_IF_ANY, &address) != BENTO_OK)) {
        address = 0U; /* an address that cannot be read is 0.0.0.0 */
    }
    *joined = *joined && (address != 0U);
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

static bento_status_t wait_for_broker(uint32_t broker)
{
    bento_status_t status = BENTO_E_IN_PROGRESS;

    while (status == BENTO_E_IN_PROGRESS) {
        status = rest(CONNECT_POLL_MS);
        if (status == BENTO_OK) {
            status = bento_mqtt_open_poll(broker);
        }
    }
    return status;
}

/* One mqtt.connect() with a new client id, so an old session on
 * the broker never clashes with it. */
static bento_status_t try_broker(watch_t *watch, bool *linked)
{
    static const char broker[] = "broker.hivemq.com";
    static const char client_base[] =
        "bento-farm-"; /* + TEAM + a suffix */
    char client_id[CLIENT_BYTES];
    bento_mqtt_config_t config;
    const uint32_t suffix =
        bento_time_ticks_ms() & (uint32_t)CLIENT_SUFFIX_MASK;
    bento_status_t status;

    (void)snprintf(client_id, sizeof(client_id), "%s%s-%04" PRIx32,
                   client_base, TEAM, suffix);
    bento_mqtt_config_init(&config);
    config.host = broker;
    config.host_len = (uint32_t)strlen(broker);
    config.port = (uint16_t)BROKER_PORT;
    config.keepalive_s = (uint16_t)KEEPALIVE_S;
    config.client_id = client_id;
    config.client_id_len = (uint32_t)strlen(client_id);
    status = bento_mqtt_open_start(&config, &watch->broker);
    watch->no_mqtt = status == BENTO_E_UNSUPPORTED;
    if (status == BENTO_OK) {
        status = wait_for_broker(watch->broker);
    }
    *linked = status == BENTO_OK;
    if (!*linked && (watch->broker != 0U)) {
        (void)bento_mqtt_close(watch->broker); /* a failed attempt */
        watch->broker = 0U;
    }
    if (status != BENTO_E_CANCELLED) {
        status = BENTO_OK;
    }
    return status;
}

/* ---- 5) The screen ---- */

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

/* The widget setters ignore a refusal in MicroPython (modui.c). */
static void show_text(const screen_t *screen, uint32_t slot,
                      const char *text)
{
    (void)bento_ui_set_text(screen->ui, screen->ids[slot], text,
                            (uint32_t)strlen(text));
}

static void show_color(const screen_t *screen, uint32_t slot,
                       uint32_t color)
{
    (void)bento_ui_set_color(screen->ui, screen->ids[slot], color);
}

static void show_value(const screen_t *screen, uint32_t slot,
                       int32_t value)
{
    (void)bento_ui_set_value(screen->ui, screen->ids[slot], value);
}

static void show_status(const screen_t *screen, const char *text,
                        uint32_t color)
{
    show_color(screen, SLOT_STATUS, color);
    show_text(screen, SLOT_STATUS, text);
}

static void show_link(const screen_t *screen, bool linked)
{
    show_value(screen, SLOT_LINK_LED, linked ? 1 : 0);
    show_text(screen, SLOT_LINK_TEXT,
              linked ? "MQTT: ต่อแล้ว" : "MQTT: ออฟไลน์");
}

static bento_status_t add_widget(screen_t *screen,
                                 const widget_spec_t *spec)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    if (spec->text != NULL) {
        args.text = spec->text;
        args.text_len = (uint32_t)strlen(spec->text);
    }
    args.x = spec->x;
    args.y = spec->y;
    args.w = spec->w;
    args.h = spec->h;
    args.color = spec->color;
    args.min_val = spec->minimum;
    args.max_val = spec->maximum;
    args.font_px = spec->value;
    return bento_ui_create(screen->ui, spec->kind, &args,
                           &screen->ids[spec->slot]);
}

static bento_status_t add_widgets(screen_t *screen,
                                  const widget_spec_t *specs,
                                  uint32_t count)
{
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < count) && (status == BENTO_OK); i++) {
        status = add_widget(screen, &specs[i]);
    }
    return status;
}

static bento_status_t add_card(screen_t *screen, const card_t *card)
{
    const widget_spec_t panel = {UI_WIDGET_PANEL, SLOT_NONE, NULL,
                                 card->x,         card->y,   card->w,
                                 card->h,         COL_CARD,  COL_EDGE,
                                 CARD_RADIUS,     1};
    const widget_spec_t title = {UI_WIDGET_LABEL,
                                 SLOT_NONE,
                                 card->title,
                                 px(card->x + CARD_TITLE_DX),
                                 px(card->y + CARD_TITLE_DY),
                                 AUTO,
                                 AUTO,
                                 COL_ICE,
                                 0,
                                 0,
                                 CARD_TITLE_PX};
    bento_status_t status = add_widget(screen, &panel);

    if (status == BENTO_OK) {
        status = add_widget(screen, &title);
    }
    return status;
}

static bento_status_t add_card_with(screen_t *screen,
                                    const card_t *card,
                                    const widget_spec_t *specs,
                                    uint32_t count)
{
    bento_status_t status = add_card(screen, card);

    if (status == BENTO_OK) {
        status = add_widgets(screen, specs, count);
    }
    return status;
}

/* The title and the thermometer card. */
static bento_status_t add_thermo_card(screen_t *screen)
{
    static const widget_spec_t title = {UI_WIDGET_LABEL,
                                        SLOT_NONE,
                                        "กล่องผลไม้เย็น: นับเวลาร้อนสะสม",
                                        12,
                                        6,
                                        AUTO,
                                        AUTO,
                                        COL_TEXT,
                                        0,
                                        0,
                                        24};
    static const card_t card = {12, 44, 168, 300, "ปรอทในกล่อง"};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_BAR, SLOT_THERMO, NULL, THERMO_X, THERMO_Y, THERMO_W,
         THERMO_H, 0U, 0, THERMO_MAX, 0},
        {UI_WIDGET_LABEL, SLOT_NONE, "40 C", SCALE_X, THERMO_Y - 8,
         AUTO, AUTO, COL_DIM, 0, 0, 14},
        {UI_WIDGET_LABEL, SLOT_NONE, "0 C", SCALE_X,
         THERMO_Y + THERMO_H - 12, AUTO, AUTO, COL_DIM, 0, 0, 14},
        {UI_WIDGET_LABEL, SLOT_MARK, "< เกณฑ์", SCALE_X, THERMO_Y, AUTO,
         AUTO, COL_BAD, 0, 0, 20},
    };
    bento_status_t status = add_widget(screen, &title);

    if (status == BENTO_OK) {
        status = add_card_with(screen, &card, specs,
                               sizeof(specs) / sizeof(specs[0]));
    }
    return status;
}

static bento_status_t add_now_card(screen_t *screen)
{
    static const card_t card = {190, 44, 410, 196, "ในกล่องตอนนี้"};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_LABEL, SLOT_CROP, "-", 202, 80, AUTO, AUTO, COL_DIM,
         0, 0, 20},
        {UI_WIDGET_SEG7, SLOT_SEG, "--.-", 202, 110, 180, 64, COL_ICE,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE, "C", 392, 136, AUTO, AUTO, COL_DIM,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_RH, "RH -- %", 420, 136, AUTO, AUTO,
         COL_DIM, 0, 0, 16},
        {UI_WIDGET_LABEL, SLOT_MOOD, "...", 202, 178, AUTO, AUTO,
         COL_WARN, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_PEAK, "สูงสุด -- C", 202, 206, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
    };

    return add_card_with(screen, &card, specs,
                         sizeof(specs) / sizeof(specs[0]));
}

static bento_status_t add_budget_card(screen_t *screen)
{
    static const card_t card = {610, 44, 170, 196, "งบความร้อนผลไม้"};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_ARC, SLOT_ARC, NULL, 645, 78, 100, 100, 0U, 0,
         PERCENT, PERCENT},
        {UI_WIDGET_LABEL, SLOT_LEFT, "เหลือ -- วิ", 622, 180, AUTO, AUTO,
         COL_TEXT, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_USED, "ใช้ไป 0 วิ", 622, 208, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
    };

    return add_card_with(screen, &card, specs,
                         sizeof(specs) / sizeof(specs[0]));
}

/* The driver's card, the status line and the link lamp. */
static bento_status_t add_driver_card(screen_t *screen)
{
    static const card_t card = {190, 244, 590, 100, "บอกคนขับ"};
    static const widget_spec_t specs[] = {
        {UI_WIDGET_LED, SLOT_LED_OK, NULL, 202, 285, 18, 18, COL_ICE, 0,
         0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE, "เย็นดี", 226, 280, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LED, SLOT_LED_WARM, NULL, 292, 285, 18, 18, COL_WARN,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE, "เริ่มอุ่น", 316, 280, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LED, SLOT_LED_BAD, NULL, 382, 285, 18, 18, COL_BAD,
         0, 0, 0},
        {UI_WIDGET_LABEL, SLOT_NONE, "ร้อนเกิน", 406, 280, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_NONE,
         "VR3 = ร้อนจากนอกกล่อง  SW5 = รับทราบ", 202, 312, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_NOTICE, "-", 480, 280, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LABEL, SLOT_STATUS, "กำลังเริ่ม", 12, 352, AUTO, AUTO,
         COL_DIM, 0, 0, 20},
        {UI_WIDGET_LED, SLOT_LINK_LED, NULL, 606, 12, 18, 18, COL_OK, 0,
         0, 0},
        {UI_WIDGET_LABEL, SLOT_LINK_TEXT, "MQTT: ออฟไลน์", 632, 10, AUTO,
         AUTO, COL_DIM, 0, 0, 20},
    };

    return add_card_with(screen, &card, specs,
                         sizeof(specs) / sizeof(specs[0]));
}

/* The whole screen in the .py's order. The bar's and the ring's
 * first colours are set once everything is drawn: the same look. */
static bento_status_t build_screen(screen_t *screen)
{
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = rest(SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_thermo_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_now_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_budget_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_driver_card(screen);
    }
    if (status == BENTO_OK) {
        /* on the Emulator a Bar's colour paints its track */
        show_color(screen, SLOT_THERMO, COL_ICE);
        show_color(screen, SLOT_ARC, COL_OK);
        drain_events(screen->ui);
    }
    return status;
}

static void show_notice(const screen_t *screen, const watch_t *watch,
                        const char *text, uint32_t color)
{
    char line[TEXT_BYTES];

    (void)snprintf(line, sizeof(line), "แจ้งไปแล้ว %" PRId32 " ครั้ง: %s",
                   watch->alerts, text);
    show_color(screen, SLOT_NOTICE, color);
    show_text(screen, SLOT_NOTICE, line);
}

/* The thermometer's y for a temperature. */
static int32_t thermo_y(int32_t temp_c)
{
    int32_t shown = (temp_c < 0) ? 0 : temp_c;

    shown = (shown > THERMO_MAX) ? THERMO_MAX : shown;
    return ((int32_t)THERMO_Y + (int32_t)THERMO_H) -
           ((shown * (int32_t)THERMO_H) / (int32_t)THERMO_MAX);
}

static void show_crop(const screen_t *screen, const watch_t *watch)
{
    char line[TEXT_BYTES];

    (void)snprintf(line, sizeof(line),
                   "%s เก็บที่ %" PRId32 "-%" PRId32 " C ทนอุ่นได้ %" PRId32
                   " วิ",
                   watch->crop->name, watch->limits.temp_low_c,
                   watch->limits.temp_high_c, watch->budget_s);
    show_text(screen, SLOT_CROP, line);
    /* a widget's pos() ignores a refusal too */
    (void)bento_ui_set_pos(
        screen->ui, screen->ids[SLOT_MARK], SCALE_X,
        px(thermo_y(watch->limits.temp_high_c) - MARK_LIFT));
}

static void show_budget(const screen_t *screen, const watch_t *watch)
{
    char line[TEXT_BYTES];
    const int32_t over = watch->budget_s - watch->warm_s;
    const int32_t left_s = (over > 0) ? over : 0;
    const int32_t left_pct = (left_s * PERCENT) / watch->budget_s;
    uint32_t color = COL_BAD;

    if (left_pct > BUDGET_GOOD_PCT) {
        color = COL_OK;
    } else if (left_pct > 0) {
        color = COL_WARN;
    } else {
        /* the budget is gone */
    }
    show_value(screen, SLOT_ARC, left_pct);
    show_color(screen, SLOT_ARC, color);
    if (left_s != 0) {
        (void)snprintf(line, sizeof(line), "เหลือ %" PRId32 " วิ",
                       left_s);
        show_text(screen, SLOT_LEFT, line);
    } else {
        show_text(screen, SLOT_LEFT, "หมดงบ!");
    }
    (void)snprintf(line, sizeof(line), "ใช้ไป %" PRId32 " วิ",
                   watch->warm_s);
    show_text(screen, SLOT_USED, line);
}

/* The digits, the humidity and the thermometer. */
static void show_climate(const screen_t *screen,
                         const reading_t *reading, int32_t level)
{
    char number[BENTO_FMT_BUF_SIZE];
    char line[TEXT_BYTES];
    const bento_real_t top = (bento_real_t)THERMO_MAX;
    bento_real_t shown =
        (reading->temp_c < top) ? reading->temp_c : top;

    shown = (shown > (bento_real_t)0.0) ? shown : (bento_real_t)0.0;
    format_tenths(number, reading->temp_c);
    show_text(screen, SLOT_SEG, number);
    show_color(screen, SLOT_SEG, LEVELS[level].color);
    (void)snprintf(line, sizeof(line), "RH %" PRId32 " %%",
                   (int32_t)reading->rh);
    show_text(screen, SLOT_RH, line);
    show_value(screen, SLOT_THERMO, (int32_t)shown);
    show_color(screen, SLOT_THERMO, LEVELS[level].color);
}

/* The mood line, the highest so far and the three lamps. */
static void show_mood(const screen_t *screen, const watch_t *watch,
                      const reading_t *reading,
                      const verdict_t *verdict)
{
    char number[BENTO_FMT_BUF_SIZE];
    char line[TEXT_BYTES];
    const int32_t level = verdict->level;
    int32_t i;

    show_color(screen, SLOT_MOOD, LEVELS[level].color);
    (void)snprintf(line, sizeof(line), "%s%s  (นอกกล่อง +%" PRId32 ")",
                   LEVELS[level].mood,
                   (level == LEVEL_OK) ? "" : verdict->why_thai,
                   reading->sun_c);
    show_text(screen, SLOT_MOOD, line);
    format_tenths(number, watch->t_max);
    (void)snprintf(line, sizeof(line), "สูงสุดตั้งแต่ออกรถ %s C", number);
    show_text(screen, SLOT_PEAK, line);
    for (i = 0; i < LEVEL_COUNT; i++) {
        show_value(screen, (uint32_t)SLOT_LED_OK + (uint32_t)i,
                   (i == level) ? 1 : 0);
    }
}

static void close_box(screen_t *screen)
{
    if (screen->box != 0U) {
        /* delete() ignores a refusal in MicroPython (modui.c) */
        (void)bento_ui_delete(screen->ui, screen->box);
        screen->box = 0U;
    }
}

/* The box takes 126 bytes at most; the longest text here fits. */
static bento_status_t open_box(screen_t *screen, const char *why)
{
    char text[TEXT_BYTES];
    bento_ui_widget_args_t args;

    (void)snprintf(text, sizeof(text), "กล่องร้อน!\n%s\nSW5 = รับทราบ",
                   why);
    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = BOX_X;
    args.y = BOX_Y;
    args.w = BOX_W;
    args.h = BOX_H;
    args.font_px = 0;
    return bento_ui_msgbox_create(screen->ui, &args, &screen->box);
}

/* ---- 6) The program ---- */

/* The .py's stop(): say why on the screen, then end politely. */
static void stop_here(screen_t *screen, watch_t *watch,
                      const char *text, uint32_t color)
{
    show_link(screen, false);
    /* the program ends either way: the matrix is only tidied */
    (void)bento_rgbmatrix_scroll(NULL, 0U,
                                 BENTO_RGBMATRIX_SCROLL_COLOR_DEFAULT,
                                 BENTO_RGBMATRIX_SCROLL_MS_DEFAULT);
    (void)bento_rgbmatrix_clear();
    close_box(screen);
    show_status(screen, text, color);
    drain_events(screen->ui);
    watch->stopped = true;
}

static void start_watch(watch_t *watch, const crop_t *crop)
{
    const bento_real_t never_read = (bento_real_t)-99.0;

    watch->crop = crop;
    watch->limits.temp_low_c = crop->temp_low_c;
    watch->limits.temp_high_c = crop->temp_high_c;
    watch->limits.rh_low = crop->rh_low;
    watch->limits.rh_high = crop->rh_high;
    watch->budget_s = crop->budget_s;
    watch->level = LEVEL_NONE;
    watch->acknowledged = true;
    watch->alerts = 0;
    watch->has_reading = false;
    watch->warm_s = 0;
    watch->t_max = never_read;
}

/* json.dumps of the event; false only when the link is gone. */
static bool send_event(const watch_t *watch, const reading_t *reading,
                       const verdict_t *verdict)
{
    char json[JSON_BYTES];
    char temp_c[BENTO_FMT_BUF_SIZE];
    char rh[BENTO_FMT_BUF_SIZE];
    char t_max[BENTO_FMT_BUF_SIZE];
    bento_status_t status;

    format_real(temp_c, round_tenth(reading->temp_c));
    format_real(rh, round_tenth(reading->rh));
    format_real(t_max, round_tenth(watch->t_max));
    (void)snprintf(
        json, sizeof(json),
        "{\"id\": \"%s\", \"crop\": \"%s\", \"level\": %" PRId32
        ", \"alert\": \"%s\", \"temp_c\": %s, \"rh\": %s, "
        "\"sun_c\": %" PRId32 ", \"warm_s\": %" PRId32
        ", \"t_max\": %s}",
        TEAM, watch->crop->code, verdict->level, verdict->why_english,
        temp_c, rh, reading->sun_c, watch->warm_s, t_max);
    status = bento_mqtt_publish(watch->broker, watch->topic_event,
                                (uint32_t)strlen(watch->topic_event),
                                (const uint8_t *)json,
                                (uint32_t)strlen(json), 0U, 0U);
    return (status != BENTO_E_SESSION) && (status != BENTO_E_STALE);
}

/* The matrix, the tune, the notice and, when too hot, the box. */
static bento_status_t announce_level(screen_t *screen,
                                     const watch_t *watch,
                                     const verdict_t *verdict)
{
    const int32_t level = verdict->level;
    const bool hot = level == LEVEL_BAD;
    bento_status_t status =
        scroll_matrix(LEVELS[level].matrix, LEVELS[level].matrix_color);

    if (status == BENTO_OK) {
        status = beep(screen->ui, LEVELS[level].tune);
    }
    if (status == BENTO_OK) {
        show_notice(screen, watch, hot ? "รอคนขับ" : "ส่งแล้ว",
                    hot ? COL_BAD : COL_DIM);
        close_box(screen);
    }
    if ((status == BENTO_OK) && hot) {
        status = open_box(screen, verdict->why_thai);
    }
    return status;
}

/* The level changed: send first, then the matrix, the tune and the
 * box. */
static bento_status_t on_level(screen_t *screen, watch_t *watch,
                               const reading_t *reading,
                               const verdict_t *verdict)
{
    const bool hot = verdict->level == LEVEL_BAD;
    bento_status_t status = BENTO_OK;

    watch->level = verdict->level;
    watch->acknowledged = !hot;
    watch->alerts++;
    if (!send_event(watch, reading, verdict)) {
        stop_here(screen, watch, "สายหลุดตอนส่ง", COL_BAD);
    } else {
        status = announce_level(screen, watch, verdict);
    }
    return status;
}

/* One reading; *alerted when the level changed and went out. */
static bento_status_t read_and_judge(screen_t *screen, watch_t *watch,
                                     bool *alerted)
{
    reading_t reading = {(bento_real_t)0.0, (bento_real_t)0.0, 0};
    verdict_t verdict;
    bento_status_t status = read_sun(&reading.sun_c);

    *alerted = false;
    watch->has_reading = (status == BENTO_OK) && read_climate(&reading);
    if (watch->has_reading) {
        reading.temp_c += (bento_real_t)reading.sun_c;
        if (reading.temp_c > watch->t_max) {
            watch->t_max = reading.temp_c;
        }
        if (reading.temp_c > (bento_real_t)watch->limits.temp_high_c) {
            watch->warm_s += READ_MS / MS_PER_S; /* read every second */
        }
        judge(&reading, &watch->limits, &verdict);
        show_climate(screen, &reading, verdict.level);
        show_mood(screen, watch, &reading, &verdict);
        show_budget(screen, watch);
        if (verdict.level != watch->level) {
            *alerted = true;
            status = on_level(screen, watch, &reading, &verdict);
        }
    }
    return status;
}

/* ---- Reading an incoming command ---- */

static bool is_space(uint8_t byte)
{
    return (byte == (uint8_t)' ') || (byte == (uint8_t)'\t') ||
           (byte == (uint8_t)'\n') || (byte == (uint8_t)'\r');
}

/* The byte under the reader, 0 past the end. */
static uint8_t peek(const command_t *command)
{
    return (command->at < command->length) ? command->text[command->at]
                                           : 0U;
}

static void skip_space(command_t *command)
{
    while ((command->at < command->length) &&
           is_space(command->text[command->at])) {
        command->at++;
    }
}

static void expect(command_t *command, uint8_t wanted)
{
    skip_space(command);
    if (peek(command) == wanted) {
        command->at++;
    } else {
        command->valid = false;
    }
}

/* A string: the reader stands on its opening quote. */
static void read_string(command_t *command, json_field_t *field)
{
    bool closed = false;

    command->at++;
    field->start = command->at;
    while (command->valid && !closed) {
        const uint8_t byte = peek(command);

        if ((command->at >= command->length) || (byte < (uint8_t)' ')) {
            command->valid = false;
        } else if (byte == (uint8_t)'"') {
            closed = true;
            field->end = command->at;
            command->at++;
        } else {
            command->at += (byte == (uint8_t)'\\') ? 2U : 1U;
        }
    }
    field->is_text = true;
}

static void read_digits(command_t *command, json_field_t *field)
{
    const int32_t too_big = 1000000; /* past every limit asked for */
    const int32_t base = 10;
    bool any = false;

    while (is_digit(peek(command))) {
        if (field->number <= too_big) {
            field->number = (field->number * base) +
                            (int32_t)peek(command) - (int32_t)'0';
        }
        any = true;
        command->at++;
    }
    command->valid = command->valid && any;
}

/* A number; an integer when it has no fraction and no exponent. */
static void read_number(command_t *command, json_field_t *field)
{
    const bool negative = peek(command) == (uint8_t)'-';

    if (negative) {
        command->at++;
    }
    field->number = 0;
    read_digits(command, field);
    field->is_integer = true;
    if (peek(command) == (uint8_t)'.') {
        command->at++;
        read_digits(command, field);
        field->is_integer = false;
    }
    if ((peek(command) == (uint8_t)'e') ||
        (peek(command) == (uint8_t)'E')) {
        command->at++;
        if ((peek(command) == (uint8_t)'+') ||
            (peek(command) == (uint8_t)'-')) {
            command->at++;
        }
        read_digits(command, field);
        field->is_integer = false;
    }
    field->number = negative ? -field->number : field->number;
}

/* true, false or null: every letter must be there. */
static void read_word(command_t *command)
{
    static const char *const words[] = {"true", "false", "null"};
    bool known = false;
    uint32_t i;

    for (i = 0U; !known && (i < (sizeof(words) / sizeof(words[0])));
         i++) {
        const uint32_t length = (uint32_t)strlen(words[i]);

        known = ((command->length - command->at) >= length) &&
                (strncmp((const char *)&command->text[command->at],
                         words[i], length) == 0);
        if (known) {
            command->at += length;
        }
    }
    command->valid = known;
}

/* A nested list or object: its strings and brackets are checked. */
static void skip_nested(command_t *command)
{
    json_field_t ignored = {false, false, false, 0U, 0U, 0};
    int32_t depth = 0;

    do {
        const uint8_t byte = peek(command);

        if (command->at >= command->length) {
            command->valid = false;
        } else if (byte == (uint8_t)'"') {
            read_string(command, &ignored);
        } else {
            depth += ((byte == (uint8_t)'{') || (byte == (uint8_t)'['))
                         ? 1
                         : 0;
            depth -= ((byte == (uint8_t)'}') || (byte == (uint8_t)']'))
                         ? 1
                         : 0;
            command->at++;
        }
    } while (command->valid && (depth > 0));
}

static void read_value(command_t *command, json_field_t *field)
{
    const uint8_t byte = peek(command);

    field->found = true;
    if (byte == (uint8_t)'"') {
        read_string(command, field);
    } else if ((byte == (uint8_t)'{') || (byte == (uint8_t)'[')) {
        skip_nested(command);
    } else if ((byte == (uint8_t)'-') || is_digit(byte)) {
        read_number(command, field);
    } else {
        read_word(command);
    }
}

static bool text_is(const command_t *command, const json_field_t *field,
                    const char *wanted)
{
    const uint32_t length = (uint32_t)strlen(wanted);

    return field->is_text && ((field->end - field->start) == length) &&
           (strncmp((const char *)&command->text[field->start], wanted,
                    length) == 0);
}

/* One "key": value pair; the value lands in the field it names. */
static void read_member(command_t *command)
{
    json_field_t key = {false, false, false, 0U, 0U, 0};
    json_field_t value = {false, false, false, 0U, 0U, 0};

    skip_space(command);
    if (peek(command) == (uint8_t)'"') {
        read_string(command, &key);
    } else {
        command->valid = false;
    }
    expect(command, (uint8_t)':');
    skip_space(command);
    if (command->valid) {
        read_value(command, &value);
    }
    if (command->valid && text_is(command, &key, "cmd")) {
        command->cmd = value;
    }
    if (command->valid && text_is(command, &key, "t_hi")) {
        command->t_hi = value;
    }
}

/* json.loads of one object; command->valid says if it was one. */
static void read_command(command_t *command)
{
    bool more = true;

    expect(command, (uint8_t)'{');
    skip_space(command);
    if (command->valid && (peek(command) == (uint8_t)'}')) {
        command->at++;
        more = false;
    }
    while (command->valid && more) {
        read_member(command);
        skip_space(command);
        more = peek(command) == (uint8_t)',';
        if (more) {
            command->at++;
        } else {
            expect(command, (uint8_t)'}');
        }
    }
    skip_space(command);
    command->valid = command->valid && (command->at == command->length);
}

/* {"cmd":"set","t_hi":..}: a new limit, an integer in a sensible
 * range; the box is judged again and reported at once. */
static bento_status_t set_limit(screen_t *screen, watch_t *watch,
                                const json_field_t *t_hi)
{
    bento_status_t status = BENTO_OK;

    if (t_hi->found && t_hi->is_integer &&
        (watch->limits.temp_low_c < t_hi->number) &&
        (t_hi->number <= T_HI_MAX)) {
        watch->limits.temp_high_c = t_hi->number;
        watch->level = LEVEL_NONE;
        watch->has_reading = false;
        show_crop(screen, watch);
        status = beep(screen->ui, TUNE_TAP);
    }
    return status;
}

/* A command from the app; *answered for an ack, an unknown one is
 * silent. Room for the team's "led" and "pump" commands here. */
static bento_status_t on_command(screen_t *screen, watch_t *watch,
                                 const uint8_t *payload,
                                 uint32_t length, bool *answered)
{
    command_t command;
    bento_status_t status = BENTO_OK;

    (void)memset(&command, 0, sizeof(command));
    command.text = payload;
    command.length = length;
    command.valid = true;
    read_command(&command);
    *answered = command.valid && text_is(&command, &command.cmd, "ack");
    if (command.valid && text_is(&command, &command.cmd, "set")) {
        status = set_limit(screen, watch, &command.t_hi);
    }
    return status;
}

/* get_message(): the newest message waiting, if any. */
static bento_status_t take_command(screen_t *screen, watch_t *watch,
                                   bool *answered)
{
    uint8_t payload[PAYLOAD_BYTES];
    char topic[TOPIC_KEEP];
    bento_mqtt_msg_t info;
    uint32_t length = 0U;
    bool found = false;
    bool more = true;
    bento_status_t status = BENTO_OK;

    while (more) {
        bento_mqtt_msg_init(&info);
        more =
            bento_mqtt_poll(watch->broker, &info, topic, sizeof(topic),
                            payload, PAYLOAD_BYTES - 1U) == BENTO_OK;
        if (more) {
            length = info.payload_len;
            found = true;
        }
    }
    *answered = false;
    if (found) {
        status = on_command(screen, watch, payload, length, answered);
    }
    return status;
}

/* SW5: the driver answers without the network. */
static bento_status_t check_button(screen_t *screen, watch_t *watch,
                                   bool *was_down, bool *answered)
{
    bool down = false;
    bento_status_t status = read_button(&down);

    if ((status == BENTO_OK) && down && !*was_down) {
        *answered = true;
        if (watch
                ->acknowledged) { /* nothing to answer: show it works */
            show_notice(screen, watch, "ยังไม่มีเหตุเตือน", COL_DIM);
            status = beep(screen->ui, TUNE_TAP);
        }
    }
    *was_down = down;
    return status;
}

/* The alarm is answered: quiet, box closed, ACK on the matrix. */
static bento_status_t acknowledge(screen_t *screen, watch_t *watch)
{
    bento_status_t status;

    watch->acknowledged = true;
    close_box(screen);
    show_notice(screen, watch, "รับทราบแล้ว", COL_OK);
    status = scroll_matrix("ACK", BENTO_RGBMATRIX_CYAN);
    if (status == BENTO_OK) {
        status = beep(screen->ui, TUNE_TAP);
    }
    return status;
}

typedef struct {
    uint32_t read_ms;
    uint32_t beep_ms;
} timers_t;

/* Read every second (at once when there is no reading), and beep
 * again every 5 s while too hot and not answered. */
static bento_status_t keep_time(screen_t *screen, watch_t *watch,
                                timers_t *timers, uint32_t now_ms)
{
    bool alerted = false;
    bento_status_t status = BENTO_OK;

    if (!watch->has_reading ||
        (bento_time_ticks_diff(now_ms, timers->read_ms) >= READ_MS)) {
        timers->read_ms = now_ms;
        status = read_and_judge(screen, watch, &alerted);
        if (alerted) {
            timers->beep_ms = now_ms;
        }
    }
    if ((status == BENTO_OK) && !watch->stopped &&
        (watch->level == LEVEL_BAD) && !watch->acknowledged &&
        (bento_time_ticks_diff(now_ms, timers->beep_ms) >= BEEP_MS)) {
        timers->beep_ms = now_ms;
        status = beep(screen->ui, TUNE_BAD);
    }
    return status;
}

/* One round: listen every 20 ms, read the sensor every second. */
static bento_status_t watch_once(screen_t *screen, watch_t *watch,
                                 timers_t *timers, bool *was_down)
{
    const uint32_t now_ms = bento_time_ticks_ms();
    bool answered = false;
    bento_status_t status = take_command(screen, watch, &answered);

    if (status == BENTO_OK) {
        status = check_button(screen, watch, was_down, &answered);
    }
    if ((status == BENTO_OK) && answered && !watch->acknowledged) {
        status = acknowledge(screen, watch);
    }
    if (status == BENTO_OK) {
        drain_events(screen->ui);
        status = keep_time(screen, watch, timers, now_ms);
    }
    if ((status == BENTO_OK) && !watch->stopped) {
        status = rest(POLL_MS);
    }
    return status;
}

static bento_status_t watch_box(screen_t *screen, watch_t *watch)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    timers_t timers = {start_ms, start_ms};
    bool was_down = false;
    bento_status_t status = BENTO_OK;

    while ((status == BENTO_OK) && !watch->stopped &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        status = watch_once(screen, watch, &timers, &was_down);
    }
    if ((status == BENTO_OK) && !watch->stopped) {
        stop_here(screen, watch, "จบรอบ", COL_DIM);
    }
    return status;
}

/* The BENTO Emulator has no MQTT from C yet: say so on the screen and
 * in the console; the caller then takes the .py's broker-down path. */
static bento_status_t show_emulator_gap(bento_ui_t *ui)
{
    /* the Emulator does not wrap a label: the break is in the text */
    static const char gap[] = "MQTT จาก C ยังไม่มีใน BENTO Emulator\n"
                              "ให้รันบน Dev Kit หรือ Eva Kit";
    bento_ui_widget_args_t args;
    bento_ui_id_t label;

    (void)printf(
        "MQTT จาก C ยังไม่มีใน BENTO Emulator ให้รันบน Dev Kit "
        "หรือ Eva Kit / MQTT from C is not in the BENTO Emulator "
        "yet: run it on a Dev Kit or Eva Kit.\n");
    bento_ui_widget_args_init(&args);
    args.text = gap;
    args.text_len = (uint32_t)strlen(gap);
    args.x = GAP_X;
    args.y = GAP_Y;
    args.w = GAP_W;
    args.color = COL_WARN;
    args.font_px = GAP_PX;
    return bento_ui_label_create(ui, &args, &label);
}

/* The public broker is down now and then: 3 tries. */
static bento_status_t connect_broker(const screen_t *screen,
                                     watch_t *watch, bool *linked)
{
    char line[TEXT_BYTES];
    int32_t attempt;
    bento_status_t status = BENTO_OK;

    *linked = false;
    for (attempt = 1;
         (attempt <= BROKER_TRIES) && (status == BENTO_OK) &&
         !*linked && !watch->no_mqtt;
         attempt++) {
        if (attempt > 1) {
            (void)snprintf(line, sizeof(line),
                           "ลองต่อ broker ใหม่ %" PRId32 "/3", attempt);
            show_status(screen, line, COL_WARN);
            drain_events(screen->ui);
        }
        status = try_broker(watch, linked);
    }
    return status;
}

/* Wi-Fi, its address, the broker and the subscription; *problem
 * names the step that failed, NULL when all are up. */
static bento_status_t connect_farm(const screen_t *screen,
                                   watch_t *watch, const char **problem)
{
    uint8_t granted_qos = 0U;
    bool joined = false;
    bool linked = false;
    bento_status_t status;

    *problem = NULL;
    show_status(screen, "ต่อ WiFi...", COL_WARN);
    drain_events(screen->ui); /* the label shows before the wait */
    status = join_wifi(&joined);
    if ((status == BENTO_OK) && !joined) {
        *problem = "WiFi ไม่ติด: ตรวจชื่อ/รหัส";
    }
    if ((status == BENTO_OK) && joined) {
        status = connect_broker(screen, watch, &linked);
    }
    if ((status == BENTO_OK) && watch->no_mqtt) {
        status = show_emulator_gap(screen->ui);
    }
    /* subscribe always after connect */
    if ((status == BENTO_OK) && linked) {
        linked =
            bento_mqtt_subscribe(watch->broker, watch->topic_cmd,
                                 (uint32_t)strlen(watch->topic_cmd), 0U,
                                 &granted_qos) == BENTO_OK;
    }
    if ((status == BENTO_OK) && joined && !linked) {
        *problem = "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่";
    }
    return status;
}

static int32_t find_crop(const char *name)
{
    int32_t found = -1;
    int32_t i;

    for (i = 0; (i < CROP_COUNT) && (found < 0); i++) {
        if (strcmp(CROPS[i].name, name) == 0) {
            found = i;
        }
    }
    return found;
}

/* From the settings check to the end of the watch. */
static bento_status_t run(screen_t *screen, watch_t *watch)
{
    static const char crop_name[] = "มะม่วง"; /* CROP: one of CROPS */
    const int32_t crop = find_crop(crop_name);
    const char *problem = NULL;
    bento_status_t status = BENTO_OK;

    if (crop < 0) {
        stop_here(screen, watch, "แก้ CROP ให้ตรงกับ CROPS", COL_BAD);
    } else {
        start_watch(watch, &CROPS[crop]);
        show_crop(screen, watch);
        show_budget(screen, watch);
    }
    if (!watch->stopped && !valid_team(TEAM)) {
        stop_here(screen, watch, "แก้ TEAM ก่อน", COL_BAD);
    }
    if (!watch->stopped) {
        status = connect_farm(screen, watch, &problem);
    }
    if ((status == BENTO_OK) && (problem != NULL)) {
        stop_here(screen, watch, problem, COL_BAD);
    }
    if ((status == BENTO_OK) && !watch->stopped) {
        show_link(screen, true);
        show_status(screen, "เฝ้ากล่องอยู่", COL_OK);
        status = watch_box(screen, watch);
    }
    return status;
}

int main(void)
{
    static const char root[] = "bento-aiot";
    screen_t screen;
    watch_t watch;
    bento_status_t status;

    (void)memset(&screen, 0, sizeof(screen));
    (void)memset(&watch, 0, sizeof(watch));
    screen.ui = bento_runtime_get_ui();
    (void)snprintf(watch.topic_event, sizeof(watch.topic_event),
                   "%s/%s/event", root, TEAM);
    (void)snprintf(watch.topic_cmd, sizeof(watch.topic_cmd),
                   "%s/%s/cmd", root, TEAM);
    /* a build without sound refuses it, as hasattr() skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run(&screen, &watch);
    }
    if (watch.broker != 0U) {
        /* the .py ignores every error of its disconnect */
        (void)bento_mqtt_close(watch.broker);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
