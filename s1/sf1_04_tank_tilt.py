# sf1_04_tank_tilt.py - แท็งก์น้ำ/รถไถเอียงเกินไหม
#
# ภารกิจ   : ใช้เซนเซอร์ความเคลื่อนไหว (IMU) วัดมุมเอียงของบอร์ด แทน "แท็งก์น้ำบนเสา"
#            หรือ "รถไถในแปลงลาดชัน" ถ้าเอียงเกินมุมปลอดภัย -> เตือนแดง + ไฟติด
#            ถ้าโดนเขย่าแรง ๆ (เช่นรถตกหลุม) -> นับเป็น "แรงกระแทก"
# ลองเล่น  : 0) ตอนเริ่ม วางบอร์ดนิ่ง ๆ 1 วินาที = ท่านั้นคือ "ศูนย์" (บอร์ดวางบนโต๊ะก็เอียงอยู่แล้ว)
#            1) ค่อย ๆ เอียงบอร์ดไปทางซ้าย/ขวา/หน้า/หลัง หามุมที่ไฟเตือนติด
#            2) เขย่าบอร์ดแรง ๆ ดูตัวนับแรงกระแทกขึ้น
#            3) เกม "ประคองแท็งก์": ถือบอร์ดเดิน ให้จุดอยู่ในเป้าสีฟ้า นับวินาทีที่อยู่ในเป้า (SW6 (ปุ่มบน) = เริ่มใหม่)
# แนวคิด    : แรงโน้มถ่วงชี้ลงพื้นเสมอ บอร์ดจึงรู้ว่าตัวเองเอียงไปเท่าไร (atan2)
# ของบนบอร์ดที่ใช้ : IMU BMI270, ไฟ RGB_RED บนบอร์ด = เอียงอันตราย,
#            ปุ่ม SW5 (ปุ่มล่าง) = ตั้งศูนย์ใหม่ (ท่าตอนกด = ศูนย์), SW6 (ปุ่มบน) = ล้างตัวนับ
#            จอไฟ RGB = "ระดับน้ำ (bubble level)" จุดวิ่งตามการเอียง เขียว/เหลือง/แดง
#            ลำโพง: มีเสียงตอนเริ่มเอียงเกินมุมปลอดภัย และทุกครั้งที่โดนกระแทก
# บนจอ     : หน้าปัดมุมเอียงมีเข็ม (Scale), ตัวเลข 7 ส่วน (Seg7), ไฟเตือน (Led),
#            แถบแรงรวม (Bar) และกราฟมุมเอียงเทียบมุมปลอดภัย (Chart)
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator

import buttons
import gpio
import math
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
SAFE_DEG = 20        # เอียงเกินกี่องศาถือว่าอันตราย
BUMP_G = 1.8         # แรงรวมเกินกี่เท่าของแรงโน้มถ่วงถือว่า "กระแทก"
BTN_NAMES = ("SW5", "SW6")   # ชื่อที่พิมพ์บนบอร์ด: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน
RUN_MS = 120000
TICK_MS = 200
ZERO_SAMPLES = 5     # ตอนเริ่มอ่าน 5 ครั้ง (1 วินาที) แล้วเฉลี่ยเป็น "ศูนย์"
GAUGE_MAX = 60       # หน้าปัดมุมเอียง 0-60 องศา
NEEDLE_LEN = 48      # ความยาวเข็มหน้าปัด (พิกเซล) สั้นกว่าวงตัวเลข จะได้ไม่บังเลข
CHART_EVERY = 1      # ใส่จุดในกราฟทุกกี่รอบ (1 = ทุก 0.2 วินาที) กราฟ 400 จุดจึงย้อนหลังได้ราว 80 วินาที
TARGET_DEG = 3       # เกม "ประคองแท็งก์": เอียงไม่เกินกี่องศานับว่า "อยู่ในเป้า"

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)
MATRIX_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)
G = 9.81
RANGE = SAFE_DEG * 1.5   # เอียงเท่านี้ จุดบนจอไฟ RGB วิ่งไปชนขอบ


# ---- 2) ฮาร์ดแวร์ ----
def read_motion():
    """คืน (ax, ay, az) หน่วย m/s2 ถ้าอ่าน IMU ไม่ได้คืน None"""
    try:
        ax, ay, az, _, _, _ = sensors.bmi270.motion()
        return ax, ay, az
    except Exception:
        return None


