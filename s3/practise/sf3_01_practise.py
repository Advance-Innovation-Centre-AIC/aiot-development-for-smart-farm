# sf3_01_practise.py - แบบฝึกเติมโค้ด (Code Quest ระดับ 3): confirm()
#
# วิธีเล่น  : ไฟล์นี้เหมือน sf3_01_pen_guard.py ทุกอย่าง ยกเว้นฟังก์ชัน confirm() ในส่วน "3) สมอง"
#            ที่เว้นช่อง ____ (ขีดล่างสี่ตัว) ไว้ 3 ช่อง: A, B, C
#            เติมให้ครบแล้วรัน โปรแกรมจะตรวจ confirm() 7 กรณีก่อนเปิดจอ (self_test)
#            ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง
#            ยังไม่ถูก = Console บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ)
# ถ้าเจอ   : NameError: name '____' isn't defined = ยังมีช่องที่ไม่ได้เติม (ตั้งใจให้หยุดชัด ๆ แบบนี้)
# ติดขัด?  : ใช้บันไดช่วยเหลือในใบงาน (คำใบ้ 3 ขั้นอยู่ท้ายใบงาน) หรือรันไฟล์ตัวอย่างเต็ม
#            sf3_01_pen_guard.py เพื่อไปต่อก่อน แล้วค่อยกลับมาเทียบกับของตัวเอง
# เฉลย     : โจทย์เพิ่ม (โบนัส) เฉลยคาบหน้า · ทำที่บ้านใน BENTO Emulator ได้
#
# (ทำจาก sf3_01_pen_guard.py 0dadd4c63dc3)

import buttons
import pots
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
MIN_CM, SPAN_CM = 50, 200    # VR3 ตั้งเขตเตือนได้ 50-250 cm
CONFIRM_N = 3                # ต้องเห็นติดกันกี่รอบถึงจะเชื่อ กันคลื่นสะท้อนวูบเดียว
THRESH_DB = 4.0              # เป้าต้องแรงกว่าฉากนิ่งกี่ dB (ตั้งได้ 0.1-60 · ค่าที่ตัวอย่าง IDE สอบเทียบไว้)
MAX_CM = 300                 # ระยะไกลสุดที่แถบ ไม้บรรทัด และกราฟแสดง
BTN_NAMES = ("SW5", "SW6")   # ชื่อที่พิมพ์บนบอร์ด: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)
RUN_MS = 180000
SAMPLE_MS = 250              # อ่านเรดาร์ทุก 0.25 วินาที (CONFIRM_N = 3 รอบ = ราว 0.75 วินาที)
TICK_MS = 500                # อัปเดตจอทุก 0.5 วินาที (ถี่กว่านี้จอกะพริบและกินแรงบอร์ด)
MATRIX_MS = 3000             # ส่งภาพจอไฟ RGB ซ้ำทุก 3 วินาที เผื่อบอร์ดทำภาพหล่นตอนงานยุ่ง
# INTRUDER วิ่งครบรอบละราว 3 วินาที (8 ตัว x 4 จุด + จอกว้าง 16 จุด = ราว 49 ก้าว x 60 ms)
# ส่งซ้ำทุก 3 วินาทีจึงเริ่มใหม่ตรงรอยต่อพอดี ไม่ตัดกลางคำ

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
COL_SAFE_BG, COL_ALERT_BG = 0x123322, 0x4A1216   # พื้นแผงประตู: เขียวเข้ม = ปลอดภัย, แดงเข้ม = บุกรุก


# ---- 2) ฮาร์ดแวร์ ----
def zone_cm():
    return MIN_CM + pots.read(2) * SPAN_CM // 4095   # VR3: เขตเตือน 50-250 cm


