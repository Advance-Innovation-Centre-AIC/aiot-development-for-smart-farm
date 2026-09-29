# sf3_03_pump_vibration.py - หมอฟังปั๊มน้ำ: เครื่องจักรสั่นผิดปกติ
# ภารกิจ   : เรียนรู้ค่าสั่นปกติ 5 วิแรก แล้วให้คะแนนการสั่น 0-100 (สั่นกว่าปกติหลายเท่า = ผิดปกติ)
# ลองเล่น  : วางนิ่งช่วงเรียนรู้ แล้วเคาะโต๊ะ / เขย่าแรง ๆ · SW5 (ปุ่มล่าง) = เรียนรู้ใหม่
# ของบนบอร์ดที่ใช้ : IMU BMI270, ปุ่ม SW5, ลำโพง, จอไฟ RGB (เลข = ครั้งผิดปกติ)
# บนจอ     : วงแหวนคะแนน, ไฟระดับ, แถบเรียนรู้, ตัวนับ, กราฟเทียบเส้นเตือน
# แนวคิด AIoT: anomaly detection ไม่ใช้ AI - จำ "ปกติ" แล้วจับสิ่งที่ต่างไป (ซ่อมก่อนพัง)
# บอร์ด     : TESAIoT Dev Kit (fw 2.4.2+ · 2.4.1 ก็รันได้) / BENTO Emulator (ปุ่ม Shake)

import buttons
import math
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
VOLUME = 25          # เสียงเตือน 0-127
SPEAKER = 40         # ลำโพงรวม 0-100% (fw 2.4.2+)
LEARN_MS = 5000      # เวลาเรียนรู้ (ms)
SAMPLE_MS = 50       # อ่าน IMU ทุกกี่ ms
WIN = 20             # คิดค่าสั่นจากกี่ตัวอย่างล่าสุด
K_WARN, K_BAD = 3.0, 6.0   # กี่เท่าของปกติ = เฝ้าระวัง / ผิดปกติ
MIN_BASE = 0.05      # ค่าปกติต่ำสุด (m/s^2)
CONFIRM_N = 10       # ค้างกี่ตัวอย่างถึงเชื่อ
RUN_MS = 120000      # เวลาเล่น (ms)
TICK_MS = 500        # วาดจอทุกกี่ ms
MATRIX_MS = 3000     # ส่งจอไฟ RGB ซ้ำทุกกี่ ms
CHART_MAX = 300      # เพดานกราฟ (ค่าสั่น x100)
BTN_NAMES = ("SW5", "SW6")   # ล่าง, บน

# สี (0xRRGGBB) ชื่อ และสีของระดับ 0/1/2
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
NAMES = ("ปกติ", "เฝ้าระวัง", "ผิดปกติ!")
COLORS = (COL_OK, COL_WARN, COL_BAD)
MX_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def magnitude():
    # ขนาดความเร่งรวมสามแกน (m/s^2) วางนิ่งได้ราว 9.8 คือแรงโน้มถ่วง
    # ขนาดไม่เปลี่ยนตามมุมเอียง บอร์ดวางเอียงก็วัดการสั่นได้เหมือนวางราบ
    # อ่านไม่ได้ (บัสไม่ว่าง) คืน None แล้วข้ามตัวอย่างนั้นไป ไม่เดาค่าแทน
    try:
        ax, ay, az, _, _, _ = sensors.bmi270.motion()
    except OSError:
        return None
    return math.sqrt(ax * ax + ay * ay + az * az)


class Button:
    # 0 = SW5, 1 = SW6 · อ่านใน wait_ms จึงไม่พลาดการกดสั้น ๆ
    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        # True ครั้งเดียวต่อการกด
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


def draw_matrix(key):
    count, level, step = key
    try:
        rgbmatrix.score(count, MX_COLORS[level])
    except OSError:
        pass
    try:
        rgbmatrix.bar(step, 8, MX_COLORS[level])
    except OSError:
        pass


# ---- 3) สมอง (ตัดสินใจ) ----
def spread(xs):
    # ส่วนเบี่ยงเบนมาตรฐาน = ค่าสั่น
    m = sum(xs) / len(xs)
    return math.sqrt(sum((x - m) * (x - m) for x in xs) / len(xs))


def classify(vib, base):
    # 0 = ปกติ, 1 = เฝ้าระวัง (เกิน K_WARN เท่าของปกติ), 2 = ผิดปกติ (เกิน K_BAD เท่า)
    return 2 if vib > base * K_BAD else (1 if vib > base * K_WARN else 0)