def led_named(name):
    """หา LED ด้วยชื่อ ไม่ใช่เลข: บน Dev Kit ดวง LED1/LED2 (เลข 0, 1) อยู่บน SoM
    มองไม่เห็น ดวงที่เห็นคือ RGB_RED / RGB_GREEN / RGB_BLUE"""
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def set_alarm(alarm, danger):
    if alarm is None:
        return
    if danger:
        alarm.on()
    else:
        alarm.off()


class Button:
    """ปุ่มบนฐานบอร์ด (0 = SW5 (ปุ่มล่าง), 1 = SW6 (ปุ่มบน)) ที่ไม่พลาดการกดสั้น ๆ
    เฟิร์มแวร์กรองสัญญาณสั่น: ต้องอ่านเห็น "กด" สองครั้งห่างกันเกิน 50 ms จึงนับว่ากดจริง
    ถ้าอ่านรอบละครั้ง (ทุกครึ่งวินาที) การกดแบบแตะจะหายไปเฉย ๆ
    เราจึงอ่านปุ่มบ่อย ๆ ระหว่างรอ (ดู wait_ms) แล้วจำไว้ว่า "เพิ่งถูกกด" """

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
        """True ครั้งเดียวต่อการกดหนึ่งครั้ง (กดค้างไว้ก็ไม่นับซ้ำ)"""
        fired = self.clicked
        self.clicked = False
        return fired


def wait_ms(ms, btns):
    """รอ ms มิลลิวินาที แต่ระหว่างรอก็อ่านปุ่มทุก 20 ms เพื่อไม่พลาดการกดสั้น ๆ"""
    t0 = time.ticks_ms()
    while True:
        for b in btns:
            b.sample()
        left = ms - time.ticks_diff(time.ticks_ms(), t0)
        if left <= 0:
            return
        time.sleep_ms(min(20, left))


def measure_zero():
    """ตอนเริ่ม: อ่าน IMU ZERO_SAMPLES ครั้งแล้วเฉลี่ย ท่าที่บอร์ดวางอยู่ = ศูนย์
    (Dev Kit วางบนโต๊ะก็เอียงอยู่แล้วราว 39 องศา ถ้าไม่ตั้งศูนย์ จะขึ้น "อันตราย" ตั้งแต่เฟรมแรก)"""
    sum_roll = sum_pitch = 0.0
    n = 0
    for _ in range(ZERO_SAMPLES):
        m = read_motion()
        if m is not None:
            roll, pitch = tilt_angles(m[0], m[1], m[2])
            sum_roll += roll
            sum_pitch += pitch
            n += 1
        ui.poll()
        time.sleep_ms(TICK_MS)
    if n == 0:
        return 0.0, 0.0
    return sum_roll / n, sum_pitch / n


def put(buf, x, y, c):
    """ตั้งสีจุด (x, y) ในเฟรม 64 ไบต์ของจอไฟ RGB (จุดละ 4 บิต)"""
    i = y * 8 + (x >> 1)
    if x & 1:
        buf[i] = (buf[i] & 0x0F) | (c << 4)
    else:
        buf[i] = (buf[i] & 0xF0) | c


def draw_bubble(bx, by, color):
    """จุด 2x2 บนจอไฟ RGB 16x8 ที่ (bx, by)  จุดฟ้าตรงกลาง = เป้าที่ต้องเล็ง"""
    buf = bytearray(64)
    for (x, y) in ((7, 3), (8, 3), (7, 4), (8, 4)):
        put(buf, x, y, rgbmatrix.BLUE)
    for (x, y) in ((bx, by), (bx + 1, by), (bx, by + 1), (bx + 1, by + 1)):
        put(buf, x, y, color)
    try:
        rgbmatrix.blit(buf)
    except OSError:
        pass                    # จอไฟ RGB ตอบไม่ทัน: ข้ามภาพนี้ไป ไม่ให้โปรแกรมหยุด


def matrix_update(roll, pitch, level, drawn):
    """วาดจอไฟ RGB ใหม่เฉพาะตอนจุดขยับหรือเปลี่ยนสี แล้วคืนภาพที่วาดอยู่"""
    bx, by = bubble_cell(roll, pitch)
    want = (bx, by, MATRIX_COLORS[level])
    if want != drawn:
        draw_bubble(bx, by, want[2])
    return want


