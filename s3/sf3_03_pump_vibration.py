# sf3_03_pump_vibration.py - หมอฟังปั๊มน้ำ: เครื่องจักรสั่นผิดปกติ
#
# ภารกิจ   : วางบอร์ดบนปั๊มน้ำ/พัดลมโรงเรือน ให้มัน "เรียนรู้ค่าสั่นปกติ" 5 วินาทีแรก
#            จากนั้นให้คะแนนการสั่น 0-100 ถ้าสั่นแรงกว่าปกติหลายเท่า = ผิดปกติ (ลูกปืนเริ่มพัง?)
# ลองเล่น  : ช่วงเรียนรู้วางบอร์ดนิ่ง ๆ หลังจากนั้นเคาะโต๊ะเบา ๆ แล้วเขย่าบอร์ดแรง ๆ
#            RGB matrix โชว์จำนวนครั้งที่ผิดปกติ + แถวล่างคือแถบคะแนนการสั่น · กด SW4 = เรียนรู้ใหม่
# แนวคิด AIoT: นี่คือ anomaly detection แบบไม่ใช้ AI - จำ "ปกติ" แล้วจับสิ่งที่ต่างไปจากปกติ
#            ซ่อมก่อนพัง (predictive maintenance) ถูกกว่าปั๊มดับกลางฤดูแล้งเสมอ
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator (กดปุ่ม Shake = เครื่องสั่น)

import buttons
import math
import rgbmatrix
import sensors
import time
import ui

LEARN_MS = 5000      # เรียนรู้ค่าปกติกี่มิลลิวินาที
WIN = 20             # ค่าสั่นคิดจาก 20 ตัวอย่างล่าสุด (20 x 50 ms = 1 วินาที)
K_WARN, K_BAD = 3.0, 6.0   # สั่นกว่าปกติกี่เท่าถึง "เฝ้าระวัง" / "ผิดปกติ"
MIN_BASE = 0.05      # ค่าปกติต่ำสุด (m/s^2) กันบอร์ดที่นิ่งสนิทได้ค่าปกติเป็นศูนย์
CONFIRM_N = 10       # ระดับใหม่ต้องค้าง 10 ตัวอย่าง (0.5 วินาที) ถึงจะเชื่อ
RUN_MS = 120000
TICK_MS = 50
CHART_MAX = 300      # กราฟรับจำนวนเต็ม จึงคูณค่าสั่นด้วย 100

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
NAMES = ("ปกติ", "เฝ้าระวัง", "ผิดปกติ!")
COLORS = (COL_OK, COL_WARN, COL_BAD)
MX_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)


def magnitude():
    """ขนาดความเร่งรวมสามแกน (m/s^2) วางนิ่งได้ราว 9.8 คือแรงโน้มถ่วง"""
    try:
        ax, ay, az, _, _, _ = sensors.bmi270.motion()
    except OSError:
        return 9.8
    return math.sqrt(ax * ax + ay * ay + az * az)


def spread(xs):
    """ส่วนเบี่ยงเบนมาตรฐาน = ค่าสั่น ยิ่งแกว่งรอบค่าเฉลี่ยมาก ยิ่งสั่นมาก"""
    m = sum(xs) / len(xs)
    return math.sqrt(sum((x - m) * (x - m) for x in xs) / len(xs))


def show_matrix(count, level, step):
    """ตัวเลขครั้งที่ผิดปกติ + แถบล่าง วาดเฉพาะตอนค่าเปลี่ยน (ทุกคำสั่ง = เขียนบัสจอหนึ่งครั้ง)"""
    try:
        rgbmatrix.score(count, MX_COLORS[level])
        rgbmatrix.bar(step, 8, MX_COLORS[level])
    except OSError:
        pass


ui.screen()
time.sleep_ms(200)
ui.Label("หมอฟังปั๊มน้ำ", x=20, y=12, color=COL_TEXT, value=24)
status = ui.Label("", x=20, y=48, color=COL_WARN, value=18)

