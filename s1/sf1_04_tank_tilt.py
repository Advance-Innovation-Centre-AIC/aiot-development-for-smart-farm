# sf1_04_tank_tilt.py - แท็งก์น้ำ/รถไถเอียงเกินไหม
# ภารกิจ : IMU วัดมุมเอียง เกินมุมปลอดภัย = เตือนแดง, เขย่าแรง = นับแรงกระแทก
# ลองเล่น : วางนิ่ง 1 วินาทีตอนเริ่ม = ศูนย์ แล้วเอียง/เขย่า · เกม: ถือเดินให้จุดอยู่ในเป้าสีฟ้า
# ของบนบอร์ดที่ใช้ : IMU, ไฟ RGB_RED, SW5 = ตั้งศูนย์, SW6 = ล้างตัวนับ, จอไฟ RGB, ลำโพง
# บอร์ด : TESAIoT Dev Kit fw 2.4.1+ และ BENTO Emulator

import buttons
import gpio
import math
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
SPEAKER = 40  # ความดังลำโพงรวม 0-100% (fw 2.4.2+)
VOLUME = 25  # ความดังเสียง 0-127
SAFE_DEG = 20  # เอียงเกินกี่องศา = อันตราย
BUMP_G = 1.8  # แรงรวมเกินกี่ g = กระแทก
BTN_NAMES = ("SW5", "SW6")  # ชื่อบนบอร์ด: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน
RUN_MS = 120000
TICK_MS = 500
ZERO_SAMPLES = 5
GAUGE_MAX = 60  # หน้าปัด 0-60 องศา
NEEDLE_LEN = 48
TARGET_DEG = 3  # เกม: เอียงไม่เกินนี้ = อยู่ในเป้า

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)
MATRIX_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)
G = 9.81
RANGE = SAFE_DEG * 1.5  # เอียงเท่านี้ จุดบนจอไฟ RGB ชนขอบ


# ---- 2) ฮาร์ดแวร์ ----

TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def read_motion():
    try:
        ax, ay, az, _, _, _ = sensors.bmi270.motion()
        return ax, ay, az
    except Exception:
        return None


def led_named(name):
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
    # ปุ่มบนฐาน (0 = SW5 ล่าง, 1 = SW6 บน) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสั่น 50 ms จึงต้องอ่านบ่อย ๆ ระหว่างรอ (wait_ms) แล้วจำว่า "เพิ่งกด"

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


def measure_zero():
    # เฉลี่ย ZERO_SAMPLES ครั้ง ท่าตอนเริ่ม = ศูนย์ (วางบนโต๊ะก็เอียงราว 39 องศาอยู่แล้ว)
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


def draw_bubble(bx, by, color):
    buf = bytearray(64)
    dots = ((7, 3, rgbmatrix.BLUE), (8, 3, rgbmatrix.BLUE), (7, 4, rgbmatrix.BLUE),
            (8, 4, rgbmatrix.BLUE), (bx, by, color), (bx + 1, by, color),
            (bx, by + 1, color), (bx + 1, by + 1, color))
    for x, y, c in dots:
        i = y * 8 + (x >> 1)
        if x & 1:
            buf[i] = (buf[i] & 0x0F) | (c << 4)
        else:
            buf[i] = (buf[i] & 0xF0) | c
    try:
        rgbmatrix.blit(buf)
    except OSError:
        pass


def matrix_update(roll, pitch, level, drawn):
    bx, by = bubble_cell(roll, pitch)
    want = (bx, by, MATRIX_COLORS[level])
    if want != drawn:
        draw_bubble(bx, by, want[2])
    return want


# ---- 3) สมอง (ตัดสินใจ) ----
def tilt_angles(ax, ay, az):
    # คืน (roll, pitch) องศา จากทิศแรงโน้มถ่วง (abs(az): หงายหรือคว่ำก็ได้)
    roll = math.degrees(math.atan2(ay, abs(az)))
    pitch = math.degrees(math.atan2(-ax, math.sqrt(ay * ay + az * az)))
    return roll, pitch