# ---- 3) สมอง (ตัดสินใจ) ----
def tilt_angles(ax, ay, az):
    """คืน (roll, pitch) เป็นองศา จากทิศของแรงโน้มถ่วง
    ใช้ abs(az) เพื่อให้วางบอร์ดหงายหรือคว่ำก็ได้ค่าใกล้ 0 เหมือนกัน"""
    roll = math.degrees(math.atan2(ay, abs(az)))
    pitch = math.degrees(math.atan2(-ax, math.sqrt(ay * ay + az * az)))
    return roll, pitch


def g_force(ax, ay, az):
    """แรงรวมเป็น "กี่เท่าของแรงโน้มถ่วง" (วางนิ่ง = 1.0 g)"""
    return math.sqrt(ax * ax + ay * ay + az * az) / G


def tilt_level(tilt):
    """0 = ปลอดภัย, 1 = ระวัง (เกิน 70 % ของมุมปลอดภัย), 2 = อันตราย"""
    if tilt > SAFE_DEG:
        return 2
    if tilt > SAFE_DEG * 0.7:
        return 1
    return 0


def bubble_cell(roll, pitch):
    """แปลงมุมเอียงเป็นตำแหน่งจุดบนจอไฟ RGB: x 0..14, y 0..6 (กลาง = 7, 3)"""
    bx = int(round(7 + max(-1.0, min(1.0, pitch / RANGE)) * 7))   # ซ้าย-ขวา = แนวนอนของจอไฟ
    by = int(round(3 + max(-1.0, min(1.0, roll / RANGE)) * 3))    # หน้า-หลัง = แนวตั้งของจอไฟ
    return bx, by


class Record:
    """สถิติของรอบนี้: จำนวนครั้งที่โดนกระแทก, มุมเอียงมากสุด, แรงรวมสูงสุด
    และเวลาที่ประคองแท็งก์ให้อยู่ในเป้าได้ (เกม)"""

    def __init__(self):
        self.last_bump = 0
        self.reset()

    def reset(self):
        self.bumps = 0
        self.max_tilt = 0
        self.g_max = 1.0
        self.on_target_ms = 0

    def add(self, tilt, g, now, dt_ms):
        """เก็บค่ารอบนี้ คืน True ถ้าเป็นแรงกระแทกครั้งใหม่ (ห่างครั้งก่อนเกินครึ่งวินาที)"""
        self.max_tilt = max(self.max_tilt, tilt)
        self.g_max = max(self.g_max, g)
        if tilt <= TARGET_DEG:
            self.on_target_ms += dt_ms            # เวลาจริงของรอบนี้ที่แท็งก์ตั้งตรง
        if g > BUMP_G and time.ticks_diff(now, self.last_bump) > 500:
            self.bumps += 1
            self.last_bump = now
            return True
        return False


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    """การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทั้ง 5 ไฟล์ใช้แบบเดียวกัน)"""
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def set_needle(scale, value):
    """Scale วาดแค่ขีดกับตัวเลข ไม่มีเข็มในตัว เราสั่งเข็มเอง:
    ความยาวเข็มอยู่ 16 บิตบน ค่าที่ชี้อยู่ 16 บิตล่าง"""
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    """กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)"""
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_tilt_card(w):
    card(12, 64, 380, 272, "มุมเอียง (องศา)")
    gauge = ui.Scale(x=24, y=92, w=160, h=160, color=COL_TEXT, min=0, max=GAUGE_MAX)
    gauge.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)   # หน้าปัดกลม ตัวเลขอยู่ด้านใน
    gauge.ticks(13, 2)                # 13 ขีด มีเลขทุก 2 ขีด = 0 10 20 ... 60
    gauge.prop(ui.PROP_SCALE_NEEDLE_COLOR, COL_WARN)
    w["gauge"] = gauge
    # ชิปบน Dev Kit วางหันแบบนี้: roll = atan2(ay, az) คือเอียง "หน้า-หลัง" ของบอร์ด
    ui.Label("หน้า-หลัง (roll)", x=200, y=96, color=COL_DIM, value=14)
    w["seg_roll"] = ui.Seg7(text="0", x=200, y=118, w=100, h=40, color=COL_OK)
    ui.Label("ซ้าย-ขวา (pitch)", x=200, y=170, color=COL_DIM, value=14)
    w["seg_pitch"] = ui.Seg7(text="0", x=200, y=192, w=100, h=40, color=COL_OK)
    w["verdict"] = ui.Label("กำลังตั้งศูนย์...", x=24, y=262, color=COL_WARN, value=28)
    w["max"] = ui.Label("เอียงมากสุด 0 องศา", x=24, y=306, color=COL_DIM, value=16)