ui.Panel(x=20, y=84, w=360, h=220, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("คะแนนการสั่น (0-100)", x=36, y=92, color=COL_DIM, value=16)
arc = ui.Arc(x=40, y=120, w=150, h=150, min=0, max=100, value=0)
arc.color(COL_OK)
score_lbl = ui.Label("0", x=210, y=124, color=COL_TEXT, value=40)
verdict = ui.Label("...", x=210, y=200, color=COL_DIM, value=28)

ui.Panel(x=400, y=84, w=380, h=220, color=COL_CARD, min=COL_DIM, max=12, value=1)
lbl_vib = ui.Label("ค่าสั่นตอนนี้ -", x=416, y=96, color=COL_TEXT, value=20)
lbl_base = ui.Label("ค่าปกติที่เรียนมา -", x=416, y=132, color=COL_INFO, value=18)
ui.Label("ผิดปกติ (ครั้ง)", x=416, y=176, color=COL_DIM, value=16)
seg_bad = ui.Seg7(text="0", x=416, y=202, w=120, h=56, color=COL_BAD)
lbl_time = ui.Label("รวม 0.0 วิ", x=560, y=210, color=COL_DIM, value=18)
ui.Label("SW4 = เรียนรู้ใหม่", x=560, y=250, color=COL_DIM, value=16)

ch = ui.Chart(x=20, y=316, w=660, h=72, min=0, max=CHART_MAX)   # ขวาล่างเว้นให้ปุ่ม Console
s_warn = ch.add_series(COL_WARN)
s_bad = ch.add_series(COL_BAD)
ui.poll()

buf = []
learned = []
base = None
level = cand = streak = bad_count = bad_ms = n = 0
sw_was = False
shown = None
t_learn = last_t = time.ticks_ms()
t0 = t_learn
while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
    buf.append(magnitude())
    if len(buf) > WIN:
        buf.pop(0)
    n += 1
    now = time.ticks_ms()
    dt, last_t = time.ticks_diff(now, last_t), now

    sw = buttons.pressed(0)                     # SW4: จับ "ขอบกด" เอง กดค้างไม่นับซ้ำ
    if sw and not sw_was:
        base, learned, t_learn, level, cand = None, [], now, 0, 0
        ui.sfx(ui.SFX_UI_START)
    sw_was = sw
    if len(buf) < WIN:
        ui.poll()
        time.sleep_ms(TICK_MS)
        continue
    vib = spread(buf)

    if base is None:                            # ช่วงเรียนรู้ค่าปกติ
        learned.append(vib)
        if time.ticks_diff(now, t_learn) >= LEARN_MS:
            base = max(MIN_BASE, sum(learned) / len(learned))
            lbl_base.text("ค่าปกติที่เรียนมา %.3f" % base)
            status.color(COL_OK)
            status.text("เฝ้าเครื่องแล้ว ลองเคาะหรือเขย่าบอร์ด")
            ui.sfx(ui.SFX_UI_SELECT)
        elif n % 4 == 0:
            left_s = (LEARN_MS - time.ticks_diff(now, t_learn)) // 1000 + 1
            status.color(COL_WARN)
            status.text("วางบอร์ดนิ่ง ๆ เรียนรู้ค่าปกติ อีก %d วิ" % left_s)
    else:
        new = 2 if vib > base * K_BAD else (1 if vib > base * K_WARN else 0)
        streak = streak + 1 if new == cand else 1
        cand = new
        if cand != level and streak >= CONFIRM_N:
            if cand == 2:
                bad_count += 1
                seg_bad.text(str(bad_count))
                ui.sfx(ui.SFX_UI_DENY)
                print("ผิดปกติครั้งที่", bad_count, "ค่าสั่น %.3f" % vib)
            elif level == 2:
                ui.sfx(ui.SFX_PONG_WIN)         # กลับมาเป็นปกติ/เฝ้าระวังแล้ว
            level = cand
        if level == 2:
            bad_ms += dt                        # เวลาจริงที่ผ่านไป ไม่ใช่ TICK_MS

    if n % 4 == 0 and base is not None:        # วาดจอทุก 200 ms พอ อ่านเซนเซอร์ทุก 50 ms
        score = min(100, int(vib * 100 / (base * K_BAD)))
        arc.value(score)
        arc.color(COLORS[level])
        score_lbl.text(str(score))
        verdict.color(COLORS[level])
        verdict.text(NAMES[level])
        lbl_vib.text("ค่าสั่นตอนนี้ %.3f" % vib)
        lbl_time.text("รวม %.1f วิ" % (bad_ms / 1000))
        ch.set_next(0, min(CHART_MAX, int(vib * 100)))
        ch.set_next(s_warn, min(CHART_MAX, int(base * K_WARN * 100)))
        ch.set_next(s_bad, min(CHART_MAX, int(base * K_BAD * 100)))
        key = (bad_count, level, score * 8 // 100)
        if key != shown:
            shown = key
            show_matrix(*key)
    ui.poll()                                   # poll ทุกลูป แม้รอบที่ไม่ได้วาดจอ
    time.sleep_ms(TICK_MS)

try:
    rgbmatrix.clear()
except OSError:
    pass
print("เฝ้าปั๊มครบเวลา ผิดปกติ", bad_count, "ครั้ง รวม %.1f วินาที" % (bad_ms / 1000))

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) กด SW4 แล้วให้เพื่อนเคาะโต๊ะเบา ๆ ตลอด 5 วินาที (สอน "ปกติ" ผิด ๆ) แล้วดูว่า
#    หลังจากนั้นเขย่าแรงแค่ไหนถึงจะเตือน - ข้อมูลตอนสอนสำคัญกับทั้งกฎและ AI
# 2) เพิ่มกฎ "ผิดปกติรวมเกิน 10 วินาที = สั่งหยุดปั๊ม" แล้วโชว์ข้อความตัวใหญ่บนจอ
