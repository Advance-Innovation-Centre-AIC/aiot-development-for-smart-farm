# cp1_03_loop_clock.py - หลักการ 1.3: ลูปเดียว สามจังหวะ โดยไม่มีใครรอใคร
#
# หลักการ  : งานแต่ละอย่างมีจังหวะของตัวเอง IMU เร็ว (20 Hz) อากาศช้า (1 Hz) จอพอดีตา (2 Hz)
#            ลูปเดียววนถามนาฬิกา ticks_ms() ว่า "ถึงนัดของงานไหนแล้ว" แล้วทำเฉพาะงานนั้น
#            นัดถัดไป = นัดเดิม + คาบ (ticks_add) ไม่ใช่ "ตอนนี้ + คาบ" คาบเฉลี่ยจึงไม่ค่อย ๆ เลื่อน
#            ticks_diff() ลบเวลาให้ถูกแม้นาฬิกาวนกลับไปเริ่มนับใหม่
#            โหมดง่าย (กด SW5): ทำงานแล้ว sleep_ms(50) ต่อกันเป็นทอด ๆ
#            คาบจริงจึง = 50 + เวลาที่งานใช้ ทุกงานช้ากว่าเป้า และช้าสะสมไปเรื่อย ๆ
# ลองเล่น  : ดูตาราง: เป้า (ms) เทียบกับคาบเฉลี่ยที่วัดได้ ต่ำสุด-สูงสุด (ความแกว่ง = jitter)
#            และ "ช้า" = จำนวนครั้งที่เริ่มช้ากว่าเป้าเกิน LATE_MS
#            กด SW5 (ปุ่มล่าง) สลับโหมด ตัวนับเริ่มใหม่ทุกครั้งที่สลับ แล้วเทียบตัวเลขสองโหมด
# ของบนบอร์ด: IMU BMI270 (sensors.bmi270.motion), SHT40 (อุณหภูมิ), ปุ่ม SW5 (ปุ่มล่าง), ลำโพง
# ในฟาร์ม  : กล่องเดียวในโรงสูบ อ่านแรงสั่นปั๊มถี่ ๆ อ่านอากาศช้า ๆ วาดจอเป็นระยะ
#            ถ้างานหนึ่งนอนรอ งานอื่นก็พลาดจังหวะตามไปด้วย
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#            ตัวเลขเวลาใน Emulator ไม่ใช่เวลาของบอร์ด (เบราว์เซอร์มีจังหวะของตัวเอง)
#            จะสรุปว่าช้าเร็วเท่าไร ต้องดูตัวเลขจากบอร์ดจริง

import buttons
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
IMU_MS = 50          # อ่าน IMU ทุก 50 ms = 20 ครั้ง/วินาที
CLIMATE_MS = 1000    # อ่าน SHT40 ทุก 1000 ms = 1 ครั้ง/วินาที
SCREEN_MS = 500      # อัปเดตตารางบนจอทุก 500 ms = 2 ครั้ง/วินาที
LATE_MS = 10         # เริ่มช้ากว่าเป้าเกินเท่านี้ = นับ "ช้า" 1 ครั้ง
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบเทอร์โมมิเตอร์ในห้องแล้วใส่ค่าชดเชย เช่น -9.5
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(1), SW6 = ปุ่มบน = pressed(0)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_accel():
    # ขนาดความเร่งรวม (m/s2) จาก BMI270 วางนิ่ง = ราว 9.8  อ่านไม่ได้ = None
    try:
        m = sensors.bmi270.motion()
        return (m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) ** 0.5
    except Exception:
        return None


def read_temp():
    # อุณหภูมิจาก SHT40 (ชดเชยแล้ว) ลอง 3 ครั้ง อ่านไม่ได้ = None
    # ถ้าพลาด การรอ 50 ms ตรงนี้ทำให้ IMU เริ่มช้า ดูได้ในคอลัมน์ "ช้า"
    for _ in range(3):
        try:
            return sensors.sht40.temperature_humidity()[0] + TEMP_OFFSET
        except Exception:
            time.sleep_ms(50)
    return None