def build_bump_card(w):
    card(402, 64, 378, 164, "แรงกระแทก")
    w["seg_bump"] = ui.Seg7(text="0", x=414, y=94, w=90, h=40, color=COL_WARN)
    ui.Label("ครั้ง", x=512, y=104, color=COL_DIM, value=16)
    w["lamp"] = ui.Led(x=724, y=92, w=40, h=40, color=COL_BAD, value=0)
    w["lbl_g"] = ui.Label("แรงรวม 1.00 g", x=414, y=144, color=COL_TEXT, value=16)
    w["bar_g"] = ui.Bar(x=414, y=174, w=354, h=18, min=0, max=300, value=100)
    ui.Label("แถบแรงรวม: เต็มแถบ = 3 g", x=414, y=198, color=COL_DIM, value=14)


def build_chart(w):
    """กราฟ: เส้นฟ้า (ชุด 0) = มุมเอียง, เส้นแดง = มุมปลอดภัย"""
    ui.Label("เส้นฟ้า = มุมเอียง   เส้นแดง = มุมปลอดภัย", x=402, y=236,
             color=COL_DIM, value=14)
    w["chart"] = line_chart(402, 256, 378, 80, 0, GAUGE_MAX, COL_INFO)
    w["s_tilt"] = 0
    w["s_safe"] = w["chart"].add_series(COL_BAD)


