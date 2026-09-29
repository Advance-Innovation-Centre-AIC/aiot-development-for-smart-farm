# cp2_01_filter_race.py - หลักการ 2.1: สัญญาณรบกวนมีหลายรูป เลือกตัวกรองให้ตรงรูป
#
# หลักการ  : ค่าจากเซนเซอร์มี "สั่นเล็ก ๆ" (jitter) กับ "กระโดดทีเดียว" (spike) ปนมา
#            EMA = เฉลี่ยแบบถ่วงน้ำหนัก ค่าใหม่ได้ alpha ส่วน ค่าเดิมได้ 1 - alpha
#                  เรียบขึ้นแต่ตามช้าลง และยังปล่อยค่ากระโดดผ่านมา alpha ส่วน
#            Median = ค่ากลางของ MED_WIN ตัวล่าสุด ค่ากระโดดตัวเดียวหายไปทั้งตัว
#            Kalman1D = ผสมค่าเดิมกับค่าใหม่ตาม q (ค่าจริงเปลี่ยนเร็วแค่ไหน) และ r (เซนเซอร์สั่นแค่ไหน)
#                  เมื่อ q, r คงที่ น้ำหนักของมันจะนิ่งที่ค่าหนึ่ง จึงทำตัวคล้าย EMA ที่เลือก alpha ให้เอง
# ลองเล่น  : หมุน VR1 = ค่าจริง ดูว่าเส้นไหนตามทันก่อน
#            หมุน VR2 = alpha ของ EMA (0.05-0.90) alpha มาก = ตามไวแต่ค่ากระโดดทะลุ (มีเสียงเตือน)
#            alpha น้อย = นิ่งแต่ตามช้า ดูแถว "ผ่าน" ทางขวา ว่าแต่ละเส้นปล่อยค่ากระโดดผ่านกี่ครั้ง
# ของบนบอร์ด: ลูกบิด VR1 VR2 · ลำโพง (เตือนเมื่อ EMA ปล่อยค่ากระโดดผ่าน)
#            สั่นเล็ก ๆ และค่ากระโดดเป็นของจำลองในโปรแกรม (random) ไม่ได้มาจากลูกบิด
#            ตัวกรองทั้งสามเป็นของเฟิร์มแวร์ (โมดูล dsp) ทำงานเป็นภาษา C
# ในฟาร์ม  : เซนเซอร์ความชื้นดินสายหลวม ส่งค่ากระโดดมาเป็นพัก ๆ ถ้าไม่กรอง ปั๊มจะเปิดผิดจังหวะ
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (dsp ใน Emulator ใช้สูตรเดียวกับเฟิร์มแวร์)

import dsp
import pots
import random
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
SPIKE_EVERY = 12     # ทุก ๆ กี่ตัวอย่างมีค่ากระโดดหนึ่งครั้ง
SPIKE = 45           # ค่ากระโดดสูงเท่าไร (%) กระโดดไปฝั่งที่ว่าง (ค่าจริงต่ำ = กระโดดขึ้น) จึงไม่ล้นกราฟ
NOISE = 3            # สั่นเล็ก ๆ บวกลบกี่ % (0 = ไม่สั่น)
SPIKE_PASS = 10      # ตอนมีค่ากระโดด เส้นไหนห่างค่าจริงเกินเท่านี้ (%) = "ปล่อยผ่าน"
MED_WIN = 5          # Median: ดูกี่ตัวล่าสุด (เฟิร์มแวร์รับ 3-15 และปัดเป็นเลขคี่)
KAL_Q = 0.1          # Kalman1D: ค่าจริงเปลี่ยนได้เร็วแค่ไหน (มาก = ตามไว)
KAL_R = 4.0          # Kalman1D: เซนเซอร์สั่นแค่ไหน (มาก = เชื่อค่าใหม่น้อย เส้นนิ่งกว่า)
TICK_MS = 500        # เก็บ 1 ตัวอย่างและอัปเดตจอทุกกี่ ms
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def clean_value():
    # VR1 = "ค่าจริง" 0-100 % (ยังไม่มีสัญญาณรบกวน)
    return pots.read(0) * 100 // 4095


def alpha_step(old):
    # VR2 -> ขั้น 1-18 = alpha 0.05-0.90 (ขั้นละ 0.05)
    # เปลี่ยนขั้นเมื่อหมุนห่างขั้นเดิมเกิน 0.7 ขั้น (ช่องกันแกว่ง: ลูกบิดค้างตรงรอยต่อจะไม่สลับไปมา)
    x = 1 + pots.read(1) * 17 / 4095
    return round(x) if abs(x - old) > 0.7 else old


