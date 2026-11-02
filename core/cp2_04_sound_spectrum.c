/*
 * cp2_04_sound_spectrum.c -- หลักการ 2.4: เสียงเดียวกัน ดูตามเวลา
 * หรือดูตามความถี่ (FFT)
 *
 * ไมค์ให้ตัวอย่างเสียง 16000 ตัวต่อวินาที เราหยิบทีละ N = 256 ตัว (16 ms)
 * แล้ว FFT แปลงเป็น 128 ช่องความถี่ ช่องละ fs / N = 62.5 Hz ความดังแต่ละ
 * ช่องแสดงเป็น dB (ดังขึ้น 6 dB = แอมพลิจูด 2 เท่า) ผิวปาก = ยอดเดียวสูง
 * ฮัมเสียงต่ำ = ยอดทางซ้าย ตบมือ = กระจายหลายช่อง กด SW5 = บอร์ดเล่นโน้ต A
 * (440 Hz) ให้ไมค์ฟังเอง SW6 = โน้ต A สูงขึ้นหนึ่งอ็อกเทฟ (880 Hz) จอไฟ
 * RGB 16x8 = 16 แท่ง แท่งละ 500 Hz สูงขึ้นหนึ่งแถว = ดังขึ้น 6 dB
 * โปรแกรมเล่น 2 นาทีแล้วจบเอง ใช้กับ TESAIoT Dev Kit และ BENTO Emulator
 *
 * The same sound seen in time or in frequency. The microphone gives
 * 16000 samples a second; each FFT takes N = 256 of them (16 ms) and
 * turns them into 128 frequency bins, 62.5 Hz each, shown in dB (6 dB
 * louder = twice the amplitude). Whistle for one tall peak, hum for a
 * peak on the left, clap for many bins. SW5 plays the note A (440 Hz)
 * for the microphone to hear, SW6 the A an octave higher (880 Hz). The
 * 16 x 8 RGB matrix shows 16 bars of 500 Hz, one row per 6 dB. It runs
 * for two minutes.
 *
 * Twin of cp2_04_sound_spectrum.py (aiot-development-for-smart-farm/core/cp2_04_sound_spectrum.py). Style: CLEAN_C.md.
 * What C does differently:
 *   - the mic module has no C API: the board's PDM microphone
 *     (machine.PDM_PCM, 16 kHz) is read directly, and
 *     bento_machine_pdm_stats with fresh = true gives the newest N
 *     samples, as mic.stats() then mic.raw() do; the reTerminal has
 *     no PDM microphone;
 *   - SENS (sensitivity 1-5) has no C setting: the PDM's default gain
 *     is used;
 *   - mic.stop() in the .py's finally is closing the PDM, in one
 *     cleanup helper with clearing the matrix;
 *   - the sample, scratch and spectrum buffers are too big for the
 *     program stack, so they are one file-scope object (CLEAN_C.md C9);
 *   - the RGB matrix's OSError is ignored, as the .py ignores it; a
 *     board without the matrix refuses each call the same way, where
 *     MicroPython's import rgbmatrix would raise;
 *   - the settings sit in one block at the top (deviation CT02); N
 *     and FS are FFT_N and FS_HZ (names say what they are), NOTES is
 *     NOTE_SW5 and NOTE_SW6;
 *   - lists of bins and bars are fixed arrays; None (no hold yet,
 *     nothing drawn yet) is a bool beside the array;
 *   - hasattr(ui, "volume") is not asked: the volume call is always
 *     made, and a firmware without it refuses it, which is ignored;
 *   - ui.tone() and chart points are fire-and-forget in MicroPython,
 *     so their status is ignored here too;
 *   - Stop ends the program at the next sleep, closes the microphone
 *     and main returns 0, without the closing tune and line
 *     (MicroPython's KeyboardInterrupt).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bento_buttons.h"
#include "bento_dsp.h"
#include "bento_fmt.h"
#include "bento_machine.h"
#include "bento_rgbmatrix.h"
#include "bento_runtime.h"
#include "bento_time.h"

/* ---- แก้ก่อนรัน: settings (MISRA deviation CT02) ---- */
enum {
    FFT_N = 256,     /* .py: N; samples per FFT (a power of 2, 8-512) */
    FS_HZ = 16000,   /* .py: FS; the microphone's sample rate, Hz */
    FLOOR_DB = 24,   /* quieter than this = the matrix stays dark */
    DB_ROW = 6,      /* one more matrix row = this many dB louder */
    SENS = 3,        /* microphone sensitivity 1-5 (no C setting) */
    NOTE_SW5 = 69,   /* .py: NOTES[0]; MIDI 69 = A 440 Hz */
    NOTE_SW6 = 81,   /* .py: NOTES[1]; MIDI 81 = A 880 Hz */
    NOTE_MS = 1000,  /* how long a reference note plays */
    POINTS = 32,     /* points on the spectrum chart (4 bins each) */
    SAMPLE_MS = 20,  /* pause between two listens */
    TICK_MS = 500,   /* sum up on the screen this often */
    RUN_MS = 120000, /* play for two minutes, then end */
    SPEAKER = 40,    /* speaker level 0-100 % (firmware 2.4.2+) */
    VOLUME = 25      /* tone loudness 0-127 (about 20 %) */
};
/* Names on the panel: SW5 = lower button = pressed(1), SW6 = upper
 * button = pressed(0). */
