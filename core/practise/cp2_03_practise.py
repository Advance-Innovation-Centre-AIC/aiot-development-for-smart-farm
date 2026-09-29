# cp2_03_practise.py - แบบฝึกเติมโค้ด (Code Quest ระดับ 3): เครื่องสถานะของเตือน
#
# วิธีเล่น  : ไฟล์นี้เหมือน cp2_03_decision_ladder.py ทุกอย่าง ยกเว้นฟังก์ชัน next_state()
#            ในส่วน "3) สมอง" ที่เว้นช่อง ____ (ขีดล่างสี่ตัว) ไว้ 3 ช่อง: A, B, C
#            เติมให้ครบแล้วรัน โปรแกรมจะตรวจการเปลี่ยนสถานะ 8 กรณีก่อนเปิดจอ (self_test)
#            ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง
#            ยังไม่ถูก = Console บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ)
# ถ้าเจอ   : NameError: name '____' isn't defined = ยังมีช่องที่ไม่ได้เติม (ตั้งใจให้หยุดชัด ๆ แบบนี้)
# ทบทวน    : OK -> WATCH (เกินเส้น) -> ALARM (เกินต่อเนื่อง HOLD_S วิ)
#            ALARM ค้างไว้แม้ค่าจะกลับปกติ จนกว่าคนจะกด SW5 รับทราบ -> COOLDOWN (พัก COOLDOWN_S วิ) -> OK
#            ระหว่าง WATCH ถ้าค่ากลับลงต่ำกว่าเส้นก่อนครบเวลา -> กลับ OK (ไม่เตือน)
# ติดขัด?  : รันไฟล์ตัวอย่างเต็ม cp2_03_decision_ladder.py เพื่อไปต่อก่อน แล้วค่อยกลับมาเทียบกับของตัวเอง
# เฉลย     : โจทย์หลัก มีเฉลยในคาบ อยู่ที่ practise/solutions/cp2_03_practise_solution.py
#            (ลองเองก่อน แล้วค่อยเปิดดูเมื่อจำเป็น)
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#
# (ทำจาก cp2_03_decision_ladder.py 87713d6e34b7)

import buttons
import pots
import random
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
LIMIT = 50           # เส้นเตือน (%)
HYST = 5             # ขั้น 2: ติดเมื่อเกิน LIMIT + HYST ดับเมื่อต่ำกว่า LIMIT - HYST
HOLD_S = 3           # ขั้น 3 และ 4: ต้องเห็นต่อเนื่องกี่วินาที
COOLDOWN_S = 10      # ขั้น 4: หลังรับทราบ พักเตือนกี่วินาที
NOISE = 2            # สั่นจำลองบวกลบกี่ % (0 = ลูกบิดล้วน)
DECIDE_MS = 100      # ตัดสินทุกกี่ ms (ทั้ง 4 วิธีเห็นค่าเดียวกันทุกครั้ง)
SAMPLE_MS = 20       # อ่านปุ่มทุกกี่ ms (เฟิร์มแวร์กรองสั่นทุกครั้งที่เราอ่าน อ่านห่างไปจะพลาดการแตะ)
TICK_MS = 500        # อัปเดตจอทุกกี่ ms (เฉพาะช่องที่เปลี่ยน)
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_input():
    # VR1 = ค่าที่วัด 0-100 % บวกสั่นจำลอง
    return pots.read(0) * 100 // 4095 + random.randint(-NOISE, NOISE)


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def hysteresis(on, v):
    # ขั้น 2: ระหว่างสองเส้นคงสถานะเดิม
    if v > LIMIT + HYST:
        return True
    if v < LIMIT - HYST:
        return False
    return on


def hold(on, over, t, now):
    # ขั้น 3: t = เวลาที่ over เริ่มต่างจาก on เปลี่ยนเมื่อต่างต่อเนื่องครบ HOLD_S วิ คืน (on, t)
    if over == on:
        return on, now
    if time.ticks_diff(now, t) >= HOLD_S * 1000:
        return over, now
    return on, t


def next_state(state, over, ack, t_s):
    # ขั้น 4: state = สถานะตอนนี้, over = เกินเส้นไหม, ack = เพิ่งกดรับทราบไหม
    # t_s = อยู่ในสถานะนี้มากี่วินาทีแล้ว คืนสถานะถัดไป
    if state == "OK":
        return "WATCH" if over else "OK"
    if state == "WATCH":
        if not over:
            return "OK"
        return "ALARM" if t_s >= ____ else "WATCH"     # ช่อง A: เกินต่อเนื่องนานเท่าไรจึงเตือน?
    if state == "ALARM":
        return "COOLDOWN" if ____ else "ALARM"         # ช่อง B: อะไรเท่านั้นที่ปลด ALARM ได้?
    return "OK" if t_s >= ____ else "COOLDOWN"         # ช่อง C: พักนานเท่าไรจึงกลับ OK?


