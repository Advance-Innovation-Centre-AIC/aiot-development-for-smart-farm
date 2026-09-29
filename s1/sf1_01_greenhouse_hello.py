# sf1_01_greenhouse_hello.py - โรงเรือนของเราตอนนี้เป็นยังไง
# ภารกิจ : อ่านอุณหภูมิ ความชื้น ความกดอากาศจากเซนเซอร์จริง โชว์เป็นแผงหน้าปัดโรงเรือน
# ลองเล่น : 0) ตั้ง TEMP_OFFSET ก่อน (บอร์ดอุ่นจากชิปเอง เทียบกับเทอร์โมมิเตอร์ในห้อง)
#   1) เป่าลมใส่บอร์ด = ความชื้นพุ่ง  2) จับเซนเซอร์ค้าง = อุณหภูมิขึ้น  3) แข่งค่าต่ำ/สูงสุด (SW5 = เริ่มใหม่)
# ของบนบอร์ดที่ใช้ : SHT40, DPS368, จอไฟ RGB 16x8, SW5 (ปุ่มล่าง)
# บอร์ด : TESAIoT Dev Kit fw 2.4.1+ และ BENTO Emulator

import buttons
import math
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
SPEAKER = 40  # ความดังลำโพงรวม 0-100% (fw 2.4.2+)
VOLUME = 25  # ความดังเสียง 0-127
RUN_MS = 120000
TICK_MS = 500
TEMP_OFFSET = 0.0  # ค่าชดเชย: เทียบเทอร์โมมิเตอร์ในห้อง เช่น -9.5
HUM_FIX = True  # แปลงความชื้นเป็นของห้อง (สูงเกินจริง = False)
T_WARM = 30  # อุ่นกว่านี้ = เหลือง
T_HOT = 35  # ร้อนกว่านี้ = แดง
T_GAUGE_MAX = 50
NEEDLE_LEN = 32

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)                        # บนจอ
MATRIX_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)  # บนจอไฟ RGB


# ---- 2) ฮาร์ดแวร์ ----

TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def read_climate():
    # คืน (อุณหภูมิชดเชยแล้ว, ดิบ, ความชื้น, ความกด) อ่านไม่ได้ = None
    t_raw = h = p = None
    for _ in range(3):  # อ่านพลาดได้บางจังหวะ จึงลองซ้ำ
        try:
            t_raw = sensors.sht40.temperature()
            h = sensors.sht40.humidity()
            break
        except Exception:
            time.sleep_ms(20)
    try:
        p = sensors.dps368.pressure()
    except Exception:
        pass
    t = None if t_raw is None else t_raw + TEMP_OFFSET
    if h is not None and t is not None and TEMP_OFFSET != 0 and HUM_FIX:
        h = room_humidity(h, t_raw, t)
    return t, t_raw, h, p


class Button:
    # ปุ่มบนฐาน (0 = SW5 ล่าง, 1 = SW6 บน) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสั่น 50 ms จึงต้องอ่านบ่อย ๆ ระหว่างรอ (wait_ms)

    def __init__(self, index):
        self.index = index
        self.down = False
        self.clicked = False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        fired = self.clicked
        self.clicked = False
        return fired


def wait_ms(ms, btns):
    t0 = time.ticks_ms()
    while True:
        for b in btns:
            b.sample()
        left = ms - time.ticks_diff(time.ticks_ms(), t0)
        if left <= 0:
            return
        time.sleep_ms(min(20, left))


def matrix_show(t, shown):
    # จอไฟ RGB: อุณหภูมิตัวใหญ่ สีตามระดับ วาดใหม่เฉพาะตอนเปลี่ยน
    want = (int(t + 0.5), heat_level(t))
    if want != shown:
        try:
            rgbmatrix.score(want[0], MATRIX_COLORS[want[1]])
        except OSError:
            pass
    return want


# ---- 3) สมอง (ตัดสินใจ) ----
def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def heat_level(t):
    if t > T_HOT:
        return 2
    if t > T_WARM:
        return 1
    return 0


def fmt(v, digits=1):
    # ใช้ None เป็นสัญญาณ "ยังไม่มีค่า" (ความกดจริงก็ราว 1010)
    if v is None:
        return "--"
    return ("%." + str(digits) + "f") % v


def signed(v):
    return ("+" if v >= 0 else "") + ("%.1f" % v)


