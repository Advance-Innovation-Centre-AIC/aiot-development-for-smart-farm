# sf3_03_pump_vibration.py - หมอฟังปั๊มน้ำ: เครื่องจักรสั่นผิดปกติ
#
# ภารกิจ   : วางบอร์ดบนปั๊มน้ำ/พัดลมโรงเรือน ให้มัน "เรียนรู้ค่าสั่นปกติ" 5 วินาทีแรก
#            จากนั้นให้คะแนนการสั่น 0-100 ถ้าสั่นแรงกว่าปกติหลายเท่า = ผิดปกติ (ลูกปืนเริ่มพัง?)
# ลองเล่น  : ช่วงเรียนรู้วางบอร์ดนิ่ง ๆ หลังจากนั้นเคาะโต๊ะเบา ๆ แล้วเขย่าบอร์ดแรง ๆ
#            ดูวงแหวนคะแนนกับกราฟ: เส้นฟ้าข้ามเส้นส้ม = เฝ้าระวัง, ข้ามเส้นแดง = ผิดปกติ
#            กด SW5 (ปุ่มล่าง) หนึ่งครั้ง = เรียนรู้ค่าปกติใหม่
# ของบนบอร์ดที่ใช้ : IMU BMI270 (ความเร่ง 3 แกน), ปุ่ม SW5 (ปุ่มล่าง) = เรียนรู้ใหม่
#            จอไฟ RGB: ตัวเลข = จำนวนครั้งที่ผิดปกติ, แถวล่าง = แถบคะแนนการสั่น (สีตามระดับ)
#            ลำโพงดังเฉพาะตอนกด SW5, ตอนเรียนรู้เสร็จ, ตอนเริ่มผิดปกติ และตอนหายผิดปกติ
# บนจอ     : วงแหวนคะแนน (Arc), ไฟระดับ 3 ดวง (Led), แถบความคืบหน้าการเรียนรู้ (Bar),
#            ตัวเลขนับครั้งผิดปกติ (Seg7), กราฟค่าสั่นเทียบเส้นเตือนสองเส้น (Chart)
# แนวคิด AIoT: นี่คือ anomaly detection แบบไม่ใช้ AI - จำ "ปกติ" แล้วจับสิ่งที่ต่างไปจากปกติ
#            ซ่อมก่อนพัง (predictive maintenance) ถูกกว่าปั๊มดับกลางฤดูแล้งเสมอ
#            เราใช้ "ขนาด" ของความเร่งรวมสามแกน จึงไม่สนว่าบอร์ดวางเอียงแค่ไหน สนแค่ว่ามันแกว่งแค่ไหน
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.2 ขึ้นไป · 2.4.1 ก็รันได้) และ BENTO Emulator (กดปุ่ม Shake = เครื่องสั่น)