static const char *const BUTTON_NAMES[] = {"SW5",
                                           "SW6"}; /* .py: BTN_NAMES */
/* ---- end of settings ---- */

/* The course palette, 0xRRGGBB. */
enum {
    COL_TEXT = 0xE8EAED,
    COL_DIM = 0x9AA3AF,
    COL_CARD = 0x171B22,
    COL_OK = 0x30A46C,
    COL_WARN = 0xF5A623,
    COL_BAD = 0xE5484D,
    COL_INFO = 0x4A9EFF
};

/* Screen layout, in pixels. */
enum {
    MARGIN_X = 12,
    TITLE_Y = 6,
    SUBTITLE_Y = 38,
    HELP_Y = 352,
    CARD_RADIUS = 12,
    CARD_BORDER = 1,
    CARD_TITLE_DX = 12,
    CARD_TITLE_DY = 6,
    CARD_Y = 64,
    CARD_H = 124,
    HZ_CARD_X = 12,
    HZ_CARD_W = 380,
    SEG_X = 24,
    SEG_Y = 98,
    SEG_W = 170,
    SEG_H = 56,
    BIN_Y = 160,
    MATRIX_CARD_X = 402,
    MATRIX_CARD_W = 378,
    MATRIX_TEXT_X = 414,
    BAR_WIDTH_Y = 100,
    ROW_DB_Y = 128,
    HEARD_Y = 156,
    AXIS_Y = 196,
    CHART_X = 12,
    CHART_Y = 218,
    CHART_W = 400,
    CHART_H = 120,
    CHART_HI = 90,
    CHART_MAX_W = 400,
    NOTE_X = 434,
    NOTE_CAPTION_Y = 226,
    NOTE_Y = 256
};

/* Font sizes, in pixels: Thai text is 20 px or more on the board. */
enum {
    FONT_TITLE = 24,
    FONT_TEXT = 20,
    FONT_SMALL = 14, /* the Latin-only axis line */
    FONT_NOTE = 20
};

/* The spectrum and the RGB matrix. */
enum {
    BINS = FFT_N / 2,      /* the FFT's frequency bins */
    SAMPLE_BYTES = 2,      /* one 16-bit sample */
    FFT_BYTES = FFT_N * 2, /* one window of 16-bit samples */
    BANDS = 16,            /* bars on the RGB matrix */
    MATRIX_ROWS = 8,       /* rows of the RGB matrix */
    FRAME_BYTES = 64,      /* one matrix picture, 4 bits a pixel */
    FRAME_ROW_BYTES = 8,   /* 16 pixels of 4 bits */
    GREEN_ROWS = 4,        /* the lowest rows are green */
    YELLOW_ROWS = 6,       /* then yellow, then red */
    NIBBLE_BITS = 4,
    LOW_NIBBLE = 0x0F,
    HIGH_NIBBLE = 0xF0,
    MS_PER_S = 1000,
    A4_NOTE = 69, /* MIDI A4 */
    A4_HZ = 440,
    NOTES_PER_OCTAVE = 12
};

