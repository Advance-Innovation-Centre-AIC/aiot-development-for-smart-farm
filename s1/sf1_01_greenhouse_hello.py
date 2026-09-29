# sf1_01_greenhouse_hello.py - โรงเรือนของเราตอนนี้เป็นยังไง
#
# ภารกิจ   : อ่านอุณหภูมิ ความชื้นอากาศ และความกดอากาศ จากเซนเซอร์จริงบนบอร์ด
#            แล้วโชว์เป็นแผงหน้าปัดโรงเรือนบนจอ อัปเดตทุกครึ่งวินาที
# ลองเล่น  : 0) ตั้งค่าชดเชยก่อน: บอร์ดอุ่นจากชิปของตัวเอง จึงอ่านได้สูงกว่าห้อง
#               เทียบเลข "ดิบ" กับเทอร์โมมิเตอร์ในห้อง (หรืออุณหภูมิที่ผู้สอนประกาศ) แล้วใส่ผลต่างใน TEMP_OFFSET
#            1) เป่าลมหายใจใส่บอร์ดเบา ๆ -> ความชื้นพุ่ง (ดูเส้นฟ้าในกราฟ) แล้วค่อย ๆ ลดลง
#            2) เอานิ้วจับบอร์ดตรงเซนเซอร์ค้างไว้ -> เข็มอุณหภูมิขยับขึ้นช้า ๆ
#            3) จดค่าต่ำสุด/สูงสุดที่ทำได้ แข่งกับกลุ่มข้าง ๆ  (SW5 (ปุ่มล่าง) = ล้างสถิติ เริ่มแข่งใหม่)
# ของบนบอร์ดที่ใช้ : SHT40 = อุณหภูมิ + ความชื้น, DPS368 = ความกดอากาศ (hPa)
#            จอไฟ RGB 16x8 บนฐานบอร์ด = อุณหภูมิตัวใหญ่ เขียว/เหลือง/แดง, ปุ่ม SW5 (ปุ่มล่าง)
# บนจอ     : หน้าปัดมีเข็ม (Scale), วงแหวน (Arc), ตัวเลข 7 ส่วน (Seg7),
#            ไฟกะพริบ (Led) และกราฟย้อนหลัง (Chart)
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator

import buttons
import math
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
RUN_MS = 120000      # เดินนานเท่าไร (2 นาที)
TICK_MS = 500        # อ่านเซนเซอร์ทุกกี่ ms
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์ในห้อง (หรืออุณหภูมิที่ผู้สอนประกาศ) แล้วใส่ค่าชดเชย เช่น -9.5
                     # (ห้องแอร์ปกติ ~25-28 C)
                     # ตั้งแล้ว ความชื้นจะถูกแปลงเป็นของห้องให้เองด้วย (ดู room_humidity)
HUM_FIX = True       # แปลงความชื้นเป็นของห้อง (ดู room_humidity) ถ้าเทียบไฮโกรมิเตอร์ในห้องแล้วสูงเกินจริง ให้ตั้ง False
T_WARM = 30          # อุ่นกว่านี้ = สีเหลือง
T_HOT = 35           # ร้อนกว่านี้ = สีแดง
T_GAUGE_MAX = 50     # หน้าปัดอุณหภูมิ 0-50 C
NEEDLE_LEN = 32      # ความยาวเข็มหน้าปัด (พิกเซล) สั้นกว่าวงตัวเลข จะได้ไม่บังเลข

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)                        # บนจอ
MATRIX_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)  # บนจอไฟ RGB


# ---- 2) ฮาร์ดแวร์ ----

# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def read_climate():
    # คืน (อุณหภูมิที่ชดเชยแล้ว, อุณหภูมิดิบ, ความชื้น, ความกด) ตัวที่อ่านไม่ได้เป็น None
    # ชดเชยตรงนี้ที่เดียว ส่วนอื่นของโปรแกรมจึงได้ค่าที่แก้แล้วเสมอ
    t_raw = h = p = None
    for _ in range(3):                  # อ่านพลาดได้บางจังหวะ (บัสไม่ว่าง) จึงลองซ้ำ
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
    # ปุ่มบนฐานบอร์ด (0 = SW5 (ปุ่มล่าง), 1 = SW6 (ปุ่มบน)) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสัญญาณสั่น: ต้องอ่านเห็น "กด" สองครั้งห่างกันเกิน 50 ms จึงนับว่ากดจริง
    # ถ้าอ่านรอบละครั้ง (ทุกครึ่งวินาที) การกดแบบแตะจะหายไปเฉย ๆ
    # เราจึงอ่านปุ่มบ่อย ๆ ระหว่างรอ (ดู wait_ms) แล้วจำไว้ว่า "เพิ่งถูกกด"

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