class Watch:
    # ความจำ ไม่แตะฮาร์ดแวร์ ไม่แตะจอ
    def __init__(self, now):
        self.buf, self.vib, self.dt, self.t_last = [], 0.0, 0, now
        self.bad_count, self.bad_ms = 0, 0
        self.relearn(now)

    def relearn(self, now):
        self.base, self.total, self.n, self.t_learn = None, 0.0, 0, now
        self.level = self.cand = self.streak = 0

    def push(self, m, now):
        # True เมื่อครบ WIN ตัว และได้ self.vib ใหม่
        self.dt, self.t_last = time.ticks_diff(now, self.t_last), now
        self.buf.append(m)
        if len(self.buf) > WIN:
            self.buf.pop(0)
        if len(self.buf) < WIN:
            return False
        self.vib = spread(self.buf)
        return True

    def learn(self, now):
        # ครบ LEARN_MS = ได้ค่าปกติ (True ครั้งเดียว)
        self.total += self.vib
        self.n += 1
        if time.ticks_diff(now, self.t_learn) < LEARN_MS:
            return False
        self.base = max(MIN_BASE, self.total / max(1, self.n))
        return True

    def judge(self):
        # ระดับใหม่ต้องค้าง CONFIRM_N ตัวอย่างติดกันถึงจะเชื่อ (เคาะทีเดียวไม่ถือว่าเครื่องเสีย)
        # คืนระดับก่อนหน้า
        new, old = classify(self.vib, self.base), self.level
        self.streak = self.streak + 1 if new == self.cand else 1
        self.cand = new
        if new != old and self.streak >= CONFIRM_N:
            self.level = new
            if new == 2:
                self.bad_count += 1
        if self.level == 2:
            self.bad_ms += self.dt  # เวลาจริง ไม่ใช่ SAMPLE_MS
        return old


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # จุด >= ความกว้าง: LVGL ไม่วาดจุดกลม
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_score_card(w):
    card(12, 64, 380, 190, "คะแนนการสั่น (0-100)")
    w["arc"] = ui.Arc(x=24, y=92, w=140, h=140, min=0, max=100, value=0)
    w["arc"].color(COL_OK)
    w["score"] = ui.Label("0", x=180, y=94, color=COL_TEXT, value=28)
    w["verdict"] = ui.Label("...", x=180, y=134, color=COL_DIM, value=28)
    w["leds"] = []
    for i in range(3):
        x = 180 + i * 66
        w["leds"].append(ui.Led(x=x, y=196, w=20, h=20, color=COLORS[i], value=0))
        ui.Label(("ปกติ", "ระวัง", "ผิดปกติ")[i], x=x + 26, y=198, color=COL_DIM, value=14)


def build_vib_card(w):
    card(402, 64, 378, 190, "ค่าสั่น (m/s^2)")
    w["vib"] = ui.Label("ตอนนี้ -", x=414, y=92, color=COL_TEXT, value=20)
    w["base"] = ui.Label("ปกติที่เรียนมา -", x=414, y=120, color=COL_INFO, value=16)
    w["learn"] = ui.Bar(x=414, y=146, w=354, h=12, min=0, max=100, value=0)
    w["learn"].color(COL_WARN)
    ui.Label("ผิดปกติ (ครั้ง)", x=414, y=166, color=COL_DIM, value=14)
    w["seg"] = ui.Seg7(text="0", x=414, y=186, w=110, h=58, color=COL_BAD)
    w["time"] = ui.Label("รวม 0.0 วิ", x=540, y=204, color=COL_DIM, value=20)