def read_radar():
    # อ่านเรดาร์สองคำสั่ง: radar() = การเคลื่อนไหว + พลังงานสะท้อน, radar_range() = ระยะเป้าแรกที่เจอ
    # เรดาร์ทำงานอยู่อีกแกนของชิป ถ้าตอบไม่ทัน (OSError) คืน None ให้โปรแกรมหลักข้ามรอบนี้ไป
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
    # ปุ่มบนฐานบอร์ด (0 = SW5 (ปุ่มล่าง), 1 = SW6 (ปุ่มบน)) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสัญญาณสั่น: ต้องอ่านเห็น "กด" สองครั้งห่างกันเกิน 50 ms จึงนับว่ากดจริง
    # ถ้าอ่านรอบละครั้ง การกดแบบแตะจะหายไปเฉย ๆ
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
        # True ครั้งเดียวต่อการกดหนึ่งครั้ง (กดค้างไว้ก็ไม่นับซ้ำ ไม่สลับรัว ๆ)
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


def show_matrix(armed, inside, count):
    # จอไฟ RGB: ปิดระบบ = มืด, บุกรุก = INTRUDER วิ่งสีแดง, ปกติ = จำนวนครั้งสีเขียว
    # คำสั่งวาด clear()/score() หยุดตัวหนังสือวิ่งเดิมให้เอง จึงไม่ต้องสั่งหยุดก่อน
    try:
        if not armed:
            rgbmatrix.clear()
        elif inside:
            rgbmatrix.scroll("INTRUDER", rgbmatrix.RED, 60)
        else:
            rgbmatrix.score(count, rgbmatrix.GREEN)
    except OSError:
        pass                    # จอไฟ RGB ตอบไม่ทัน: รอบส่งซ้ำ (MATRIX_MS) จะวาดให้ใหม่


def siren():
    # สลับสองโน้ตสี่ครั้ง (เลขโน้ต MIDI ไม่ใช่เฮิรตซ์) เล่นตอนเริ่มบุกรุกเท่านั้น
    for note in (81, 74, 81, 74):
        ui.tone(note, ui.WAVE_SQUARE, 100, 140)
        time.sleep_ms(150)


# ---- 3) สมอง (ตัดสินใจ) ----
def smooth(hist, cm):
    # median 5 ค่า กันเรดาร์กระโดดข้ามเฟรมจากคลื่นสะท้อนหลายทาง
    hist.append(cm)
    if len(hist) > 5:
        hist.pop(0)
    return sorted(hist)[len(hist) // 2]


def in_zone(armed, cm, alert_cm):
    # "ใกล้" = ระบบเฝ้าเปิดอยู่ + มีเป้า + เป้าอยู่ในเขตเตือน
    return armed and cm is not None and cm < alert_cm


def confirm(inside, streak, near):
    # สถานะใหม่ต้องยืนครบ CONFIRM_N รอบติดกันถึงจะเชื่อ
    # คืน (สถานะที่เชื่อแล้ว, จำนวนรอบที่เห็นสถานะใหม่ติดกัน)
    if near == inside:
        return inside, ____             # ช่อง A: เห็นเหมือนสถานะเดิม ตัวนับควรกลับไปเป็นเท่าไร?
    if streak + 1 >= ____:              # ช่อง B: ต้องเห็นติดกันกี่รอบถึงจะเชื่อ? (ดูส่วน 1 ตั้งค่า)
        return near, 0
    return inside, ____                 # ช่อง C: ยังไม่ครบ ตัวนับควรเป็นเท่าไร?


def self_test():
    # ตรวจ confirm() 7 กรณีก่อนเปิดจอ (ใช้ได้เมื่อ CONFIRM_N ตั้งแต่ 2 ขึ้นไป)
    n = CONFIRM_N
    for a, want in (((False, 0, False), (False, 0)), ((False, 0, True), (False, 1)),
                    ((False, n - 2, True), (False, n - 1)), ((False, n - 1, True), (True, 0)),
                    ((True, 1, True), (True, 0)), ((True, n - 1, False), (False, 0)),
                    ((False, 1, False), (False, 0))):
        got = confirm(*a)
        if got != want:
            print("ยังไม่ถูก: confirm", a, "ควรได้", want, "แต่ได้", got)
            return False
    print("ผ่าน! confirm() ถูกทั้ง 7 กรณี")
    return True


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (คืนกล่องไว้ เผื่ออยากเปลี่ยนสีพื้นทีหลัง)
    box = ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)
    return box


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
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
    ruler.ticks(13, 2)          # ไม้บรรทัดใต้แถบ: ขีดทุก 25 cm มีเลขทุก 50 cm (Scale ไม่มีเข็ม)