/* Sound, timing and sizes that are not settings. */
enum {
    SETTLE_MS = 200,   /* let the cleared screen settle */
    TONE_MS = 90,      /* each note of a tune */
    NOTE_GAP_MS = 100, /* one note to the next */
    TUNE_NOTES = 3,    /* the longest tune */
    TEXT_BYTES = 128,  /* a SET_TEXT carries at most 126 bytes */
    BUTTON_COUNT = 2
};

/* The tunes every course file shares. */
enum {
    TUNE_TAP = 0,
    TUNE_START = 1,
    TUNE_STOP = 2,
    TUNE_GOOD = 3,
    TUNE_BAD = 4,
    TUNE_EMPTY = 5,
    TUNE_HIT = 6,
    TUNE_KINDS = 7
};

typedef struct {
    int32_t x;
    int32_t y;
    int32_t font_px;
    uint32_t color;
} label_spec_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} rect_t;

/* The sound buffers: the raw bytes, the samples, the FFT's work area,
 * the newest spectrum and the loudest of each bin this half second. */
typedef struct {
    uint8_t bytes[FFT_BYTES];
    int16_t samples[FFT_N];
    float scratch[FFT_N * 2];
    float spectrum[BINS];
    float hold[BINS];
} sound_t;

/* Too big for the 8 KB program stack: file scope, zeroed by the
 * loader on every load, and set again at the start of main()
 * (CLEAN_C.md C9, deviation BENTO-MISRA-E03/E04). */
extern sound_t g_sound;
sound_t g_sound;

/* The widgets that change after the screen is drawn. */
typedef struct {
    bento_ui_t *ui;
    bento_ui_id_t hz;
    bento_ui_id_t bin;
    bento_ui_id_t heard;
    bento_ui_id_t chart;
    bento_ui_id_t note;
    bento_ui_id_t help;
} screen_t;

/* What the loop remembers between rounds. */
typedef struct {
    bool down[BUTTON_COUNT];
    bool hold_known;
    int32_t heard;
    bool drawn_known;
    int32_t drawn[BANDS];
} listen_t;

static int16_t px(int32_t value)
{
    return (int16_t)value;
}

/* A sleep that turns Stop into BENTO_E_CANCELLED. */
static bento_status_t pause_ms(uint32_t delay_ms)
{
    bento_status_t status = bento_time_sleep_ms(delay_ms);

    if ((status == BENTO_OK) && !bento_runtime_is_running()) {
        status = BENTO_E_CANCELLED;
    }
    return status;
}

/* Opens the PDM microphone at FS_HZ and starts it; the start throws the
 * first frames away itself. */
static bento_status_t open_microphone(uint32_t *handle)
{
    bento_machine_pdm_cfg_t config;
    bento_status_t status;

    bento_machine_pdm_cfg_init(&config);
    config.sample_rate = FS_HZ;
    status = bento_machine_pdm_open(&config, handle);
    if (status == BENTO_OK) {
        status = bento_machine_pdm_start(*handle);
        if (status != BENTO_OK) {
            /* the start failed: its status is the one to report */
            (void)bento_machine_pdm_close(*handle);
        }
    }
    return status;
}

/* One window of FFT_N samples (16 ms) as BINS bins. Older sound waiting
 * in the ring is dropped first, so this is the latest sound; the FFT
 * takes off the mean (the microphone's DC) and applies a Hann window
 * itself. */
static bento_status_t listen(uint32_t handle, sound_t *sound)
{
    bento_machine_pdm_stats_t stats;
    uint32_t bins = 0U;
    uint32_t i;
    bento_status_t status;

    bento_machine_pdm_stats_init(&stats);
    status = bento_machine_pdm_stats(handle, sound->bytes,
                                     (uint32_t)sizeof(sound->bytes),
                                     true, &stats);
    for (i = 0U; (i < (uint32_t)FFT_N) && (status == BENTO_OK); i++) {
        const uint32_t at = i * (uint32_t)SAMPLE_BYTES;
        const uint16_t bits =
            (uint16_t)((uint16_t)sound->bytes[at] |
                       (uint16_t)((uint16_t)sound->bytes[at + 1U]
                                  << 8U));

        sound->samples[i] = (int16_t)bits;
    }
    if (status == BENTO_OK) {
        status = bento_dsp_fft_mag_s16(
            sound->samples, FFT_N, FFT_N, BENTO_DSP_FFT_DEFAULT,
            sound->scratch, FFT_N * 2, sound->spectrum, BINS, &bins);
    }
    return status;
}