import buttons
import math
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
VOLUME = 25          # ความดังเสียง 0-127 (≈20%)
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
LEARN_MS = 5000      # เรียนรู้ค่าปกติกี่มิลลิวินาที
SAMPLE_MS = 50       # อ่าน IMU ทุก 50 ms (การสั่นเร็ว อ่านห่างกว่านี้จะมองไม่เห็นการแกว่ง)
WIN = 20             # ค่าสั่นคิดจาก 20 ตัวอย่างล่าสุด (20 x 50 ms = 1 วินาที)
K_WARN, K_BAD = 3.0, 6.0   # สั่นกว่าปกติกี่เท่าถึง "เฝ้าระวัง" / "ผิดปกติ"
MIN_BASE = 0.05      # ค่าปกติต่ำสุด (m/s^2) กันบอร์ดที่นิ่งสนิทได้ค่าปกติเป็นศูนย์
CONFIRM_N = 10       # ระดับใหม่ต้องค้าง 10 ตัวอย่าง (0.5 วินาที) ถึงจะเชื่อ
RUN_MS = 120000
TICK_MS = 500        # วาดจอทุก 500 ms (อ่านเซนเซอร์ถี่กว่านั้น แต่จอไม่ต้องถี่ตาม)
MATRIX_MS = 3000     # ส่งภาพจอไฟ RGB ซ้ำทุก 3 วินาที เผื่อภาพก่อนหน้าหล่นหายระหว่างทาง
CHART_MAX = 300      # กราฟรับจำนวนเต็ม จึงคูณค่าสั่นด้วย 100
BTN_NAMES = ("SW5", "SW6")   # ชื่อที่พิมพ์บนบอร์ด: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
NAMES = ("ปกติ", "เฝ้าระวัง", "ผิดปกติ!")
COLORS = (COL_OK, COL_WARN, COL_BAD)
MX_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    # เสียงเบา ๆ แทน ui.sfx (ui.sfx ดังคงที่ ปรับเบาไม่ได้) · เล่นโน้ต MIDI ทีละตัว ห่างกัน 120 ms
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
    # ปุ่มบนฐานบอร์ด (0 = SW5 ปุ่มล่าง, 1 = SW6 ปุ่มบน) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสัญญาณสั่น 50 ms ถ้าอ่านรอบละครั้งการกดแบบแตะจะหายไป จึงอ่านบ่อย ๆ ใน wait_ms

    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        # True ครั้งเดียวต่อการกดหนึ่งครั้ง (กดค้างไว้ก็ไม่นับซ้ำ)
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    # รอ ms มิลลิวินาที แต่ระหว่างรอก็อ่านปุ่มทุก 20 ms เพื่อไม่พลาดการกดสั้น ๆ
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


def draw_matrix(key):
    # จอไฟ RGB: ตัวเลข = ครั้งที่ผิดปกติ, แถวล่าง = แถบคะแนน 0-8 ช่อง (ทุกคำสั่ง = เขียนบัสจอหนึ่งครั้ง)
    # กันพังแยกทีละคำสั่ง: คำสั่งแรกตอบไม่ทัน คำสั่งที่สองยังได้ไป และโปรแกรมไม่หยุด
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
    # ส่วนเบี่ยงเบนมาตรฐาน = ค่าสั่น ยิ่งแกว่งรอบค่าเฉลี่ยมาก ยิ่งสั่นมาก
    m = sum(xs) / len(xs)
    return math.sqrt(sum((x - m) * (x - m) for x in xs) / len(xs))


def classify(vib, base):
    # 0 = ปกติ, 1 = เฝ้าระวัง (เกิน K_WARN เท่าของปกติ), 2 = ผิดปกติ (เกิน K_BAD เท่า)
    return 2 if vib > base * K_BAD else (1 if vib > base * K_WARN else 0)


class Watch:
    # ความจำของหมอฟังปั๊ม: ตัวอย่างล่าสุด ค่าปกติ ระดับ และตัวนับ (ไม่แตะฮาร์ดแวร์ ไม่แตะจอ)

    def __init__(self, now):
        self.buf, self.vib, self.dt, self.t_last = [], 0.0, 0, now
        self.bad_count, self.bad_ms = 0, 0
        self.relearn(now)

    def relearn(self, now):
        # ลืมค่าปกติเดิมแล้วเริ่มเรียนใหม่ (ตัวนับครั้งผิดปกติยังเก็บไว้)
        self.base, self.total, self.n, self.t_learn = None, 0.0, 0, now
        self.level = self.cand = self.streak = 0

    def push(self, m, now):
        # เก็บตัวอย่างล่าสุด WIN ตัว คืน True เมื่อครบแล้วและได้ค่าสั่นใหม่ใน self.vib
        self.dt, self.t_last = time.ticks_diff(now, self.t_last), now
        self.buf.append(m)
        if len(self.buf) > WIN:
            self.buf.pop(0)
        if len(self.buf) < WIN:
            return False
        self.vib = spread(self.buf)
        return True

    def learn(self, now):
        # ช่วงเรียนรู้: สะสมค่าสั่นไว้ ครบ LEARN_MS แล้วเฉลี่ยเป็น "ค่าปกติ" (คืน True ครั้งเดียวตอนครบ)
        self.total += self.vib
        self.n += 1
        if time.ticks_diff(now, self.t_learn) < LEARN_MS:
            return False
        self.base = max(MIN_BASE, self.total / max(1, self.n))
        return True

    def judge(self):
        # ระดับใหม่ต้องค้าง CONFIRM_N ตัวอย่างติดกันถึงจะเชื่อ (เคาะทีเดียวไม่ถือว่าเครื่องเสีย)
        # คืนระดับก่อนหน้า ให้ผู้เรียกรู้ว่าระดับเพิ่งเปลี่ยนไหม
        new, old = classify(self.vib, self.base), self.level
        self.streak = self.streak + 1 if new == self.cand else 1
        self.cand = new
        if new != old and self.streak >= CONFIRM_N:
            self.level = new
            if new == 2:
                self.bad_count += 1
        if self.level == 2:
            self.bad_ms += self.dt      # เวลาจริงที่ผ่านไป ไม่ใช่ SAMPLE_MS
        return old


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
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
    for i in range(3):          # ไฟระดับ 3 ดวง ติดทีละดวงเหมือนแผงควบคุมจริง
        x = 180 + i * 66
        w["leds"].append(ui.Led(x=x, y=196, w=20, h=20, color=COLORS[i], value=0))
        ui.Label(("ปกติ", "ระวัง", "ผิดปกติ")[i], x=x + 26, y=198, color=COL_DIM, value=14)


