# cp2_04_sound_spectrum.py - หลักการ 2.4: เสียงเดียวกัน ดูตามเวลา หรือดูตามความถี่ (FFT)
#
# หลักการ  : ไมค์ให้ตัวอย่างเสียง 16000 ตัวต่อวินาที (อัตราสุ่ม fs) เราหยิบทีละ N = 256 ตัว (16 ms)
#            dsp.fft_mag() แปลง 256 ตัวอย่าง "ตามเวลา" -> 128 "ช่องความถี่" (bin) คำนวณในภาษา C
#            ความกว้างช่อง = fs / N = 16000 / 256 = 62.5 Hz: ช่อง k คือความถี่ k x 62.5 Hz
#            ช่องสุดท้าย (127) ราว 7.9 kHz เพราะวัดได้สูงสุดแค่ครึ่งหนึ่งของ fs
#            ความดังแต่ละช่องแสดงเป็น dB (สเกลลอการิทึม): ดังขึ้น 6 dB = แอมพลิจูดเป็น 2 เท่า
#            dB ในไฟล์นี้เทียบกับ "1 หน่วยดิบของไมค์" ไม่ใช่ dB ของเครื่องวัดเสียง
# ลองเล่น  : ผิวปาก = ยอดเดียวสูง · ฮัมเสียงต่ำ = ยอดทางซ้าย · ตบมือ = กระจายหลายช่อง
#            กด SW5 = บอร์ดเล่นโน้ต A (440 Hz) ให้ไมค์ฟังเอง · SW6 = โน้ต A สูงขึ้นหนึ่งอ็อกเทฟ (880 Hz)
#            ดูว่ายอดไปอยู่ช่องไหน แล้วลองคูณ 62.5 ว่าใกล้ความถี่ของโน้ตไหม
#            (เสียงโน้ตเบา VOLUME 25 ถ้ายอดไม่ขึ้นชัด ให้ผิวปากแทน)
# ของบนบอร์ด: ไมค์บนบอร์ด (mic) · dsp.fft_mag · ปุ่ม SW5 (ปุ่มล่าง) / SW6 (ปุ่มบน) · ลำโพง
#            จอไฟ RGB 16x8 = 16 แท่ง แท่งละ 500 Hz (8 ช่อง) สูงขึ้นหนึ่งแถว = ดังขึ้น 6 dB
# ในฟาร์ม  : ปั๊มน้ำ พัดลมโรงเรือน ฝูงไก่ ต่างมี "ความถี่ประจำตัว" ฟังเป็นความถี่ = แยกได้ว่าอะไรดัง
#            เสียงผิดปกติของเครื่องจักร (เช่นลูกปืนสึก) มักโผล่เป็นยอดที่ความถี่ใหม่
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#            ไมค์ใน Emulator เป็นเสียงสังเคราะห์ 440 Hz (ยอดอยู่ช่อง 7 = 437 Hz) ตบมือใส่คอมพิวเตอร์ไม่มีผล
#            และเสียงโน้ตจากปุ่มใน Emulator ไม่เข้าไมค์จำลอง

import buttons
import dsp
import math
import mic
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
N = 256              # ตัวอย่างต่อหนึ่ง FFT (2 ยกกำลัง, 8-512) คงไว้ 256: เฟิร์มแวร์ยืมหน่วยความจำ 2 x N float
FS = 16000           # อัตราสุ่มของไมค์ (Hz) ตามที่โมดูล mic ตั้งไว้
FLOOR_DB = 24        # เบากว่านี้ = ไฟดับ ถ้าห้องเงียบแต่ไฟยังติด ให้เพิ่มเลขนี้
DB_ROW = 6           # สูงขึ้นหนึ่งแถวบนจอไฟ RGB = ดังขึ้นกี่ dB
SENS = 3             # ความไวไมค์ 1-5
NOTES = (69, 81)     # โน้ตของ SW5, SW6 (เลข MIDI: 69 = A 440 Hz, 81 = A 880 Hz)
NOTE_MS = 1000       # เล่นโน้ตนานกี่ ms
POINTS = 32          # จุดบนกราฟสเปกตรัม (1 จุด = 4 ช่อง = 250 Hz) ดูเหตุผลใน show_spectrum()
SAMPLE_MS = 20       # พักระหว่างการฟังแต่ละครั้ง
TICK_MS = 500        # สรุปผลขึ้นจอทุกกี่ ms
RUN_MS = 120000      # เล่นนาน 2 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def listen():
    # ฟังหนึ่งหน้าต่าง (256 ตัวอย่าง = 16 ms) แล้วคืน 128 ช่องความถี่ (list ของ float)
    # mic.stats() ทิ้งเสียงเก่าที่ค้างในคิวก่อน mic.raw() จึงได้เสียงล่าสุด ไม่ใช่เสียงเมื่อครู่ก่อน
    # fft_mag หักค่าเฉลี่ย (DC ของไมค์) และคูณหน้าต่าง Hann ให้เองก่อนแปลง
    mic.stats()
    return dsp.fft_mag(mic.raw(), N)


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