def build_counter(w):
    card(492, 44, 288, 196, "ผู้บุกรุก (ครั้ง)")
    w["n"] = ui.Seg7(text="0", x=504, y=72, w=150, h=60, color=COL_BAD)
    w["sw"] = ui.Switch(x=504, y=146, w=64, h=32, value=1)
    w["arm"] = ui.Label(" ", x=578, y=150, color=COL_OK, value=16)
    w["zone"] = ui.Label("เขตเตือน - cm (VR3)", x=504, y=196, color=COL_INFO, value=16)
    show_arm(w, True)


def build_log(w):
    # กราฟ: ฟ้า (ชุด 0) = ระยะเป้า, แดง = เขตเตือน · เส้นฟ้าต่ำกว่าเส้นแดง = เป้าอยู่ในเขต
    ui.Label("ฟ้า = ระยะเป้า   แดง = เขตเตือน", x=12, y=244, color=COL_DIM, value=14)
    w["chart"] = line_chart(12, 264, 400, 74, 0, MAX_CM, COL_INFO)
    w["s_zone"] = w["chart"].add_series(COL_BAD)
    card(422, 250, 358, 88, "บันทึกเรดาร์")
    w["last"] = ui.Label("ยังไม่มี", x=434, y=280, color=COL_TEXT, value=16)
    w["energy"] = ui.Label("energy: -", x=434, y=308, color=COL_DIM, value=14)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
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
    # บรรทัดสถานะล่างจอ (บรรทัดเดียว เว้นมุมขวาล่างให้ปุ่ม Console)
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
    w["bar"].value(MAX_CM if cm is None else min(cm, MAX_CM))   # ไม่มีเป้า = แถบเต็ม (ไกล)
    w["bar"].color(COL_BAD if near else COL_OK)
    w["led"].value(1 if moving else 0)   # ไฟส้มสว่าง = เรดาร์เห็นการเคลื่อนไหว (0 = หรี่ ไม่ดับมืด)


def show_zone(w, alert_cm, cm, energy):
    w["zone"].text("เขตเตือน %d cm (VR3)" % alert_cm)
    w["energy"].text("energy: %.0f" % energy)
    w["chart"].set_next(0, MAX_CM if cm is None else min(cm, MAX_CM))
    w["chart"].set_next(w["s_zone"], alert_cm)


# ---- 5) โปรแกรมหลัก ----
class Guard:
    # ทุกอย่างที่ยามต้องจำข้ามรอบ รวมไว้ที่เดียว
    def __init__(self):
        self.armed, self.inside, self.streak, self.count = True, False, 0, 0
        self.hist = []              # ระยะล่าสุด 5 ค่า สำหรับ smooth()
        self.radar_ok = True
        self.shown, self.sent = None, 0   # ภาพที่จอไฟ RGB วาดอยู่ และส่งไปเมื่อไร


def start_guard(w):
    # นับถอยหลังให้คนออกจากหน้าบอร์ดก่อน แล้วสั่งเรดาร์จำ "ฉากนิ่ง" ใหม่
    for i in (3, 2, 1):
        say(w, "ถอยห่างบอร์ด! จำฉากนิ่งใน %d วินาที" % i, COL_WARN)
        time.sleep_ms(1000)
    if calibrate_radar():
        say(w, "เฝ้าคอกแล้ว เดินเข้าหาบอร์ดได้เลย", COL_OK)
    else:
        say(w, "จำฉากไม่สำเร็จ ใช้ฉากเดิม", COL_BAD)
    ui.sfx(ui.SFX_UI_START)