def build_vib_card(w):
    card(402, 64, 378, 190, "ค่าสั่น (m/s^2)")
    w["vib"] = ui.Label("ตอนนี้ -", x=414, y=92, color=COL_TEXT, value=20)
    w["base"] = ui.Label("ปกติที่เรียนมา -", x=414, y=120, color=COL_INFO, value=16)
    w["learn"] = ui.Bar(x=414, y=146, w=354, h=12, min=0, max=100, value=0)
    w["learn"].color(COL_WARN)      # แถบเรียนรู้: ส้ม = กำลังเรียน, เขียว = ได้ค่าปกติแล้ว
    ui.Label("ผิดปกติ (ครั้ง)", x=414, y=166, color=COL_DIM, value=14)
    w["seg"] = ui.Seg7(text="0", x=414, y=186, w=110, h=58, color=COL_BAD)
    w["time"] = ui.Label("รวม 0.0 วิ", x=540, y=204, color=COL_DIM, value=20)


def build_chart(w):
    # กราฟ: ฟ้า (ชุด 0) = ค่าสั่นตอนนี้, ส้ม = เส้นเฝ้าระวัง, แดง = เส้นผิดปกติ
    w["chart"] = line_chart(12, 262, 400, 76, 0, CHART_MAX, COL_INFO)
    w["s_warn"] = w["chart"].add_series(COL_WARN)
    w["s_bad"] = w["chart"].add_series(COL_BAD)
    ui.Label("ฟ้า = ค่าสั่น x100", x=424, y=266, color=COL_INFO, value=14)
    ui.Label("ส้ม = เส้นเฝ้าระวัง", x=424, y=290, color=COL_WARN, value=14)
    ui.Label("แดง = เส้นผิดปกติ", x=424, y=314, color=COL_BAD, value=14)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
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
    # ช่วงเรียนรู้: นับถอยหลัง + แถบความคืบหน้า
    left = max(0, LEARN_MS - time.ticks_diff(now, st.t_learn))
    show_note(w, "วางบอร์ดนิ่ง ๆ เรียนรู้ค่าปกติ อีก %d วิ" % (left // 1000 + 1), COL_WARN)
    w["learn"].value(100 - left * 100 // LEARN_MS)


def show_learned(w, st):
    # เรียกครั้งเดียวต่อการเรียนรู้หนึ่งรอบ ตอนได้ค่าปกติ
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
        w["leds"][i].value(1 if i == st.level else 0)    # 0 = หรี่ (ไม่ดับมืด)
    w["vib"].text("ตอนนี้ %.3f" % st.vib)
    w["seg"].text(str(st.bad_count))
    w["time"].text("รวม %.1f วิ" % (st.bad_ms / 1000))
    ch = w["chart"]
    ch.set_next(0, min(CHART_MAX, int(st.vib * 100)))
    ch.set_next(w["s_warn"], min(CHART_MAX, int(st.base * K_WARN * 100)))
    ch.set_next(w["s_bad"], min(CHART_MAX, int(st.base * K_BAD * 100)))


# ---- 5) โปรแกรมหลัก ----
def think(w, st, now):
    # ได้ค่าสั่นใหม่: ช่วงเรียนรู้ = เก็บค่าปกติ, หลังจากนั้น = จัดระดับ
    # เสียงดังเฉพาะตอนเหตุการณ์เกิด (ระดับเปลี่ยน) ไม่ใช่ทุกรอบ
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
        beep(79, 84)                    # หายผิดปกติแล้ว


def refresh(w, st, now, mx, fails):
    # วาดจอ (ทุก TICK_MS) แล้วส่งภาพจอไฟ RGB เมื่อภาพเปลี่ยน หรือครบ MATRIX_MS
    # mx = [ภาพที่ส่งล่าสุด, เวลาที่ส่ง]
    score = 0
    if fails >= WIN:
        show_note(w, "อ่าน IMU ไม่ได้", COL_BAD)
    elif st.base is None:
        show_learning(w, st, now)
    else:
        score = min(100, int(st.vib * 100 / (st.base * K_BAD)))   # 100 = สั่นถึงเส้น "ผิดปกติ" แล้ว
        show_watch(w, st, score)
        show_note(w, "เฝ้าเครื่องแล้ว ลองเคาะหรือเขย่าบอร์ด", COL_OK)
    ui.poll()
    key = (st.bad_count, st.level, score * 8 // 100)
    if key != mx[0] or time.ticks_diff(now, mx[1]) >= MATRIX_MS:
        draw_matrix(key)
        mx[0], mx[1] = key, now


def finish(w, st):
    # จบรอบ: ล้างจอไฟ RGB บอกวิธีเล่นใหม่ และพิมพ์สรุปลง Console
    try:
        rgbmatrix.clear()
    except OSError:
        pass
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()
    print("เฝ้าปั๊มครบเวลา ผิดปกติ", st.bad_count, "ครั้ง รวม %.1f วินาที" % (st.bad_ms / 1000))


def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    sw5 = Button(0)                     # SW5 (ปุ่มล่าง) = เรียนรู้ค่าปกติใหม่
    t0 = t_draw = time.ticks_ms()
    st, mx, fails = Watch(t0), [None, t0], 0
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        m = magnitude()                                 # 1) อ่าน
        fails = 0 if m is not None else fails + 1
        if m is not None and st.push(m, now):
            think(w, st, now)                           # 2) ตัดสิน
        if sw5.pressed_now():
            st.relearn(now)
            w["learn"].color(COL_WARN)
            beep(72, 79)
        if time.ticks_diff(now, t_draw) >= TICK_MS:     # 3) โชว์ ทุก TICK_MS
            t_draw = now
            refresh(w, st, now, mx, fails)
        wait_ms(SAMPLE_MS, (sw5,))                      # รอ แต่ยังคอยฟังปุ่ม

    finish(w, st)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) กด SW5 (ปุ่มล่าง) แล้วให้เพื่อนเคาะโต๊ะเบา ๆ ตลอด 5 วินาที (สอน "ปกติ" ผิด ๆ) แล้วดูว่า
#    หลังจากนั้นเขย่าแรงแค่ไหนถึงจะเตือน - ข้อมูลตอนสอนสำคัญกับทั้งกฎและ AI
# 2) เพิ่มกฎ "ผิดปกติรวมเกิน 10 วินาที = สั่งหยุดปั๊ม" เขียนเป็นฟังก์ชันในส่วน 3) ที่รับ st.bad_ms
#    แล้วโชว์ข้อความตัวใหญ่บนจอด้วย show_note()
# 3) ให้ SW6 (ปุ่มบน) = Button(1) ล้างตัวนับครั้งผิดปกติ (st.bad_count และ st.bad_ms)
#    อย่าลืมใส่ปุ่มใหม่ลงใน wait_ms(SAMPLE_MS, (sw5, sw6)) ด้วย ไม่งั้นการกดสั้น ๆ จะหาย