def self_test():
    # ตรวจ next_state() 8 กรณีก่อนเปิดจอ: (สถานะ, เกินเส้น, กดรับทราบ, อยู่มากี่วิ, ควรได้)
    for c in (("WATCH", True, False, HOLD_S - 0.1, "WATCH"),
              ("WATCH", True, False, HOLD_S, "ALARM"),
              ("WATCH", False, False, 1, "OK"),
              ("ALARM", False, False, 99, "ALARM"),         # ค่ากลับปกติแล้ว แต่ยังไม่มีใครรับทราบ
              ("ALARM", True, False, 99, "ALARM"),
              ("ALARM", False, True, 0, "COOLDOWN"),
              ("COOLDOWN", True, False, COOLDOWN_S - 0.1, "COOLDOWN"),
              ("COOLDOWN", True, False, COOLDOWN_S, "OK")):
        got = next_state(c[0], c[1], c[2], c[3])
        if got != c[4]:
            print("ยังไม่ถูก: next_state", c[:4], "ควรได้", c[4], "แต่ได้", got)
            raise SystemExit
    print("ผ่าน! เครื่องสถานะถูกทั้ง 8 กรณี")


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
TITLES = ("1 เกณฑ์", "2 กันกระพือ", "3 รอให้นาน", "4 สถานะ")
STATE_COL = {"OK": COL_OK, "WATCH": COL_WARN, "ALARM": COL_BAD, "COOLDOWN": COL_INFO}


def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("บันไดตัดสิน: ค่าเดียว 4 วิธี", x=12, y=6, color=COL_TEXT, value=24)
    w = {"v": ui.Label(" ", x=12, y=38, color=COL_WARN, value=16), "led": [], "st": [], "seg": []}
    rules = ("> %d" % LIMIT, "> %d ดับ < %d" % (LIMIT + HYST, LIMIT - HYST),
             "ต่อเนื่อง %d วิ" % HOLD_S, "ค้างจนกด " + BTN_NAMES[0])
    for i in range(4):                                  # 4 การ์ดเรียงกัน กว้าง 186 ห่างกัน 194
        x = 12 + 194 * i
        card(x, 66, 186, 272, TITLES[i])
        w["led"].append(ui.Led(x=x + 14, y=100, w=44, h=44, color=COL_BAD, value=0))
        w["st"].append(ui.Label("ดับ", x=x + 64, y=114, color=COL_TEXT, value=16))
        ui.Label("สลับ (ครั้ง)", x=x + 14, y=160, color=COL_DIM, value=16)
        w["seg"].append(ui.Seg7(text="0", x=x + 14, y=186, w=150, h=56, color=COL_INFO))
        ui.Label(rules[i], x=x + 14, y=262, color=COL_DIM, value=16)
    w["help"] = ui.Label("หมุน VR1 ค้างแถวเส้น ดูตัวนับ   " + BTN_NAMES[0] + " = รับทราบ",
                         x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, i, on, n, st):
    w["seg"][i].text(str(n))
    w["led"][i].value(1 if on else 0)
    if i < 3:
        w["st"][i].text("ติด" if on else "ดับ")
    else:
        w["st"][3].text(st)
        w["led"][3].color(STATE_COL[st])
        w["led"][3].value(0 if st == "OK" else 1)


# ---- 6) โปรแกรมหลัก ----
def main():
    self_test()                           # ตรวจ next_state() ก่อน ไม่ผ่าน = หยุดตรงนี้
    w = build_screen()
    on = [False] * 4                      # ผลของแต่ละวิธี (วิธี 4: ติด = อยู่ใน ALARM)
    n = [0] * 4                           # สลับติด/ดับไปกี่ครั้ง
    shown = [None] * 4                    # สิ่งที่จอแสดงอยู่ (อัปเดตเฉพาะช่องที่เปลี่ยน)
    st, ack, last, v = "OK", False, False, 0
    t0 = t3 = t_st = t_dec = t_show = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        d = bool(buttons.pressed(0))                    # อ่านปุ่มทุกรอบ (ทุก SAMPLE_MS)
        if d and not last:
            ack = True                                  # จำไว้ว่าเพิ่งกด จนถึงรอบตัดสิน
            beep("tap")
        last = d
        if time.ticks_diff(now, t_dec) >= DECIDE_MS:
            t_dec = now
            v = read_input()                            # 1) อ่านค่าเดียว
            over = v > LIMIT
            h3, t3 = hold(on[2], over, t3, now)         # 2) ตัดสิน 4 วิธี
            ns = next_state(st, over, ack, time.ticks_diff(now, t_st) / 1000)
            ack = False
            if ns != st:                                # 3) ทำ: เสียงตอนสถานะเปลี่ยน
                if ns == "ALARM":
                    beep("bad")                       # alert
                elif ns == "OK" and st == "COOLDOWN":
                    beep("start")                       # ok: พักครบ กลับปกติ
                st, t_st = ns, now
            new = (over, hysteresis(on[1], v), h3, st == "ALARM")
            for i in range(4):
                if new[i] != on[i]:
                    n[i] += 1
            on = list(new)
        if time.ticks_diff(now, t_show) >= TICK_MS:      # 4) โชว์ ทุกครึ่งวินาที
            t_show = now
            w["v"].text("ค่า %d   เส้น %d" % (v, LIMIT))
            for i in range(4):
                s = (on[i], n[i], st if i == 3 else 0)
                if s != shown[i]:
                    show(w, i, on[i], n[i], st)
                    shown[i] = s
            ui.poll()
        time.sleep_ms(SAMPLE_MS)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: หมุน VR1 ค้างไว้ที่ 50 พอดี (สั่น +-2) หนึ่งนาที ตัวนับขั้นไหนวิ่ง ขั้นไหนนิ่ง เพราะอะไร
# 2) ตั้ง HOLD_S = 0 แล้วค้าง VR1 แถวเส้นเหมือนเดิม เทียบตัวนับขั้น 3 กับขั้น 1 (รอ 0 วิ = ไม่ได้รอ)
# 3) ตั้ง NOISE = 6 (เซนเซอร์สั่นมากขึ้น) HYST = 5 ยังพอไหม ต้องตั้ง HYST เท่าไรขั้น 2 จึงนิ่ง