def matrix_show(t, shown):
    # จอไฟ RGB: อุณหภูมิเป็นเลขจำนวนเต็มตัวใหญ่ สีตามระดับความร้อน
    # วาดใหม่เฉพาะตอนเลขหรือสีเปลี่ยน (สั่งจอไฟทุกรอบทั้งที่ภาพเดิม = เปลืองเวลาเปล่า ๆ)
    want = (int(t + 0.5), heat_level(t))
    if want != shown:
        try:
            rgbmatrix.score(want[0], MATRIX_COLORS[want[1]])
        except OSError:
            pass                # จอไฟ RGB ตอบไม่ทัน: ข้ามภาพนี้ไป ไม่ให้โปรแกรมหยุด
    return want


# ---- 3) สมอง (ตัดสินใจ) ----
def clamp(v, lo, hi):
    # บีบค่าให้อยู่ในช่วง lo..hi
    return max(lo, min(hi, v))


def heat_level(t):
    # 0 = สบาย, 1 = อุ่น, 2 = ร้อน
    if t > T_HOT:
        return 2
    if t > T_WARM:
        return 1
    return 0


def fmt(v, digits=1):
    # ตัวเลขเป็นข้อความ ถ้ายังไม่มีค่า (None) ให้เป็น "--"
    # ใช้ None เท่านั้นเป็นสัญญาณ "ยังไม่มีค่า" ห้ามดูจากขนาดตัวเลข
    # เพราะความกดอากาศจริงก็ราว 1010 อยู่แล้ว
    if v is None:
        return "--"
    return ("%." + str(digits) + "f") % v


def signed(v):
    # ตัวเลขมีเครื่องหมายนำหน้าเสมอ เช่น +0.3 หรือ -1.2
    return ("+" if v >= 0 else "") + ("%.1f" % v)


def sat_pressure(t):
    # ความดันไอน้ำอิ่มตัว (hPa) ที่อุณหภูมิ t C (สูตร Magnus)
    return 6.112 * math.exp(17.62 * t / (243.12 + t))


def room_humidity(h_raw, t_raw, t_room):
    # อากาศอุ่นขึ้นรอบเซนเซอร์ ความชื้นสัมพัทธ์จึงอ่านได้ต่ำกว่าห้อง
    # ไอน้ำในอากาศเท่าเดิม แต่ห้องเย็นกว่า จึงแปลงกลับด้วยอัตราส่วนความดันไออิ่มตัว
    return min(100.0, h_raw * sat_pressure(t_raw) / sat_pressure(t_room))