/* Tones go through ui.tone because its loudness can be set. */
static bento_status_t beep(bento_ui_t *ui, int32_t tune)
{
    static const uint8_t notes[TUNE_KINDS][TUNE_NOTES] = {
        {76U, 0U, 0U},   {72U, 79U, 0U}, {79U, 72U, 0U},
        {72U, 79U, 84U}, {84U, 76U, 0U}, {84U, 76U, 69U},
        {88U, 0U, 0U}};
    static const uint8_t lengths[TUNE_KINDS] = {1U, 2U, 2U, 3U,
                                                2U, 3U, 1U};
    bento_status_t status = BENTO_OK;
    uint8_t i;

    for (i = 0U; (i < lengths[tune]) && (status == BENTO_OK); i++) {
        /* fire-and-forget in MicroPython too */
        (void)bento_ui_sfx_tone(ui, notes[tune][i],
                                (uint8_t)BENTO_UI_WAVE_SINE,
                                (uint8_t)VOLUME, (uint16_t)TONE_MS);
        status = pause_ms((uint32_t)NOTE_GAP_MS);
    }
    return status;
}

/* Sets pixel (x, y) of the 64-byte matrix picture (4 bits a pixel). */
static void put_pixel(uint8_t frame[FRAME_BYTES], int32_t x, int32_t y,
                      int32_t color)
{
    const int32_t i = (y * FRAME_ROW_BYTES) + (x / 2);
    const uint32_t bits = (uint32_t)color;

    if ((x % 2) != 0) {
        frame[i] =
            (uint8_t)(((uint32_t)frame[i] & (uint32_t)LOW_NIBBLE) |
                      (bits << (uint32_t)NIBBLE_BITS));
    } else {
        frame[i] =
            (uint8_t)(((uint32_t)frame[i] & (uint32_t)HIGH_NIBBLE) |
                      bits);
    }
}

/* Green low, yellow in the middle, red on top. */
static int32_t band_color(int32_t row)
{
    int32_t color = BENTO_RGBMATRIX_RED;

    if (row < GREEN_ROWS) {
        color = BENTO_RGBMATRIX_GREEN;
    } else if (row < YELLOW_ROWS) {
        color = BENTO_RGBMATRIX_YELLOW;
    } else {
        /* the top rows stay red */
    }
    return color;
}

/* The whole matrix: bar x is heights[x] rows from the bottom. */
static void draw_bands(const int32_t heights[BANDS])
{
    uint8_t frame[FRAME_BYTES];
    int32_t x;

    (void)memset(frame, 0, sizeof(frame));
    for (x = 0; x < BANDS; x++) {
        int32_t row;

        for (row = 0; row < heights[x]; row++) {
            put_pixel(frame, x, (MATRIX_ROWS - 1) - row,
                      band_color(row));
        }
    }
    /* the matrix may be slow to answer: this picture is skipped */
    (void)bento_rgbmatrix_blit(frame, (uint32_t)sizeof(frame));
}

/* A magnitude in dB = 20 log10(m): 1 -> 0 dB, 2 -> 6 dB, 1000 -> 60
 * dB; 20 / ln(10) = 8.686. */
static int32_t to_db(float magnitude)
{
    const float per_ln = 8.686F;
    int32_t db = 0;

    if (magnitude > 1.0F) {
        db = (int32_t)(per_ln * logf(magnitude));
    }
    return db;
}

/* Keeps the loudest of each bin for half a second, so a short sound
 * (a clap) is not lost between two screens. */
static void hold_max(listen_t *state, sound_t *sound)
{
    uint32_t i;

    if (!state->hold_known) {
        (void)memcpy(sound->hold, sound->spectrum, sizeof(sound->hold));
        state->hold_known = true;
    }
    for (i = 0U; i < (uint32_t)BINS; i++) {
        sound->hold[i] = (sound->spectrum[i] > sound->hold[i])
                             ? sound->spectrum[i]
                             : sound->hold[i];
    }
}