def g_force(ax, ay, az):
    return math.sqrt(ax * ax + ay * ay + az * az) / G


def tilt_level(tilt):
    # 0 = ปลอดภัย, 1 = ระวัง (เกิน 70 %), 2 = อันตราย
    if tilt > SAFE_DEG:
        return 2
    if tilt > SAFE_DEG * 0.7:
        return 1
    return 0


def bubble_cell(roll, pitch):
    bx = int(round(7 + max(-1.0, min(1.0, pitch / RANGE)) * 7))
    by = int(round(3 + max(-1.0, min(1.0, roll / RANGE)) * 3))
    return bx, by


class Record:

    def __init__(self):
        self.last_bump = 0
        self.reset()

    def reset(self):
        self.bumps = 0
        self.max_tilt = 0
        self.g_max = 1.0
        self.on_target_ms = 0

    def add(self, tilt, g, now, dt_ms):
        self.max_tilt = max(self.max_tilt, tilt)
        self.g_max = max(self.g_max, g)
        if tilt <= TARGET_DEG:
            self.on_target_ms += dt_ms
        if g > BUMP_G and time.ticks_diff(now, self.last_bump) > 500:
            self.bumps += 1
            self.last_bump = now
            return True
        return False


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def set_needle(scale, value):
    # Scale ไม่มีเข็มในตัว: ความยาวเข็ม 16 บิตบน ค่าที่ชี้ 16 บิตล่าง
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_tilt_card(w):
    card(12, 64, 380, 272, "มุมเอียง (องศา)")
    gauge = ui.Scale(x=24, y=92, w=160, h=160, color=COL_TEXT, min=0, max=GAUGE_MAX)
    gauge.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)
    gauge.ticks(13, 2)
    gauge.prop(ui.PROP_SCALE_NEEDLE_COLOR, COL_WARN)
    w["gauge"] = gauge
    w["angles"] = ui.Label("หน้า-หลัง 0\nซ้าย-ขวา 0", x=200, y=110, color=COL_OK, value=20)
    w["verdict"] = ui.Label("...", x=24, y=262, color=COL_WARN, value=28)
    w["max"] = ui.Label("", x=24, y=306, color=COL_DIM, value=16)


def build_bump_card(w):
    card(402, 64, 378, 164, "แรงกระแทก (เต็มแถบ = 3 g)")
    w["seg_bump"] = ui.Seg7(text="0", x=414, y=94, w=90, h=40, color=COL_WARN)
    ui.Label("ครั้ง", x=512, y=104, color=COL_DIM, value=16)
    w["lbl_g"] = ui.Label("แรงรวม 1.00 g", x=414, y=144, color=COL_TEXT, value=16)
    w["bar_g"] = ui.Bar(x=414, y=174, w=354, h=18, min=0, max=300, value=0)


def build_chart(w):
    ui.Label("เส้นฟ้า = มุมเอียง   เส้นแดง = มุมปลอดภัย", x=402, y=236,
             color=COL_DIM, value=14)
    w["chart"] = line_chart(402, 256, 378, 80, 0, GAUGE_MAX, COL_INFO)
    w["s_tilt"] = 0
    w["s_safe"] = w["chart"].add_series(COL_BAD)