def sat_pressure(t):
    # ความดันไอน้ำอิ่มตัว (hPa) สูตร Magnus
    return 6.112 * math.exp(17.62 * t / (243.12 + t))


def room_humidity(h_raw, t_raw, t_room):
    # รอบเซนเซอร์อุ่นกว่าห้อง %RH จึงต่ำกว่า แปลงกลับด้วยอัตราส่วนความดันไออิ่มตัว
    return min(100.0, h_raw * sat_pressure(t_raw) / sat_pressure(t_room))


class MinMax:
    # จำค่าต่ำสุด/สูงสุด (None = ยังไม่เคยเห็น)

    def __init__(self):
        self.reset()

    def reset(self):
        self.lo = None
        self.hi = None

    def add(self, v):
        self.lo = v if self.lo is None else min(self.lo, v)
        self.hi = v if self.hi is None else max(self.hi, v)

    def text(self):
        return "ต่ำสุด " + fmt(self.lo) + "  สูงสุด " + fmt(self.hi)


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def calibration_hint():
    if TEMP_OFFSET == 0:
        return "ยังไม่ชดเชย: แก้ TEMP_OFFSET ก่อน (ดูใบงาน กิจกรรม 1)", COL_WARN
    return "ชดเชยแล้ว - ลองเป่าลมใส่บอร์ดดูสิ", COL_DIM


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def set_needle(scale, value):
    # Scale ไม่มีเข็มในตัว: ความยาวเข็ม 16 บิตบน ค่าที่ชี้ 16 บิตล่าง
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def build_temp_card(w):
    card(12, 64, 252, 208, "อุณหภูมิ (C)")
    gauge = ui.Scale(x=24, y=92, w=120, h=120, color=COL_TEXT, min=0, max=T_GAUGE_MAX)
    gauge.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)
    gauge.ticks(11, 2)
    gauge.prop(ui.PROP_SCALE_NEEDLE_COLOR, COL_WARN)
    w["gauge"] = gauge
    w["seg_t"] = ui.Seg7(text="--", x=156, y=104, w=100, h=40, color=COL_OK)
    w["raw_t"] = ui.Label("ดิบ --", x=156, y=156, color=COL_DIM, value=14)
    ui.Label("ชดเชย " + ("%.1f" % TEMP_OFFSET), x=156, y=178, color=COL_DIM, value=14)
    w["mm_t"] = ui.Label("ต่ำสุด --  สูงสุด --", x=24, y=240, color=COL_DIM, value=14)


def build_humid_card(w):
    card(272, 64, 252, 208, "ความชื้นอากาศ (%)")
    w["arc_h"] = ui.Arc(x=284, y=92, w=120, h=120, min=0, max=100, value=0)
    w["arc_h"].color(COL_INFO)
    w["seg_h"] = ui.Seg7(text="--", x=416, y=104, w=100, h=40, color=COL_INFO)
    ui.Label("เป่าลม = พุ่ง", x=416, y=156, color=COL_DIM, value=14)
    w["mm_h"] = ui.Label("ต่ำสุด --  สูงสุด --", x=284, y=240, color=COL_DIM, value=14)


def build_pressure_card(w):
    card(532, 64, 248, 208, "ความกดอากาศ (hPa)")
    w["seg_p"] = ui.Seg7(text="--", x=544, y=100, w=200, h=40, color=COL_WARN)
    w["trend_p"] = ui.Label("เทียบตอนเริ่ม --", x=544, y=152, color=COL_TEXT, value=14)
    ui.Label("ลดเรื่อย ๆ = ฝนอาจมา", x=544, y=176, color=COL_DIM, value=14)
    w["live"] = ui.Led(x=546, y=236, w=20, h=20, color=COL_OK, value=0)
    ui.Label("ไฟกะพริบ = กำลังอ่านสด", x=576, y=236, color=COL_DIM, value=14)


def build_trend(w):
    # กราฟ: เส้นฟ้า = ความชื้น %, เส้นส้ม = อุณหภูมิ C
    w["chart"] = line_chart(12, 280, 400, 62, 0, 100, COL_INFO)
    w["s_hum"] = 0
    w["s_temp"] = w["chart"].add_series(COL_WARN)
    ui.Label("เส้นฟ้า = ความชื้น %", x=428, y=284, color=COL_INFO, value=14)
    ui.Label("เส้นส้ม = อุณหภูมิ C", x=428, y=306, color=COL_WARN, value=14)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โรงเรือนของเราตอนนี้", x=12, y=6, color=COL_TEXT, value=24)
    hint, hint_color = calibration_hint()
    ui.Label(hint, x=12, y=38, color=hint_color, value=16)
    w = {}
    build_temp_card(w)
    build_humid_card(w)
    build_pressure_card(w)
    build_trend(w)
    w["status"] = ui.Label("กำลังอ่านเซนเซอร์...", x=12, y=356, color=COL_WARN, value=16)
    ui.poll()
    return w