/* The loudest bin, from bin 1 on (bin 0 is DC, not sound). */
static int32_t peak_bin(const float spectrum[BINS])
{
    int32_t peak = 1;
    int32_t i;

    for (i = 2; i < BINS; i++) {
        if (spectrum[i] > spectrum[peak]) {
            peak = i;
        }
    }
    return peak;
}

/* BINS bins folded into `count` groups, each the loudest of its bins
 * (so a peak survives the folding). */
static void group(const float spectrum[BINS], int32_t count,
                  float groups[])
{
    const int32_t width = BINS / count;
    int32_t i;

    for (i = 0; i < count; i++) {
        const int32_t first = i * width;
        float loudest = spectrum[first];
        int32_t j;

        for (j = 1; j < width; j++) {
            const float value = spectrum[first + j];

            loudest = (value > loudest) ? value : loudest;
        }
        groups[i] = loudest;
    }
}

/* A MIDI note's frequency: 69 = 440 Hz, an octave (12 notes) up is
 * twice the frequency. */
static int32_t note_hz(int32_t note)
{
    const float octave_ratio = 2.0F;
    const float octaves =
        (float)(note - A4_NOTE) / (float)NOTES_PER_OCTAVE;

    const float hz = (float)A4_HZ * powf(octave_ratio, octaves);

    return (int32_t)hz;
}

/* Loudness as a bar height of 0-8 rows: lit from FLOOR_DB, one more
 * row every DB_ROW. */
static int32_t rows_of(float magnitude)
{
    const int32_t above = to_db(magnitude) - FLOOR_DB;
    int32_t rows = 0;

    if (above >= 0) {
        rows = (above / DB_ROW) + 1;
        rows = (rows < MATRIX_ROWS) ? rows : MATRIX_ROWS;
    }
    return rows;
}

static bento_status_t add_label(bento_ui_t *ui, const char *text,
                                const label_spec_t *spec,
                                bento_ui_id_t *out_id)
{
    bento_ui_widget_args_t args;

    bento_ui_widget_args_init(&args);
    args.text = text;
    args.text_len = (uint32_t)strlen(text);
    args.x = px(spec->x);
    args.y = px(spec->y);
    args.color = spec->color;
    args.font_px = spec->font_px;
    return bento_ui_label_create(ui, &args, out_id);
}

/* A card: a dark box with a grey border and a blue title. */
static bento_status_t add_card(bento_ui_t *ui, const rect_t *box,
                               const char *title)
{
    const label_spec_t spec = {box->x + CARD_TITLE_DX,
                               box->y + CARD_TITLE_DY, FONT_TEXT,
                               COL_INFO};
    bento_ui_widget_args_t args;
    bento_ui_id_t unused;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = px(box->x);
    args.y = px(box->y);
    args.w = px(box->w);
    args.h = px(box->h);
    args.color = COL_CARD;
    args.min_val = COL_DIM;
    args.max_val = CARD_RADIUS;
    args.font_px = CARD_BORDER;
    status = bento_ui_panel_create(ui, &args, &unused);
    if (status == BENTO_OK) {
        status = add_label(ui, title, &spec, &unused);
    }
    return status;
}

/* The line under the title: FFT_N, the bins and the width of a bin. */
static bento_status_t add_heading(bento_ui_t *ui)
{
    const label_spec_t title = {MARGIN_X, TITLE_Y, FONT_TITLE,
                                COL_TEXT};
    const label_spec_t subtitle = {MARGIN_X, SUBTITLE_Y, FONT_TEXT,
                                   COL_DIM};
    char width[BENTO_FMT_BUF_SIZE];
    char text[TEXT_BYTES];
    uint32_t length = 0U;
    bento_ui_id_t unused;
    bento_status_t status =
        add_label(ui, "เสียง -> ความถี่ (FFT)", &title, &unused);

    if (bento_fmt_real(width, (uint32_t)sizeof(width),
                       (float)FS_HZ / (float)FFT_N,
                       &length) != BENTO_OK) {
        width[0] = '\0'; /* BENTO_FMT_BUF_SIZE always holds it */
    }
    (void)snprintf(text, sizeof(text), "N = %d -> %d ช่อง ช่องละ %s Hz",
                   FFT_N, BINS, width);
    if (status == BENTO_OK) {
        status = add_label(ui, text, &subtitle, &unused);
    }
    return status;
}

