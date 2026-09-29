# sf3_01_pen_guard.py - ยามเฝ้าคอก: เรดาร์นับผู้บุกรุก
# ภารกิจ : ใครเข้าเขตเตือน = บุกรุก -> ไซเรน, จอไฟ RGB วิ่ง INTRUDER, ตัวนับเพิ่ม
# ลองเล่น : ตอนเริ่มถอยห่างบอร์ด แล้วเดินเข้าหา · VR3 = เขตเตือน · SW5/สวิตช์บนจอ = เปิด/ปิดเฝ้า
# ของบนบอร์ดที่ใช้ : เรดาร์ BGT60TR13C, VR3, SW5 (ปุ่มล่าง), ลำโพง, จอไฟ RGB
# บนจอ : ประตูคอก ระยะเป้า การเคลื่อนไหว ตัวนับ สวิตช์ กราฟ บันทึก
# แนวคิด AIoT: เรดาร์เห็นในที่มืด ไม่ใช่กล้อง ละเอียดราว 0.33 ม.
#   Sense (เรดาร์) -> Decide (ยืนยัน CONFIRM_N รอบ) -> Act (ไซเรน จอไฟ ตัวนับ)
# บอร์ด : TESAIoT Dev Kit fw 2.4.1+ · Emulator: VR1 = ระยะเป้า, Shake = เคลื่อนไหว

import buttons
import pots
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
MIN_CM, SPAN_CM = 50, 200  # VR3 ตั้งเขตเตือนได้ 50-250 cm
CONFIRM_N = 3  # ต้องเห็นติดกันกี่รอบถึงเชื่อ (1 ขึ้นไป)
THRESH_DB = 4.0  # เป้าต้องแรงกว่าฉากนิ่งกี่ dB (0.1-60)
MAX_CM = 300  # ระยะไกลสุดบนแถบ ไม้บรรทัด กราฟ (cm)
BTN_NAMES = ("SW5", "SW6")  # ชื่อบนบอร์ด: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน
VOLUME = 25  # ความดังเสียง 0-127
SPEAKER = 40  # ความดังลำโพงรวม 0-100% (fw 2.4.2+)
RUN_MS = 180000  # เวลาเล่นต่อรอบ ms (3 นาที)
SAMPLE_MS = 250  # อ่านเรดาร์ทุกกี่ ms
TICK_MS = 500  # อัปเดตจอทุกกี่ ms (ถี่ไปจอกะพริบ)
MATRIX_MS = 3000  # ส่งภาพจอไฟ RGB ซ้ำทุกกี่ ms

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF  # สี 0xRRGGBB
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
COL_SAFE_BG, COL_ALERT_BG = 0x123322, 0x4A1216  # พื้นแผงประตู: ปลอดภัย / บุกรุก


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    # เบากว่า ui.sfx
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def zone_cm():
    return MIN_CM + pots.read(2) * SPAN_CM // 4095  # VR3


def read_radar():
    # เรดาร์ไม่ตอบ = None ข้ามรอบ
    try:
        r, rr = sensors.radar(), sensors.radar_range()
    except OSError:
        return None
    cm = int(rr["distance_m"] * 100) if rr["target"] else None
    return cm, r["presence"], r["energy"]


def calibrate_radar():
    # radar_config(0) = จำ "ฉากนิ่ง" ใหม่ (ต้องไม่มีใครขยับหน้าบอร์ด)
    # แล้วตั้งเกณฑ์ THRESH_DB: ของที่แรงกว่าฉากนิ่งเท่านี้ถึงนับเป็นเป้า
    try:
        sensors.radar_config(0)
        time.sleep_ms(500)
        sensors.radar_config(THRESH_DB)
        return True
    except OSError:
        return False


class Button:
    # 0 = SW5, 1 = SW6 · อ่านบ่อยใน wait_ms() จะไม่พลาดการแตะสั้น

    def __init__(self, index):
        self.index = index
        self.down = False  # กดค้างอยู่
        self.clicked = False  # กดใหม่ตั้งแต่ถามครั้งก่อน

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        # True ครั้งเดียวต่อการกด
        fired = self.clicked
        self.clicked = False
        return fired


def wait_ms(ms, btns):
    # รอ ms แต่อ่านปุ่มทุก 20 ms
    t0 = time.ticks_ms()
    while True:
        for b in btns:
            b.sample()
        left = ms - time.ticks_diff(time.ticks_ms(), t0)
        if left <= 0:
            return
        time.sleep_ms(min(20, left))


def show_matrix(armed, inside, count):
    # ปิดเฝ้า = มืด, บุกรุก = INTRUDER, ปกติ = จำนวนครั้ง
    try:
        if not armed:
            rgbmatrix.clear()
        elif inside:
            rgbmatrix.scroll("INTRUDER", rgbmatrix.RED, 60)
        else:
            rgbmatrix.score(count, rgbmatrix.GREEN)
    except OSError:
        pass  # รอบส่งซ้ำ (MATRIX_MS) จะวาดใหม่