def put(buf, x, y, c):
    # ตั้งสีจุด (x, y) ในเฟรม 64 ไบต์ของจอไฟ RGB (จุดละ 4 บิต)
    i = y * 8 + (x >> 1)
    if x & 1:
        buf[i] = (buf[i] & 0x0F) | (c << 4)
    else:
        buf[i] = (buf[i] & 0xF0) | c


def draw_bands(heights):
    # วาดทั้งจอ: แท่งที่ x สูง heights[x] แถวจากล่าง ล่างเขียว กลางเหลือง บนแดง
    buf = bytearray(64)
    for x, h in enumerate(heights):
        for r in range(h):
            c = rgbmatrix.GREEN if r < 4 else (rgbmatrix.YELLOW if r < 6 else rgbmatrix.RED)
            put(buf, x, 7 - r, c)
    try:
        rgbmatrix.blit(buf)
    except OSError:
        pass                    # จอไฟ RGB ตอบไม่ทัน: ข้ามภาพนี้ไป


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def db(m):
    # ขนาด -> dB = 20 log10(m): 1 -> 0 dB, 2 -> 6 dB, 1000 -> 60 dB
    return int(8.686 * math.log(m)) if m > 1 else 0


def hold_max(hold, spec):
    # จำค่าสูงสุดของแต่ละช่องตลอดครึ่งวินาที เสียงสั้น (ตบมือ) จะได้ไม่หลุดรอบจอ
    if hold is None:
        return spec
    for i in range(len(spec)):
        if spec[i] > hold[i]:
            hold[i] = spec[i]
    return hold


def peak_bin(spec):
    # ช่องที่ดังที่สุด เริ่มดูจากช่อง 1 (ช่อง 0 = DC ไม่ใช่เสียง)
    k = 1
    for i in range(2, len(spec)):
        if spec[i] > spec[k]:
            k = i
    return k


def group(spec, n):
    # ยุบ 128 ช่องเหลือ n กลุ่ม กลุ่มละช่องที่ดังที่สุด (ยอดจึงไม่หายตอนย่อ)
    w = len(spec) // n
    return [max(spec[i * w:i * w + w]) for i in range(n)]


def note_hz(n):
    # เลขโน้ต MIDI -> ความถี่: 69 = 440 Hz ขึ้นหนึ่งอ็อกเทฟ (12 โน้ต) = ความถี่ 2 เท่า
    return int(440 * 2 ** ((n - 69) / 12))