def on_arm(w, g, sw5):
    # SW5 (ปุ่มล่าง) หรือแตะสวิตช์บนจอ = เปิด/ปิดระบบเฝ้า (ui.poll() อยู่ที่นี่ที่เดียว)
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
        ui.sfx(ui.SFX_UI_SELECT if armed else ui.SFX_UI_BACK)


def on_radar(w, g, ok):
    # เรดาร์ตอบ/ไม่ตอบ: เขียนบรรทัดสถานะเฉพาะตอนเปลี่ยน แล้ววนต่อ โปรแกรมไม่หยุด
    if ok != g.radar_ok:
        g.radar_ok = ok
        say(w, "เรดาร์กลับมาแล้ว" if ok else "เรดาร์ไม่ตอบ กำลังลองใหม่",
            COL_OK if ok else COL_BAD)


def decide(w, g, cm, alert_cm):
    # ตัดสินว่าบุกรุกไหม แล้วส่งเสียง/นับ เฉพาะตอนสถานะเปลี่ยน (ไม่ใช่ทุกรอบ)
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
        ui.sfx(ui.SFX_PONG_WIN)     # ออกไปแล้ว = ปลอดภัย
    return near


def matrix_tick(g):
    # จอไฟ RGB: วาดใหม่ตอนภาพเปลี่ยน และส่งซ้ำทุก MATRIX_MS เผื่อภาพก่อนหน้าหล่นหาย
    look = (g.armed, g.inside, g.count)
    now = time.ticks_ms()
    if look != g.shown or time.ticks_diff(now, g.sent) >= MATRIX_MS:
        show_matrix(g.armed, g.inside, g.count)
        g.shown, g.sent = look, now


def finish(w, count):
    # จบรอบ: ล้างจอไฟ RGB บอกวิธีเล่นใหม่ และพิมพ์สรุปลง Console
    show_matrix(False, False, count)
    say(w, "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่", COL_WARN)
    ui.poll()
    print("เฝ้าคอกครบเวลา พบผู้บุกรุก", count, "ครั้ง")


def main():
    if not self_test():
        return
    w = build_screen()
    start_guard(w)
    g, sw5 = Guard(), Button(0)            # Button(0) = SW5 (ปุ่มล่าง)
    every = TICK_MS // SAMPLE_MS   # อัปเดตจอทุกกี่รอบอ่าน (TICK_MS ต้องไม่น้อยกว่า SAMPLE_MS)
    tick, t0 = 0, time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        on_arm(w, g, sw5)                                  # 0) ปุ่ม / สวิตช์บนจอ
        got = read_radar()                                 # 1) อ่าน
        on_radar(w, g, got is not None)
        if got is not None:
            raw, moving, energy = got
            cm = None if raw is None else smooth(g.hist, raw)
            alert_cm = zone_cm()
            near = decide(w, g, cm, alert_cm)          # 2) ตัดสิน + 3) ทำ
            if tick % every == 0:                          # 4) โชว์
                show_gate(w, g, near, cm, moving)
                show_zone(w, alert_cm, cm, energy)
        matrix_tick(g)
        tick += 1
        wait_ms(SAMPLE_MS, (sw5,))         # รอ แต่ยังคอยฟังปุ่ม
    finish(w, g.count)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ตั้ง CONFIRM_N = 1 แล้วเดินผ่านหน้าบอร์ดเร็ว ๆ 5 รอบ นับว่าตัวนับขึ้นกี่ครั้ง
#    เทียบกับ CONFIRM_N = 3 แบบไหน "เตือนผิด" น้อยกว่า แบบไหน "เตือนช้า" กว่า
# 2) นับผู้บุกรุกเฉพาะตอนที่เรดาร์บอกว่า "มีการเคลื่อนไหว" ด้วย (ส่ง moving เข้า in_zone())
#    แล้วลองยืนนิ่งหน้าบอร์ด - ระบบแบบไหนเหมาะกับคอกวัวตอนกลางคืน
# 3) ให้ SW6 (ปุ่มบน = Button(1)) ล้างตัวนับกลับเป็น 0 (อย่าลืมใส่ปุ่มใหม่ใน wait_ms ด้วย)