def siren():
    # เลขโน้ต MIDI ไม่ใช่เฮิรตซ์
    for note in (81, 74, 81, 74):
        ui.tone(note, ui.WAVE_SQUARE, VOLUME, 140)
        time.sleep_ms(150)


# ---- 3) สมอง (ตัดสินใจ) ----
def smooth(hist, cm):
    # median 5 ค่า กันเรดาร์กระโดดข้ามเฟรมจากคลื่นสะท้อนหลายทาง
    hist.append(cm)
    if len(hist) > 5:
        hist.pop(0)
    return sorted(hist)[len(hist) // 2]


def in_zone(armed, cm, alert_cm):
    return armed and cm is not None and cm < alert_cm


def confirm(inside, streak, near):
    # สถานะใหม่ต้องยืนครบ CONFIRM_N รอบติดกันถึงจะเชื่อ
    # คืน (สถานะที่เชื่อแล้ว, จำนวนรอบที่เห็นสถานะใหม่ติดกัน)
    if near == inside:
        return inside, 0
    if streak + 1 >= CONFIRM_N:
        return near, 0
    return inside, streak + 1


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    box = ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)
    return box


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # จุด >= ความกว้าง LVGL จึงไม่วาดจุดกลม (รับ 10-400 จุด)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_gate(w):
    w["gate"] = card(12, 44, 470, 196, "ประตูคอก (เรดาร์)")
    w["state"] = ui.Label("ปลอดภัย", x=24, y=72, color=COL_OK, value=28)
    w["cm"] = ui.Seg7(text="---", x=24, y=112, w=150, h=56, color=COL_INFO)
    ui.Label("cm", x=182, y=140, color=COL_DIM, value=16)
    w["led"] = ui.Led(x=300, y=116, w=28, h=28, color=COL_WARN, value=0)
    ui.Label("การเคลื่อนไหว", x=338, y=118, color=COL_DIM, value=16)
    w["bar"] = ui.Bar(x=24, y=176, w=446, h=20, min=0, max=MAX_CM, value=0)
    ruler = ui.Scale(x=24, y=198, w=446, h=34, color=COL_DIM, min=0, max=MAX_CM)
    ruler.ticks(13, 2)  # ขีดทุก 25 cm เลขทุก 50 cm


def build_counter(w):
    card(492, 44, 288, 196, "ผู้บุกรุก (ครั้ง)")
    w["n"] = ui.Seg7(text="0", x=504, y=72, w=150, h=60, color=COL_BAD)
    w["sw"] = ui.Switch(x=504, y=146, w=64, h=32, value=1)
    w["arm"] = ui.Label(" ", x=578, y=150, color=COL_OK, value=16)
    w["zone"] = ui.Label("เขตเตือน - cm (VR3)", x=504, y=196, color=COL_INFO, value=16)
    show_arm(w, True)


def build_log(w):
    ui.Label("ฟ้า = ระยะเป้า   แดง = เขตเตือน", x=12, y=244, color=COL_DIM, value=14)
    w["chart"] = line_chart(12, 264, 400, 74, 0, MAX_CM, COL_INFO)
    w["s_zone"] = w["chart"].add_series(COL_BAD)
    card(422, 250, 358, 88, "บันทึกเรดาร์")
    w["last"] = ui.Label("ยังไม่มี", x=434, y=280, color=COL_TEXT, value=16)
    w["energy"] = ui.Label("energy: -", x=434, y=308, color=COL_DIM, value=14)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ยามเฝ้าคอก (เรดาร์)", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    build_gate(w)
    build_counter(w)
    build_log(w)
    w["status"] = ui.Label(" ", x=12, y=352, color=COL_WARN, value=16)
    ui.poll()
    return w


def say(w, text, col):
    # บรรทัดสถานะล่างจอ
    w["status"].color(col)
    w["status"].text(text)


def show_arm(w, armed):
    w["arm"].text("ระบบเฝ้า: " + ("เปิด" if armed else "ปิด") + " (" + BTN_NAMES[0] + ")")
    w["arm"].color(COL_OK if armed else COL_DIM)


def show_gate(w, g, near, cm, moving):
    w["gate"].color(COL_ALERT_BG if g.inside else COL_SAFE_BG)
    w["state"].color(COL_BAD if g.inside else (COL_OK if g.armed else COL_DIM))
    w["state"].text("มีผู้บุกรุก!" if g.inside else ("ปลอดภัย" if g.armed else "ปิดระบบเฝ้า"))
    w["cm"].text("---" if cm is None else str(cm))
    w["bar"].value(MAX_CM if cm is None else min(cm, MAX_CM))  # ไม่มีเป้า = แถบเต็ม
    w["bar"].color(COL_BAD if near else COL_OK)
    w["led"].value(1 if moving else 0)  # ไฟส้ม = มีการเคลื่อนไหว