class MinMax:
    # จำค่าต่ำสุด/สูงสุดที่เคยเห็น เริ่มจาก None ซึ่งแปลว่า "ยังไม่เคยเห็น"
    # (ค่าจริงตัวแรกจะเป็นทั้งต่ำสุดและสูงสุดทันที)

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
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทั้ง 5 ไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def calibration_hint():
    # บรรทัดใต้หัวเรื่อง: เตือนให้ชดเชยก่อน ถ้ายังไม่ได้ตั้ง TEMP_OFFSET
    if TEMP_OFFSET == 0:
        return "ยังไม่ชดเชย: แก้ TEMP_OFFSET ก่อน (ดูใบงาน กิจกรรม 1)", COL_WARN
    return "ชดเชยแล้ว - ลองเป่าลมใส่บอร์ดดูสิ", COL_DIM


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def set_needle(scale, value):
    # Scale วาดแค่ขีดกับตัวเลข ไม่มีเข็มในตัว เราสั่งเข็มเอง:
    # ความยาวเข็มอยู่ 16 บิตบน ค่าที่ชี้อยู่ 16 บิตล่าง
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def build_temp_card(w):
    card(12, 64, 252, 208, "อุณหภูมิ (C)")
    gauge = ui.Scale(x=24, y=92, w=120, h=120, color=COL_TEXT, min=0, max=T_GAUGE_MAX)
    gauge.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)   # หน้าปัดกลม ตัวเลขอยู่ด้านใน
    gauge.ticks(11, 2)                # 11 ขีด มีเลขทุก 2 ขีด = 0 10 20 30 40 50
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
    # กราฟย้อนหลัง: เส้นฟ้า (ชุด 0) = ความชื้น %, เส้นส้ม (ชุดที่เพิ่ม) = อุณหภูมิ C
    w["chart"] = line_chart(12, 280, 400, 62, 0, 100, COL_INFO)
    w["s_hum"] = 0
    w["s_temp"] = w["chart"].add_series(COL_WARN)
    ui.Label("เส้นฟ้า = ความชื้น %", x=428, y=284, color=COL_INFO, value=14)
    ui.Label("เส้นส้ม = อุณหภูมิ C", x=428, y=306, color=COL_WARN, value=14)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
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
    w["live"].value(rounds % 2 if ok else 0)      # Led: 1 = สว่าง, 0 = หรี่ (ไม่ดับมืด)
    if ok:
        w["status"].color(COL_OK)
        w["status"].text("อ่านไปแล้ว " + str(rounds) + " รอบ - ลองเป่าลมใส่บอร์ดดูสิ")
    else:
        w["status"].color(COL_BAD)
        w["status"].text("อ่าน SHT40 ไม่ได้ - เช็กว่าใช้ firmware Dev Kit 2.4.1")


# ---- 5) โปรแกรมหลัก ----
def finish(w, temp_rec, hum_rec):
    # จบรอบ: ดับจอไฟ RGB บอกวิธีเล่นใหม่ และพิมพ์สรุปลง Console
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
    w = build_screen()
    sw5 = Button(0)
    temp_rec, hum_rec = MinMax(), MinMax()
    p_start = None
    shown = None            # เลข+สีที่จอไฟ RGB โชว์อยู่
    rounds = 0
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        t, t_raw, h, p = read_climate()                  # 1) อ่าน
        rounds += 1
        if sw5.pressed_now():                             # 2) SW5 (ปุ่มล่าง) = เริ่มแข่งใหม่
            temp_rec.reset()
            hum_rec.reset()
            beep("tap")
        if t is not None:
            temp_rec.add(t)
            shown = matrix_show(t, shown)                # 3) ทำ: จอไฟ RGB
        if h is not None:
            hum_rec.add(h)
        if p is not None and p_start is None:
            p_start = p                                  # จำความกดตอนเริ่มไว้เทียบ
        show_temp(w, t, t_raw, temp_rec)                 # 4) โชว์บนจอ
        show_humid(w, h, hum_rec)
        show_pressure(w, p, p_start)
        show_status(w, t is not None or h is not None, rounds)
        ui.poll()
        wait_ms(TICK_MS, (sw5,))          # รอ แต่ยังคอยฟังปุ่ม

    finish(w, temp_rec, hum_rec)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ตั้ง TEMP_OFFSET ให้ตรงกับเทอร์โมมิเตอร์ของห้อง แล้วดูว่า "ดิบ" ไม่เปลี่ยน แต่เข็มกับเลขใหญ่เปลี่ยน
# 2) เปลี่ยน T_WARM / T_HOT ให้เหมาะกับพืชของกลุ่มคุณ แล้วดูสีเลขกับจอไฟ RGB
# 3) คำนวณ "ดัชนีความร้อน" แบบง่าย: t + 0.1 * (h - 50) เขียนเป็นฟังก์ชันในส่วน 3)
#    แล้วโชว์เพิ่มอีกบรรทัดในการ์ดความกดอากาศ
# 4) ให้จอไฟ RGB โชว์ความชื้นแทนอุณหภูมิ เมื่อกด SW6 (ปุ่มบน) ค้างไว้
#    ใบ้: sw6 = Button(1) ใส่ใน wait_ms(TICK_MS, (sw5, sw6)) แล้วดู sw6.down