/* The main frequency card: a Seg7 and the bin line. */
static bento_status_t add_hz_card(screen_t *screen)
{
    const rect_t box = {HZ_CARD_X, CARD_Y, HZ_CARD_W, CARD_H};
    const label_spec_t bin = {SEG_X, BIN_Y, FONT_TEXT, COL_TEXT};
    bento_ui_widget_args_t args;
    bento_status_t status = add_card(screen->ui, &box, "ความถี่เด่น (Hz)");

    bento_ui_widget_args_init(&args);
    args.text = "0";
    args.text_len = 1U;
    args.x = SEG_X;
    args.y = SEG_Y;
    args.w = SEG_W;
    args.h = SEG_H;
    args.color = COL_INFO;
    if (status == BENTO_OK) {
        status = bento_ui_seg7_create(screen->ui, &args, &screen->hz);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &bin, &screen->bin);
    }
    return status;
}

/* The RGB matrix card: what one bar and one row mean. */
static bento_status_t add_matrix_card(screen_t *screen)
{
    const rect_t box = {MATRIX_CARD_X, CARD_Y, MATRIX_CARD_W, CARD_H};
    const label_spec_t bar = {MATRIX_TEXT_X, BAR_WIDTH_Y, FONT_TEXT,
                              COL_TEXT};
    const label_spec_t row = {MATRIX_TEXT_X, ROW_DB_Y, FONT_TEXT,
                              COL_TEXT};
    const label_spec_t heard = {MATRIX_TEXT_X, HEARD_Y, FONT_TEXT,
                                COL_DIM};
    bento_ui_id_t unused;
    bento_status_t status =
        add_card(screen->ui, &box, "จอไฟ RGB 16 แท่ง");

    if (status == BENTO_OK) {
        status = add_label(screen->ui, "แท่งละ 500 Hz", &bar, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "1 แถว = +6 dB", &row, &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &heard, &screen->heard);
    }
    return status;
}

/* A line chart at most 400 wide with POINTS points: one spectrum
 * exactly. */
static bento_status_t add_chart(screen_t *screen)
{
    bento_ui_widget_args_t args;
    bento_status_t status;

    bento_ui_widget_args_init(&args);
    args.x = CHART_X;
    args.y = CHART_Y;
    args.w = (CHART_W < CHART_MAX_W) ? CHART_W : CHART_MAX_W;
    args.h = CHART_H;
    args.color = COL_INFO;
    args.min_val = 0;
    args.max_val = CHART_HI;
    status = bento_ui_chart_create(screen->ui, &args, &screen->chart);
    if (status == BENTO_OK) {
        /* a setter: MicroPython ignores its refusal too */
        (void)bento_ui_set_prop(screen->ui, screen->chart,
                                (uint8_t)BENTO_UI_PROP_CHART_POINTS,
                                POINTS);
    }
    return status;
}

/* ui.poll() here only keeps the screen drawn: its events are not
 * read, and a failed poll answers no events. */
static void poll_screen(bento_ui_t *ui)
{
    bento_ui_event_t events[BENTO_UI_POLL_EVENTS_MAX];
    uint32_t count = 0U;

    (void)bento_ui_poll(ui, events, BENTO_UI_POLL_EVENTS_MAX, &count,
                        NULL);
}

/* The chart's axis line, the note card and the help line. */
static bento_status_t add_lower_labels(screen_t *screen)
{
    const label_spec_t axis = {MARGIN_X, AXIS_Y, FONT_SMALL, COL_DIM};
    const label_spec_t caption = {NOTE_X, NOTE_CAPTION_Y, FONT_TEXT,
                                  COL_INFO};
    const label_spec_t note = {NOTE_X, NOTE_Y, FONT_NOTE, COL_WARN};
    const label_spec_t help = {MARGIN_X, HELP_Y, FONT_TEXT, COL_DIM};
    bento_ui_id_t unused;
    bento_status_t status =
        add_label(screen->ui, "dB   0 Hz -> 8 kHz", &axis, &unused);

    if (status == BENTO_OK) {
        status = add_chart(screen);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "SW5 / SW6 = โน้ตอ้างอิง", &caption,
                           &unused);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, " ", &note, &screen->note);
    }
    if (status == BENTO_OK) {
        status = add_label(screen->ui, "ผิวปาก ฮัม ตบมือ", &help,
                           &screen->help);
    }
    return status;
}