def show_zone(w, alert_cm, cm, energy):
    w["zone"].text("เขตเตือน %d cm (VR3)" % alert_cm)
    w["energy"].text("energy: %.0f" % energy)
    w["chart"].set_next(0, MAX_CM if cm is None else min(cm, MAX_CM))
    w["chart"].set_next(w["s_zone"], alert_cm)


# ---- 5) โปรแกรมหลัก ----
class Guard:
    def __init__(self):
        self.armed, self.inside, self.streak, self.count = True, False, 0, 0
        self.hist = []  # 5 ค่าล่าสุดให้ smooth()
        self.radar_ok = True
        self.shown, self.sent = None, 0  # ภาพจอไฟ RGB ล่าสุด และเวลาส่ง


def start_guard(w):
    for i in (3, 2, 1):
        say(w, "ถอยห่างบอร์ด! จำฉากนิ่งใน %d วินาที" % i, COL_WARN)
        time.sleep_ms(1000)
    if calibrate_radar():
        say(w, "เฝ้าคอกแล้ว เดินเข้าหาบอร์ดได้เลย", COL_OK)
    else:
        say(w, "จำฉากไม่สำเร็จ ใช้ฉากเดิม", COL_BAD)
    beep(72, 79)


def on_arm(w, g, sw5):
    # ui.poll() อยู่ที่นี่ที่เดียว
    armed = g.armed
    for ev in ui.poll():
        if ev["handle"] == w["sw"].id() and ev["type"] == "toggled":
            armed = ev["value"] == 1
    if sw5.pressed_now():
        armed = not armed
        w["sw"].value(1 if armed else 0)
    if armed != g.armed:
        g.armed, g.inside, g.streak = armed, False, 0
        show_arm(w, armed)
        beep(76)


def on_radar(w, g, ok):
    # เขียนเฉพาะตอนเปลี่ยน
    if ok != g.radar_ok:
        g.radar_ok = ok
        say(w, "เรดาร์กลับมาแล้ว" if ok else "เรดาร์ไม่ตอบ กำลังลองใหม่",
            COL_OK if ok else COL_BAD)


def decide(w, g, cm, alert_cm):
    # ส่งเสียง/นับ เฉพาะตอนสถานะเปลี่ยน
    was = g.inside
    near = in_zone(g.armed, cm, alert_cm)
    g.inside, g.streak = confirm(g.inside, g.streak, near)
    if g.inside and not was:
        g.count += 1
        w["n"].text(str(g.count))
        msg = "คนที่ %d  ระยะ %d cm" % (g.count, cm)
        w["last"].text(msg)
        print("ผู้บุกรุก" + msg)
        siren()
    elif was and not g.inside:
        beep(79, 84)  # ออกไปแล้ว
    return near


def matrix_tick(g):
    # วาดเมื่อภาพเปลี่ยน + ส่งซ้ำ
    look = (g.armed, g.inside, g.count)
    now = time.ticks_ms()
    if look != g.shown or time.ticks_diff(now, g.sent) >= MATRIX_MS:
        show_matrix(g.armed, g.inside, g.count)
        g.shown, g.sent = look, now


def finish(w, count):
    show_matrix(False, False, count)
    say(w, "จบรอบแล้ว - กด Program to Device เพื่อเล่นใหม่", COL_WARN)
    ui.poll()
    print("เฝ้าคอกครบเวลา พบผู้บุกรุก", count, "ครั้ง")


def main():
    if hasattr(ui, "volume"):  # fw 2.4.1 ข้าม
        ui.volume(SPEAKER)
    w = build_screen()
    start_guard(w)
    g, sw5 = Guard(), Button(0)  # SW5
    every = TICK_MS // SAMPLE_MS  # TICK_MS ต้อง >= SAMPLE_MS
    tick, t0 = 0, time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            on_arm(w, g, sw5)  # 0) ปุ่ม
            got = read_radar()  # 1) อ่าน
            on_radar(w, g, got is not None)
            if got is not None:
                raw, moving, energy = got
                cm = None if raw is None else smooth(g.hist, raw)
                alert_cm = zone_cm()
                near = decide(w, g, cm, alert_cm)  # 2) ตัดสิน + 3) ทำ
                if tick % every == 0:  # 4) โชว์
                    show_gate(w, g, near, cm, moving)
                    show_zone(w, alert_cm, cm, energy)
            matrix_tick(g)
            tick += 1
            wait_ms(SAMPLE_MS, (sw5,))  # รอ + ฟังปุ่ม
    finally:  # Stop กลางทางก็ล้างจอไฟ RGB และสรุป
        finish(w, g.count)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) CONFIRM_N = 1 เดินผ่านเร็ว ๆ 5 รอบ เทียบกับ 3: แบบไหนเตือนผิดน้อย แบบไหนช้ากว่า
# 2) นับเฉพาะตอนมีการเคลื่อนไหว (ส่ง moving เข้า in_zone()) แล้วยืนนิ่ง - แบบไหนเหมาะคอกกลางคืน
# 3) ให้ SW6 (Button(1)) ล้างตัวนับเป็น 0 (ใส่ใน wait_ms ด้วย)