def build_screen():
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
    w["angles"].text("หน้า-หลัง %d\nซ้าย-ขวา %d" % (roll, pitch))
    w["angles"].color(col)
    w["verdict"].color(col)
    if level == 2:
        w["verdict"].text("อันตราย! เอียง " + str(tilt) + " องศา")
    else:
        w["verdict"].text("ระวัง" if level == 1 else "ปลอดภัย")
    w["max"].text("เอียงมากสุด " + str(rec.max_tilt) + " องศา   อยู่ในเป้า " +
                  str(rec.on_target_ms // 1000) + " วินาที")


def show_bumps(w, g, rec):
    w["seg_bump"].text(str(rec.bumps))
    w["lbl_g"].text("แรงรวม %.2f g  สูงสุด %.2f g" % (g, rec.g_max))
    w["bar_g"].value(int(min(300, g * 100)))
    w["bar_g"].color(COL_BAD if g > BUMP_G else COL_INFO)


# ---- 5) โปรแกรมหลัก ----
def finish(w, alarm, rec):
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
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    alarm = led_named("RGB_RED")
    sw5, sw6 = Button(0), Button(1)
    rec = Record()
    roll0, pitch0 = measure_zero()     # ท่าตอนเริ่ม = ศูนย์
    w["help"].color(COL_DIM)
    w["help"].text(BTN_NAMES[0] + " (ล่าง) = ตั้งศูนย์ใหม่   " + BTN_NAMES[1] + " (บน) = ล้างตัวนับ")
    drawn = None
    was_danger = False
    last_ms = time.ticks_ms()
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        m = read_motion()                                   # 1) อ่าน
        if m is None:
            w["verdict"].color(COL_BAD)
            w["verdict"].text("อ่าน IMU ไม่ได้")
            time.sleep_ms(500)
            continue
        raw_roll, raw_pitch = tilt_angles(m[0], m[1], m[2])
        if sw5.pressed_now():  # SW5: ท่าตอนนี้ = ศูนย์
            roll0, pitch0 = raw_roll, raw_pitch
            beep("tap")
        if sw6.pressed_now():  # SW6: ล้างตัวนับ
            rec.reset()
            beep("stop")
        roll, pitch = raw_roll - roll0, raw_pitch - pitch0  # 2) คิด
        tilt = int(max(abs(roll), abs(pitch)))
        g = g_force(m[0], m[1], m[2])
        now = time.ticks_ms()
        if rec.add(tilt, g, now, time.ticks_diff(now, last_ms)):
            beep("hit")
        last_ms = now
        level = tilt_level(tilt)
        if level == 2 and not was_danger:
            beep("bad")  # เพิ่งเอียงเกิน: เตือนครั้งเดียว
        was_danger = level == 2
        set_alarm(alarm, was_danger)                        # 3) ทำ
        drawn = matrix_update(roll, pitch, level, drawn)
        show_tilt(w, roll, pitch, tilt, level, rec)         # 4) โชว์
        show_bumps(w, g, rec)
        w["chart"].set_next(w["s_tilt"], min(GAUGE_MAX, tilt))
        w["chart"].set_next(w["s_safe"], int(SAFE_DEG))
        ui.poll()
        wait_ms(TICK_MS, (sw5, sw6))

    finish(w, alarm, rec)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ปรับ SAFE_DEG ให้เหมาะกับงานของกลุ่ม (แท็งก์บนเสา / รถไถ / ชั้นวางผัก) แล้วดูเส้นแดงในกราฟขยับ
# 2) ให้เตือนก็ต่อเมื่อเอียงเกินค้างนาน 5 วินาที (กันเตือนผิดตอนหยิบบอร์ดขึ้นมาดู)
#    ใบ้: นับรอบที่ level == 2 ติดกัน ใน main() ครบ 10 รอบ (10 x TICK_MS 500 ms) ค่อยเตือน
# 3) เอียงบอร์ดไปทางขวา จุดบนจอไฟ RGB วิ่งไปทางไหน? ถ้าวิ่งกลับด้าน แก้เครื่องหมายใน bubble_cell()
# 4) เกม "ประคองแท็งก์" (บรรทัด "อยู่ในเป้า"): ถือบอร์ดเดินรอบโต๊ะ ให้จุดอยู่ในเป้าสีฟ้า
#    ทำให้ยากขึ้นด้วย TARGET_DEG = 2 หรือให้มีเสียง ui.tone(...) ทุกครั้งที่ครบ 5 วินาที (ครั้งเดียว ไม่ใช่ทุกรอบ)