def build_chart(w):
    w["chart"] = line_chart(12, 262, 400, 76, 0, CHART_MAX, COL_INFO)
    w["s_warn"] = w["chart"].add_series(COL_WARN)
    w["s_bad"] = w["chart"].add_series(COL_BAD)
    ui.Label("ฟ้า = ค่าสั่น x100", x=424, y=266, color=COL_INFO, value=14)
    ui.Label("ส้ม = เส้นเฝ้าระวัง", x=424, y=290, color=COL_WARN, value=14)
    ui.Label("แดง = เส้นผิดปกติ", x=424, y=314, color=COL_BAD, value=14)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("หมอฟังปั๊มน้ำ", x=12, y=6, color=COL_TEXT, value=24)
    w = {"status": ui.Label(" ", x=12, y=38, color=COL_WARN, value=16)}
    build_score_card(w)
    build_vib_card(w)
    build_chart(w)
    w["help"] = ui.Label(BTN_NAMES[0] + " (ปุ่มล่าง) = เรียนรู้ค่าปกติใหม่", x=12, y=352,
                         color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    w["status"].color(col)
    w["status"].text(text)


def show_learning(w, st, now):
    left = max(0, LEARN_MS - time.ticks_diff(now, st.t_learn))
    show_note(w, "วางบอร์ดนิ่ง ๆ เรียนรู้ค่าปกติ อีก %d วิ" % (left // 1000 + 1), COL_WARN)
    w["learn"].value(100 - left * 100 // LEARN_MS)


def show_learned(w, st):
    w["base"].text("ปกติที่เรียนมา %.3f" % st.base)
    w["learn"].value(100)
    w["learn"].color(COL_OK)


def show_watch(w, st, score):
    col = COLORS[st.level]
    w["arc"].value(score)
    w["arc"].color(col)
    w["score"].text(str(score))
    w["verdict"].color(col)
    w["verdict"].text(NAMES[st.level])
    for i in range(3):
        w["leds"][i].value(1 if i == st.level else 0)  # 0 = หรี่
    w["vib"].text("ตอนนี้ %.3f" % st.vib)
    w["seg"].text(str(st.bad_count))
    w["time"].text("รวม %.1f วิ" % (st.bad_ms / 1000))
    ch = w["chart"]
    ch.set_next(0, min(CHART_MAX, int(st.vib * 100)))
    ch.set_next(w["s_warn"], min(CHART_MAX, int(st.base * K_WARN * 100)))
    ch.set_next(w["s_bad"], min(CHART_MAX, int(st.base * K_BAD * 100)))


# ---- 5) โปรแกรมหลัก ----
def think(w, st, now):
    # ดังเฉพาะตอนระดับเปลี่ยน
    if st.base is None:
        if st.learn(now):
            show_learned(w, st)
            beep(76)
        return
    old = st.judge()
    if st.level == old:
        return
    if st.level == 2:
        beep(84, 76)
        print("ผิดปกติครั้งที่", st.bad_count, "ค่าสั่น %.3f" % st.vib)
    elif old == 2:
        beep(79, 84)  # หายผิดปกติ


def refresh(w, st, now, mx, fails):
    # mx = [ภาพจอไฟ RGB ล่าสุด, เวลาที่ส่ง]
    score = 0
    if fails >= WIN:
        show_note(w, "อ่าน IMU ไม่ได้", COL_BAD)
    elif st.base is None:
        show_learning(w, st, now)
    else:
        score = min(100, int(st.vib * 100 / (st.base * K_BAD)))  # 100 = ถึงเส้นผิดปกติ
        show_watch(w, st, score)
        show_note(w, "เฝ้าเครื่องแล้ว ลองเคาะหรือเขย่าบอร์ด", COL_OK)
    ui.poll()
    key = (st.bad_count, st.level, score * 8 // 100)
    if key != mx[0] or time.ticks_diff(now, mx[1]) >= MATRIX_MS:
        draw_matrix(key)
        mx[0], mx[1] = key, now


def finish(w, st):
    try:
        rgbmatrix.clear()
    except OSError:
        pass
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()
    print("เฝ้าปั๊มครบเวลา ผิดปกติ", st.bad_count, "ครั้ง รวม %.1f วินาที" % (st.bad_ms / 1000))


def main():
    if hasattr(ui, "volume"):  # 2.4.1 ไม่มี
        ui.volume(SPEAKER)
    w = build_screen()
    sw5 = Button(0)
    t0 = t_draw = time.ticks_ms()
    st, mx, fails = Watch(t0), [None, t0], 0
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            now = time.ticks_ms()
            m = magnitude()  # 1) อ่าน
            fails = 0 if m is not None else fails + 1
            if m is not None and st.push(m, now):
                think(w, st, now)  # 2) ตัดสิน
            if sw5.pressed_now():
                st.relearn(now)
                w["learn"].color(COL_WARN)
                beep(72, 79)
            if time.ticks_diff(now, t_draw) >= TICK_MS:  # 3) โชว์
                t_draw = now
                refresh(w, st, now, mx, fails)
            wait_ms(SAMPLE_MS, (sw5,))
    finally:  # กด Stop ก็ล้างจอไฟ RGB และสรุป
        finish(w, st)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) กด SW5 ขณะเพื่อนเคาะโต๊ะ 5 วิ (สอนปกติผิด ๆ) ต้องเขย่าแรงแค่ไหนถึงเตือน?
# 2) กฎ "ผิดปกติรวมเกิน 10 วิ = หยุดปั๊ม" ในส่วน 3) ใช้ st.bad_ms โชว์ด้วย show_note()
# 3) SW6 = Button(1) ล้าง st.bad_count, st.bad_ms · ใส่ใน wait_ms(SAMPLE_MS, (sw5, sw6))