static bento_status_t build_screen(screen_t *screen)
{
    bento_status_t status =
        bento_ui_screen(screen->ui, BENTO_RUNTIME_SCREEN_WIDTH_PX,
                        BENTO_RUNTIME_SCREEN_HEIGHT_PX);

    if (status == BENTO_OK) {
        status = pause_ms((uint32_t)SETTLE_MS);
    }
    if (status == BENTO_OK) {
        status = add_heading(screen->ui);
    }
    if (status == BENTO_OK) {
        status = add_hz_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_matrix_card(screen);
    }
    if (status == BENTO_OK) {
        status = add_lower_labels(screen);
    }
    if (status == BENTO_OK) {
        poll_screen(screen->ui);
    }
    return status;
}

/* MicroPython's widget setters ignore a refusal (modui.c), so the
 * twin does too. */
static void set_text(bento_ui_t *ui, bento_ui_id_t widget,
                     const char *text)
{
    (void)bento_ui_set_text(ui, widget, text, (uint32_t)strlen(text));
}

/* The main frequency, its bin, the share of time heard, and the
 * spectrum on the chart. */
static void show(const screen_t *screen, const float spectrum[BINS],
                 int32_t heard)
{
    const int32_t peak = peak_bin(spectrum);
    const int32_t db = to_db(spectrum[peak]);
    const bool loud = db >= FLOOR_DB;
    float points[POINTS];
    char text[TEXT_BYTES];
    int32_t heard_ms;
    int32_t i;

    (void)snprintf(text, sizeof(text), "%ld",
                   loud ? (long)((peak * FS_HZ) / FFT_N) : 0L);
    set_text(screen->ui, screen->hz, text);
    (void)snprintf(text, sizeof(text), "ช่อง %ld  %ld dB", (long)peak,
                   (long)db);
    set_text(screen->ui, screen->bin, loud ? text : "เงียบ");
    heard_ms = (heard * FFT_N * (int32_t)MS_PER_S) / FS_HZ;
    (void)snprintf(text, sizeof(text), "ฟัง %ld ms ต่อ %d ms",
                   (long)heard_ms, TICK_MS);
    set_text(screen->ui, screen->heard, text);
    /* the screen's queue drops chart commands when nearly full, and a
     * lost point shifts the whole line, so only POINTS points go */
    group(spectrum, POINTS, points);
    for (i = 0; i < POINTS; i++) {
        /* chart points are fire-and-forget in MicroPython too */
        (void)bento_ui_chart_set_next(screen->ui, screen->chart, 0U,
                                      to_db(points[i]));
    }
}

/* A press edge of SW5 or SW6 plays a reference note of known
 * frequency, for the microphone to hear the board's own speaker.
 * Called often: the firmware filters the bounce at each read. */
static bento_status_t check_buttons(const screen_t *screen,
                                    listen_t *state)
{
    static const int32_t notes[BUTTON_COUNT] = {NOTE_SW5, NOTE_SW6};
    bento_status_t status = BENTO_OK;
    uint32_t i;

    for (i = 0U; (i < (uint32_t)BUTTON_COUNT) && (status == BENTO_OK);
         i++) {
        bool down = false;

        status = bento_buttons_pressed(1U - i, &down);
        if ((status == BENTO_OK) && down && !state->down[i]) {
            char text[TEXT_BYTES];

            /* fire-and-forget in MicroPython too */
            (void)bento_ui_sfx_tone(screen->ui, (uint8_t)notes[i],
                                    (uint8_t)BENTO_UI_WAVE_SINE,
                                    (uint8_t)VOLUME, (uint16_t)NOTE_MS);
            (void)snprintf(text, sizeof(text), "%s เล่น %ld Hz",
                           BUTTON_NAMES[i], (long)note_hz(notes[i]));
            set_text(screen->ui, screen->note, text);
        }
        state->down[i] = down;
    }
    return status;
}