class Button:
    # ปุ่มบนฐานบอร์ด (1 = SW5 ปุ่มล่าง ขา P17.7, 0 = SW6 ปุ่มบน ขา P17.5) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสั่น 50 ms "ทุกครั้งที่อ่าน" (ไม่ใช่ตัวจับเวลาเบื้องหลัง)
    # จึงต้องอ่านถี่ ๆ (ดู wait_ms) แล้วจำไว้ว่า "เพิ่งถูกกด"
    def __init__(self, index):
        self.index = index
        self.down = False       # ตอนนี้กดค้างอยู่ไหม
        self.clicked = False    # ถูกกดลงมาใหม่ ตั้งแต่ถามครั้งก่อนไหม

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        # True ครั้งเดียวต่อการกดหนึ่งครั้ง (กดค้างไว้ก็ไม่นับซ้ำ)
        fired = self.clicked
        self.clicked = False
        return fired


def wait_ms(ms, btns):
    # รอ ms มิลลิวินาที แต่ระหว่างรอก็อ่านปุ่มทุก 20 ms เพื่อไม่พลาดการกดสั้น ๆ
    t0 = time.ticks_ms()
    while True:
        for b in btns:
            b.sample()
        left = ms - time.ticks_diff(time.ticks_ms(), t0)
        if left <= 0:
            return
        time.sleep_ms(min(20, left))


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
class Task:
    # นัดและสถิติของงานหนึ่งงาน: คาบที่วัดได้จริง เฉลี่ย ต่ำสุด สูงสุด และจำนวนครั้งที่ช้า
    def __init__(self, period, now):
        self.period = period
        self.reset(now)

    def reset(self, now):
        self.due = time.ticks_add(now, self.period)
        self.last = None         # เวลาเริ่มงานครั้งก่อน (None = ยังไม่เคยเริ่ม)
        self.n = self.total = self.late = self.hi = self.work = 0
        self.lo = 99999

    def is_due(self, now):
        # ถึงนัดแล้วหรือยัง ถ้าถึงแล้ว นัดถัดไป = นัดเดิม + คาบ
        if time.ticks_diff(now, self.due) < 0:
            return False
        self.due = time.ticks_add(self.due, self.period)
        if time.ticks_diff(now, self.due) >= 0:
            self.due = time.ticks_add(now, self.period)   # ช้าเกินหนึ่งคาบ: ข้ามนัดที่พลาด ไม่วิ่งไล่
        return True

    def record(self, t):
        # จดเวลาเริ่มงานจริง แล้วอัปเดตสถิติของคาบ
        if self.last is not None:
            dt = time.ticks_diff(t, self.last)
            self.n += 1
            self.total += dt
            self.lo = min(self.lo, dt)
            self.hi = max(self.hi, dt)
            if dt > self.period + LATE_MS:
                self.late += 1
        self.last = t


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
ROWS = ("IMU 20Hz", "อากาศ 1Hz", "จอ 2Hz")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ลูปเดียว สามจังหวะ", x=12, y=6, color=COL_TEXT, value=24)
    w = {"mode": ui.Label(" ", x=12, y=38, color=COL_OK, value=16), "cells": {}}
    w["vals"] = ui.Label(" ", x=500, y=38, color=COL_INFO, value=16)
    table = ui.Table(x=12, y=64, w=768, h=272, cols=6, rows=4)
    for col, width in enumerate((150, 100, 120, 150, 90, 158)):
        table.col_width(col, width)
    w["table"] = table
    for col, s in enumerate(("งาน", "เป้า ms", "เฉลี่ย", "ต่ำ-สูง", "ช้า", "ใช้ ms")):
        table.cell(0, col, s)
    for r in (1, 2, 3):
        table.cell(r, 0, ROWS[r - 1])
        table.cell(r, 1, str((IMU_MS, CLIMATE_MS, SCREEN_MS)[r - 1]))
    w["help"] = ui.Label("ช้า = ช้ากว่าเป้าเกิน %d ms   %s (ล่าง) = สลับโหมด" %
                         (LATE_MS, BTN_NAMES[0]), x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_mode(w, naive):
    w["mode"].text("โหมดง่าย: sleep_ms(%d) ต่อกัน" % IMU_MS if naive else "โหมดนัด: ticks_add")
    w["mode"].color(COL_WARN if naive else COL_OK)


def put(w, r, c, s):
    # เขียนช่องตารางเฉพาะตอนข้อความเปลี่ยน (ส่งข้ามไปคอร์จอน้อยลง)
    if w["cells"].get(r * 8 + c) != s:
        w["cells"][r * 8 + c] = s
        w["table"].cell(r, c, s)


def show_all(w, tasks, a, t):
    # ตาราง: คาบเฉลี่ย ต่ำ-สูง จำนวนครั้งที่ช้า และเวลาที่งานรอบล่าสุดใช้ไป
    for r in (1, 2, 3):
        k = tasks[r - 1]
        put(w, r, 2, "%d" % (k.total // k.n) if k.n else "--")
        put(w, r, 3, "%d-%d" % (k.lo, k.hi) if k.n else "--")
        put(w, r, 4, str(k.late))
        put(w, r, 5, str(k.work))
    w["vals"].text(("a --" if a is None else "a %.2f m/s2" % a) +
                   ("   T --" if t is None else "   T %.1f C" % t))


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    now = time.ticks_ms()
    tasks = (Task(IMU_MS, now), Task(CLIMATE_MS, now), Task(SCREEN_MS, now))
    a = t = None                         # ค่าล่าสุดของ IMU และอุณหภูมิ
    sw5 = Button(1)                      # SW5 = ปุ่มล่าง
    naive, k = False, 0
    show_mode(w, naive)
    t0 = now
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        if sw5.pressed_now():                                  # SW5 กดหนึ่งครั้ง = สลับโหมด
            naive = not naive
            for x in tasks:
                x.reset(now)
            show_mode(w, naive)
            beep("tap")
        if naive:
            # ทุกงานต่อกันเป็นแถว นับรอบแทนการดูนาฬิกา
            k += 1
            due = (True, k % (CLIMATE_MS // IMU_MS) == 0, k % (SCREEN_MS // IMU_MS) == 0)
        else:
            due = [x.is_due(now) for x in tasks]
        for i in (0, 1, 2):
            if due[i]:
                s = time.ticks_ms()
                tasks[i].record(s)                             # จดเวลาเริ่มงานจริง
                if i == 0:
                    a = read_accel()                           # งานที่ 1: IMU
                elif i == 1:
                    t = read_temp()                            # งานที่ 2: อากาศ
                else:
                    show_all(w, tasks, a, t)                   # งานที่ 3: จอ
                    ui.poll()
                tasks[i].work = time.ticks_diff(time.ticks_ms(), s)
        if naive:
            wait_ms(IMU_MS, (sw5,))                            # นอนเต็มคาบ ไม่สนว่างานใช้ไปเท่าไร
        else:
            now = time.ticks_ms()                              # นอนถึงนัดที่ใกล้ที่สุด แล้วตื่นมาดูใหม่
            wait_ms(min([time.ticks_diff(x.due, now) for x in tasks]), (sw5,))
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนกด SW5: ในโหมดง่าย คาบเฉลี่ยของ "อากาศ" จะมากกว่า 1000 ms ไหม มากกว่าเพราะอะไร
# 2) ตั้ง CLIMATE_MS = 250 แล้วดูคอลัมน์ "ช้า" ของ IMU เพิ่มขึ้นไหม (อ่าน SHT40 ถี่ขึ้น = กินเวลามากขึ้น)
# 3) ใน is_due() เปลี่ยน ticks_add(self.due, self.period) เป็น ticks_add(now, self.period)
#    แล้วเทียบคาบเฉลี่ยกับเดิม (นัดจาก "ตอนนี้" = ช้าไปเท่าไร ก็ต่อท้ายไปทุกครั้ง)