def sensor(clean, spike):
    # "เซนเซอร์" จำลอง = ค่าจริง + สั่นเล็ก ๆ + ค่ากระโดด (เมื่อ spike เป็น True)
    v = clean + random.randint(-NOISE, NOISE)
    if spike:
        v += SPIKE if clean < 50 else -SPIKE
    return max(0, min(100, v))


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def make_ema(step, old):
    # dsp.EMA ไม่มีคำสั่งเปลี่ยน alpha จึงสร้างตัวใหม่เฉพาะตอน VR2 เปลี่ยนขั้น (ไม่ใช่ทุกรอบ)
    # EMA ของเฟิร์มแวร์ใช้ค่าแรกที่ป้อนเป็นจุดเริ่ม เราจึงป้อนค่าล่าสุดของตัวเก่าให้ก่อน เส้นจะไม่กระตุก
    # (เลือกใช้ dsp.EMA ไม่เขียน EMA เองใน Python เพราะไฟล์นี้อยากให้เห็นตัวกรองของเฟิร์มแวร์ครบทั้งสาม)
    f = dsp.EMA(alpha=step * 0.05)
    if old is not None:
        f.update(old.value())
    return f


def leaked(out, clean):
    # ตอนมีค่ากระโดด: เส้นนี้ห่างค่าจริงเกิน SPIKE_PASS ไหม (ห่างเกิน = ปล่อยค่ากระโดดผ่าน)
    return abs(out - clean) > SPIKE_PASS


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
NAMES = ("ดิบ", "EMA", "Median", "Kalman")
COLORS = (COL_DIM, COL_WARN, COL_OK, COL_INFO)   # สีเส้นในกราฟ = สีชื่อทางขวา


def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("แข่งกรอง: ดิบ EMA Median Kalman", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("ค่ากระโดดทุก %d ตัวอย่าง - เส้นไหนปล่อยผ่าน" % SPIKE_EVERY, x=12, y=38,
             color=COL_DIM, value=16)
    ch = line_chart(12, 66, 400, 272, 0, 100, COLORS[0])
    # กราฟเดียวมีได้ 4 เส้น (เส้น 0 มากับกราฟ + add_series อีก 3)
    w = {"ch": ch, "s": [0] + [ch.add_series(c) for c in COLORS[1:]]}
    card(424, 66, 356, 272, "ค่า  /  ปล่อยผ่านกี่ครั้ง")
    w["row"] = [ui.Label(n, x=436, y=100 + 46 * i, color=COLORS[i], value=20)
                for i, n in enumerate(NAMES)]
    ui.Label("ผ่าน = ห่างค่าจริงเกิน %d ตอนกระโดด" % SPIKE_PASS, x=436, y=296,
             color=COL_DIM, value=16)
    w["help"] = ui.Label("VR1 = ค่าจริง   VR2 = alpha ของ EMA", x=12, y=352,
                         color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, outs, got, spikes, step):
    for i in range(4):
        v = int(round(outs[i]))                            # กราฟรับเฉพาะจำนวนเต็ม (float = TypeError)
        w["ch"].set_next(w["s"][i], v)
        name = "EMA a=%.2f" % (step * 0.05) if i == 1 else NAMES[i]
        w["row"][i].text("%s  %d  ผ่าน %d/%d" % (name, v, got[i], spikes))


# ---- 6) โปรแกรมหลัก ----
def main():
    w = build_screen()
    med = dsp.Median(window=MED_WIN)                       # constructor ของ dsp รับแบบมีชื่อเท่านั้น
    kal = dsp.Kalman1D(q=KAL_Q, r=KAL_R)
    ema, step = None, 0
    got, spikes, n = [0, 0, 0, 0], 0, 0
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        s = alpha_step(step)
        if s != step:                                      # VR2 เปลี่ยนขั้น = สร้าง EMA ใหม่
            ema, step = make_ema(s, ema), s
        clean = clean_value()                              # 1) อ่าน
        spike = n % SPIKE_EVERY == SPIKE_EVERY - 1
        raw = sensor(clean, spike)
        outs = (raw, ema.update(raw), med.update(raw), kal.update(raw))   # 2) กรอง
        if spike:                                          # 3) นับว่าใครปล่อยผ่าน
            spikes += 1
            for i in range(4):
                if leaked(outs[i], clean):
                    got[i] += 1
            if leaked(outs[1], clean):
                beep("bad")                              # alert: EMA ปล่อยค่ากระโดดผ่าน
        show(w, outs, got, spikes, step)                   # 4) โชว์
        ui.poll()
        n += 1
        time.sleep_ms(TICK_MS)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: EMA alpha = 0.2 ค่านิ่งอยู่ที่ 40 แล้วมีค่ากระโดดไป 85 หนึ่งตัว เส้น EMA ขึ้นไปที่เท่าไร
#    และ Median หน้าต่าง 5 ตัว (ก่อนหน้านั้นเป็น 40 มาหลายตัวแล้ว) ได้เท่าไร
#    แล้วหมุน VR2 ให้ขึ้น a=0.20 ตั้ง NOISE = 0 และหมุน VR1 ไว้ที่ 40 เพื่อดูตัวเลขจริงบนจอ
# 2) ตั้ง SPIKE_EVERY = 2 (ค่ากระโดดมาถี่ทุกตัวเว้นตัว) Median ยังเอาอยู่ไหม ลอง MED_WIN = 3 กับ 9
# 3) ตั้ง KAL_R = 40 แล้วตั้ง KAL_R = 0.4 เส้นฟ้าตามช้าลงหรือเร็วขึ้น ปล่อยค่ากระโดดผ่านกี่ครั้ง