/* Every TICK_MS: BINS bins as 16 bars on the matrix (drawn again only
 * when the picture changes), then the screen. */
static void sum_up(const screen_t *screen, listen_t *state,
                   const sound_t *sound)
{
    float bands[BANDS];
    int32_t heights[BANDS];
    int32_t i;

    group(sound->hold, BANDS, bands);
    for (i = 0; i < BANDS; i++) {
        heights[i] = rows_of(bands[i]);
    }
    if (!state->drawn_known ||
        (memcmp(heights, state->drawn, sizeof(heights)) != 0)) {
        draw_bands(heights);
        (void)memcpy(state->drawn, heights, sizeof(heights));
        state->drawn_known = true;
    }
    show(screen, sound->hold, state->heard);
    state->hold_known = false;
    state->heard = 0;
    poll_screen(screen->ui);
}

/* Buttons, a listen, then buttons again (a listen takes about two
 * windows). */
static bento_status_t listen_once(const screen_t *screen,
                                  listen_t *state, uint32_t handle,
                                  sound_t *sound)
{
    bento_status_t status = check_buttons(screen, state);

    if (status == BENTO_OK) {
        status = listen(handle, sound);
    }
    if (status == BENTO_OK) {
        hold_max(state, sound);
        state->heard++;
        status = check_buttons(screen, state);
    }
    return status;
}

/* Listens until RUN_MS, with a summary every TICK_MS. */
static bento_status_t run_loop(const screen_t *screen, uint32_t handle,
                               sound_t *sound)
{
    const uint32_t start_ms = bento_time_ticks_ms();
    uint32_t shown_ms = start_ms;
    listen_t state;
    bento_status_t status = BENTO_OK;

    (void)memset(&state, 0, sizeof(state));
    while ((status == BENTO_OK) &&
           (bento_time_ticks_diff(bento_time_ticks_ms(), start_ms) <
            RUN_MS)) {
        uint32_t now_ms;

        status = listen_once(screen, &state, handle, sound);
        now_ms = bento_time_ticks_ms();
        if ((status == BENTO_OK) &&
            (bento_time_ticks_diff(now_ms, shown_ms) >= TICK_MS)) {
            shown_ms = now_ms;
            sum_up(screen, &state, sound);
        }
        if (status == BENTO_OK) {
            status = pause_ms((uint32_t)SAMPLE_MS);
        }
    }
    return status;
}

/* The .py's finally: the microphone closed, the matrix cleared. A
 * close that fails would raise there, so its status wins. */
static bento_status_t close_all(uint32_t handle)
{
    const bento_status_t status = bento_machine_pdm_close(handle);

    /* the matrix may be slow to answer: then it stays as it is */
    (void)bento_rgbmatrix_clear();
    return status;
}

/* The microphone open, the loop, and always the cleanup. */
static bento_status_t run_listening(const screen_t *screen)
{
    sound_t *sound = &g_sound;
    uint32_t handle = 0U;
    bento_status_t status = open_microphone(&handle);

    if (status == BENTO_OK) {
        const bento_status_t loop_status =
            run_loop(screen, handle, sound);
        const bento_status_t close_status = close_all(handle);

        status =
            (close_status != BENTO_OK) ? close_status : loop_status;
    }
    return status;
}

int main(void)
{
    screen_t screen;
    bento_status_t status;

    (void)memset(&g_sound, 0, sizeof(g_sound));
    (void)memset(&screen, 0, sizeof(screen));
    screen.ui = bento_runtime_get_ui();
    /* firmware 2.4.1 has no volume and refuses it; the .py skips it */
    (void)bento_ui_sfx_volume_set(screen.ui, SPEAKER);
    status = build_screen(&screen);
    if (status == BENTO_OK) {
        status = run_listening(&screen);
    }
    if (status == BENTO_OK) {
        /* the microphone is closed: the tune does not reach the
         * spectrum */
        status = beep(screen.ui, TUNE_GOOD);
    }
    if (status == BENTO_OK) {
        (void)bento_ui_set_color(screen.ui, screen.help, COL_WARN);
        set_text(screen.ui, screen.help,
                 "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่");
        poll_screen(screen.ui);
    }
    return (status == BENTO_E_CANCELLED) ? 0 : (int)status;
}