def show_temp(w, t, t_raw, rec):
    if t is None:
        return
    w["seg_t"].text(fmt(t))
    w["seg_t"].color(LEVEL_COLORS[heat_level(t)])
    set_needle(w["gauge"], clamp(t, 0, T_GAUGE_MAX))
    w["raw_t"].text("ดิบ " + fmt(t_raw))
    w["mm_t"].text(rec.text())
    w["chart"].set_next(w["s_temp"], int(clamp(t, 0, 100)))


def show_humid(w, h, rec):
    if h is None:
        return
    w["seg_h"].text(fmt(h))
    w["arc_h"].value(int(h))
    w["mm_h"].text(rec.text())
    w["chart"].set_next(w["s_hum"], int(h))


def show_pressure(w, p, p_start):
    if p is None:
        return
    w["seg_p"].text(fmt(p, 0))
    w["trend_p"].text("เทียบตอนเริ่ม " + signed(p - p_start) + " hPa")


def show_status(w, ok, rounds):
    w["live"].value(rounds % 2 if ok else 0)
    if ok:
        w["status"].color(COL_OK)
        w["status"].text("อ่านไปแล้ว " + str(rounds) + " รอบ - ลองเป่าลมใส่บอร์ดดูสิ")
    else:
        w["status"].color(COL_BAD)
        w["status"].text("อ่าน SHT40 ไม่ได้ - เช็กว่าใช้ firmware Dev Kit 2.4.1")


# ---- 5) โปรแกรมหลัก ----
def finish(w, temp_rec, hum_rec):
    try:
        rgbmatrix.clear()
    except OSError:
        pass
    w["status"].color(COL_WARN)
    w["status"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()
    print("สรุป: T", fmt(temp_rec.lo), "-", fmt(temp_rec.hi), "C  RH",
          fmt(hum_rec.lo), "-", fmt(hum_rec.hi), "%")


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    sw5 = Button(0)
    temp_rec, hum_rec = MinMax(), MinMax()
    p_start = None
    shown = None
    rounds = 0
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        t, t_raw, h, p = read_climate()                  # 1) อ่าน
        rounds += 1
        if sw5.pressed_now():  # 2) SW5 = เริ่มแข่งใหม่
            temp_rec.reset()
            hum_rec.reset()
            beep("tap")
        if t is not None:
            temp_rec.add(t)
            shown = matrix_show(t, shown)                # 3) ทำ: จอไฟ RGB
        if h is not None:
            hum_rec.add(h)
        if p is not None and p_start is None:
            p_start = p
        show_temp(w, t, t_raw, temp_rec)                 # 4) โชว์บนจอ
        show_humid(w, h, hum_rec)
        show_pressure(w, p, p_start)
        show_status(w, t is not None or h is not None, rounds)
        ui.poll()
        wait_ms(TICK_MS, (sw5,))

    finish(w, temp_rec, hum_rec)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ตั้ง TEMP_OFFSET ให้ตรงกับเทอร์โมมิเตอร์ของห้อง แล้วดูว่า "ดิบ" ไม่เปลี่ยน แต่เข็มกับเลขใหญ่เปลี่ยน
# 2) เปลี่ยน T_WARM / T_HOT ให้เหมาะกับพืชของกลุ่มคุณ แล้วดูสีเลขกับจอไฟ RGB
# 3) คำนวณ "ดัชนีความร้อน" แบบง่าย: t + 0.1 * (h - 50) เขียนเป็นฟังก์ชันในส่วน 3)
#    แล้วโชว์เพิ่มอีกบรรทัดในการ์ดความกดอากาศ
# 4) ให้จอไฟ RGB โชว์ความชื้นแทนอุณหภูมิ เมื่อกด SW6 (ปุ่มบน) ค้างไว้
#    ใบ้: sw6 = Button(1) ใส่ใน wait_ms(TICK_MS, (sw5, sw6)) แล้วดู sw6.down