def build_screen():
    """สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง"""
    ui.screen()
    time.sleep_ms(200)
    ui.Label("แท็งก์น้ำ/รถไถ เอียงเกินไหม", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("มุมปลอดภัยไม่เกิน " + str(SAFE_DEG) + " องศา - ลองเอียงบอร์ดดู",
             x=12, y=38, color=COL_DIM, value=16)
    w = {}
    build_tilt_card(w)
    build_bump_card(w)
    build_chart(w)
    w["help"] = ui.Label("วางบอร์ดนิ่ง ๆ ตอนเริ่ม = ศูนย์ (กำลังวัด...)", x=12, y=352,
                         color=COL_WARN, value=16)
    ui.poll()
    return w


def show_tilt(w, roll, pitch, tilt, level, rec):
    col = LEVEL_COLORS[level]
    set_needle(w["gauge"], min(GAUGE_MAX, tilt))
    w["seg_roll"].text(str(int(roll)))
    w["seg_pitch"].text(str(int(pitch)))
    w["seg_roll"].color(col)
    w["seg_pitch"].color(col)
    w["verdict"].color(col)
    if level == 2:
        w["verdict"].text("อันตราย! เอียง " + str(tilt) + " องศา")
    else:
        w["verdict"].text("ระวัง" if level == 1 else "ปลอดภัย")
    w["max"].text("เอียงมากสุด " + str(rec.max_tilt) + " องศา   อยู่ในเป้า " +
                  str(rec.on_target_ms // 1000) + " วินาที")
    w["lamp"].value(1 if level == 2 else 0)        # Led: 0 = หรี่ (ไม่ดับมืด)


def show_bumps(w, g, rec):
    w["seg_bump"].text(str(rec.bumps))
    w["lbl_g"].text("แรงรวม %.2f g  สูงสุด %.2f g" % (g, rec.g_max))
    w["bar_g"].value(int(min(300, g * 100)))
    w["bar_g"].color(COL_BAD if g > BUMP_G else COL_INFO)


def show_imu_error(w):
    w["verdict"].color(COL_BAD)
    w["verdict"].text("อ่าน IMU ไม่ได้")


# ---- 5) โปรแกรมหลัก ----
def finish(w, alarm, rec):
    """จบรอบ: ดับไฟเตือน ล้างจอไฟ RGB บอกวิธีเล่นใหม่ และพิมพ์สรุปลง Console"""
    set_alarm(alarm, False)
    try:
        rgbmatrix.clear()
    except OSError:
        pass
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()
    print("เอียงมากสุด", rec.max_tilt, "องศา  แรงกระแทก", rec.bumps, "ครั้ง")


def main():
    w = build_screen()
    alarm = led_named("RGB_RED")       # ไฟแดงบนบอร์ด = เอียงอันตราย
    sw5, sw6 = Button(0), Button(1)   # SW5 = ปุ่มล่าง, SW6 = ปุ่มบน
    rec = Record()
    roll0, pitch0 = measure_zero()     # ท่าตอนเริ่ม = ศูนย์
    w["help"].color(COL_DIM)
    w["help"].text(BTN_NAMES[0] + " (ล่าง) = ตั้งศูนย์ใหม่   " + BTN_NAMES[1] + " (บน) = ล้างตัวนับ")
    drawn = None                       # ภาพที่จอไฟ RGB วาดอยู่
    was_danger = False
    tick = 0
    last_ms = time.ticks_ms()          # เวลาของรอบก่อน (ใช้จับเวลาเกม)
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        m = read_motion()                                   # 1) อ่าน
        if m is None:
            show_imu_error(w)
            ui.poll()
            time.sleep_ms(500)
            continue
        raw_roll, raw_pitch = tilt_angles(m[0], m[1], m[2])
        if sw5.pressed_now():                          # SW5 (ปุ่มล่าง): ท่าตอนนี้ = ศูนย์
            roll0, pitch0 = raw_roll, raw_pitch
            ui.sfx(ui.SFX_UI_SELECT)
        if sw6.pressed_now():                          # SW6 (ปุ่มบน): ล้างตัวนับ
            rec.reset()
            ui.sfx(ui.SFX_UI_BACK)
        roll, pitch = raw_roll - roll0, raw_pitch - pitch0  # 2) คิด
        tilt = int(max(abs(roll), abs(pitch)))
        g = g_force(m[0], m[1], m[2])
        now = time.ticks_ms()
        if rec.add(tilt, g, now, time.ticks_diff(now, last_ms)):
            ui.sfx(ui.SFX_SHOOT_HIT)                        # โดนกระแทก!
        last_ms = now
        level = tilt_level(tilt)
        if level == 2 and not was_danger:
            ui.sfx(ui.SFX_SHOOT_LOSE_LIFE)                  # เพิ่งเอียงเกิน -> เตือนครั้งเดียว
        was_danger = level == 2
        set_alarm(alarm, was_danger)                        # 3) ทำ
        drawn = matrix_update(roll, pitch, level, drawn)
        show_tilt(w, roll, pitch, tilt, level, rec)         # 4) โชว์
        show_bumps(w, g, rec)
        if tick % CHART_EVERY == 0:
            w["chart"].set_next(w["s_tilt"], min(GAUGE_MAX, tilt))
            w["chart"].set_next(w["s_safe"], int(SAFE_DEG))
        tick += 1
        ui.poll()
        wait_ms(TICK_MS, (sw5, sw6))       # รอ แต่ยังคอยฟังปุ่ม

    finish(w, alarm, rec)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ปรับ SAFE_DEG ให้เหมาะกับงานของกลุ่ม (แท็งก์บนเสา / รถไถ / ชั้นวางผัก) แล้วดูเส้นแดงในกราฟขยับ
# 2) ให้เตือนก็ต่อเมื่อเอียงเกินค้างนาน 2 วินาที (กันเตือนผิดตอนหยิบบอร์ดขึ้นมาดู)
#    ใบ้: นับรอบที่ level == 2 ติดกัน ใน main() ครบ 10 รอบ (10 x 200 ms) ค่อยเตือน
# 3) เอียงบอร์ดไปทางขวา จุดบนจอไฟ RGB วิ่งไปทางไหน? ถ้าวิ่งกลับด้าน แก้เครื่องหมายใน bubble_cell()
# 4) เกม "ประคองแท็งก์" (บรรทัด "อยู่ในเป้า"): ถือบอร์ดเดินรอบโต๊ะ ให้จุดอยู่ในเป้าสีฟ้า
#    ทำให้ยากขึ้นด้วย TARGET_DEG = 2 หรือให้มีเสียง ui.tone(...) ทุกครั้งที่ครบ 5 วินาที (ครั้งเดียว ไม่ใช่ทุกรอบ)