def rows(m):
    # ความดัง -> ความสูงแท่ง 0-8 แถว: เริ่มติดที่ FLOOR_DB แล้วเพิ่มแถวละ DB_ROW
    d = db(m) - FLOOR_DB
    return 0 if d < 0 else min(8, d // DB_ROW + 1)


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, points=400):
    # กราฟเส้น กว้างไม่เกิน 400 จำนวนจุดตั้งได้ 10-400 (ไฟล์นี้ใช้ POINTS จุด = หนึ่งสเปกตรัมพอดี)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi)
    ch.prop(ui.PROP_CHART_POINTS, points)
    return ch


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("เสียง -> ความถี่ (FFT)", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("N = %d -> %d ช่อง ช่องละ %s Hz" % (N, N // 2, FS / N), x=12, y=38,
             color=COL_DIM, value=16)
    card(12, 64, 380, 124, "ความถี่เด่น (Hz)")
    w = {"hz": ui.Seg7(text="0", x=24, y=92, w=170, h=56, color=COL_INFO)}
    w["bin"] = ui.Label(" ", x=24, y=158, color=COL_TEXT, value=16)
    card(402, 64, 378, 124, "จอไฟ RGB 16 แท่ง")
    ui.Label("แท่งละ 500 Hz", x=414, y=94, color=COL_TEXT, value=16)
    ui.Label("1 แถว = +6 dB", x=414, y=122, color=COL_TEXT, value=16)
    w["heard"] = ui.Label(" ", x=414, y=154, color=COL_DIM, value=14)
    ui.Label("dB   0 Hz -> 8 kHz", x=12, y=196, color=COL_DIM, value=14)
    w["chart"] = line_chart(12, 218, 400, 120, 0, 90, COL_INFO, POINTS)
    ui.Label("SW5 / SW6 = โน้ตอ้างอิง", x=434, y=226, color=COL_INFO, value=16)
    w["note"] = ui.Label(" ", x=434, y=256, color=COL_WARN, value=20)
    w["help"] = ui.Label("ผิวปาก ฮัม ตบมือ", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, spec, heard):
    k = peak_bin(spec)
    d = db(spec[k])
    w["hz"].text(str(int(k * FS / N)) if d >= FLOOR_DB else "0")
    w["bin"].text("ช่อง %d  %d dB" % (k, d) if d >= FLOOR_DB else "เงียบ")
    w["heard"].text("ฟัง %d ms ต่อ %d ms" % (heard * N * 1000 // FS, TICK_MS))
    # ส่งกราฟทีละจุด ฝั่งจอมีคิวคำสั่ง 64 ช่องและทิ้งคำสั่งกราฟเมื่อคิวเกือบเต็ม
    # จุดที่หายทำให้ทั้งเส้นเลื่อนไปหนึ่งช่อง จึงส่งแค่ POINTS จุด ไม่ส่งครบ 128 ช่อง
    for m in group(spec, POINTS):
        w["chart"].set_next(0, db(m))


# ---- 6) โปรแกรมหลัก ----
def check_buttons(w, down):
    # ขอบกดลงของ SW5/SW6 -> เล่นโน้ตอ้างอิงที่รู้ความถี่ ไมค์จะได้ยินลำโพงของบอร์ดเอง
    # เรียกบ่อย (ห่างไม่เกิน ~50 ms) เพราะเฟิร์มแวร์กรองสั่นทุกครั้งที่อ่าน
    for i in (0, 1):
        d = bool(buttons.pressed(i))
        if d and not down[i]:
            ui.tone(NOTES[i], ui.WAVE_SINE, VOLUME, NOTE_MS)
            w["note"].text(BTN_NAMES[i] + " เล่น %d Hz" % note_hz(NOTES[i]))
        down[i] = d


def main():
    w = build_screen()
    mic.start(sens=SENS, samples=N)   # start() ทิ้งเสียงสองชุดแรกให้เอง
    down, drawn, hold, heard = [False, False], None, None, 0
    t0 = t_show = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            check_buttons(w, down)                   # อ่านปุ่มก่อนฟัง
            hold = hold_max(hold, listen())          # 1) วัด: หนึ่งหน้าต่าง -> 128 ช่อง
            heard += 1
            check_buttons(w, down)                   # และหลังฟัง (การฟังกินเวลาราว 2 หน้าต่าง)
            now = time.ticks_ms()
            if time.ticks_diff(now, t_show) >= TICK_MS:
                t_show = now
                bands = [rows(m) for m in group(hold, 16)]   # 2) สรุป: 128 ช่อง -> 16 แท่ง
                if bands != drawn:                   # 3) จอไฟ RGB วาดใหม่เฉพาะตอนภาพเปลี่ยน
                    draw_bands(bands)
                    drawn = bands
                show(w, hold, heard)                 # 4) โชว์
                hold, heard = None, 0
                ui.poll()
            time.sleep_ms(SAMPLE_MS)
    finally:
        mic.stop()
        try:
            rgbmatrix.clear()
        except OSError:
            pass
    beep("good")                                    # done (ปิดไมค์แล้ว ไม่รบกวนสเปกตรัม)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: ผิวปากที่ 2000 Hz ยอดจะอยู่ช่องที่เท่าไร (2000 / 62.5) และอยู่แท่งที่เท่าไรบนจอไฟ RGB (นับจาก 0)
# 2) ตั้ง N = 128 แล้วดูบรรทัดบนสุด ช่องหนึ่งกว้างกี่ Hz ตอนนี้ เสียงผิวปากยังแยกช่องได้ชัดไหม
#    ได้อะไร เสียอะไร (หน้าต่างสั้นลง = ตอบไวขึ้น แต่แยกความถี่ใกล้ ๆ กันไม่ออก)
# 3) ตั้ง FLOOR_DB ให้ห้องเงียบแล้วไฟดับหมด แต่ผิวปากแล้วติด จดเลขที่ได้ลงใบงาน
