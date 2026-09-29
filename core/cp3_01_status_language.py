# cp3_01_status_language.py - หลักการ 3.1: สถานะหนึ่ง ภาษาเดียว (สี + รูปแบบ + เสียง)
#
# หลักการ  : ระบบแจ้งเตือนที่ดีมี "ภาษาสถานะ" ชุดเดียว สถานะเดียวกันต้องได้สี รูปแบบ และเสียงเดียวกัน
#            ทุกช่องทาง (ไฟ RGB บนบอร์ด จอไฟ 16x8 จอภาพ ลำโพง) ใครมองจากมุมไหนก็อ่านออกเหมือนกัน
#            ยิ่งรุนแรงยิ่งเร่ง: ติดนิ่ง -> กะพริบช้า -> กะพริบเร็ว · จอไฟ: สถานะนิ่ง = สีเต็มแผ่น
#            สถานะที่ต้องรีบดู = คำวิ่ง · ไม่พึ่งสีอย่างเดียว คนตาบอดสีก็อ่านจังหวะกับคำได้
#            เสียงดังเฉพาะตอนแย่ลงและตอนกลับมาปกติ ดีขึ้นแต่ยังไม่ปกติ = เงียบ
#            "อันตราย" ดังซ้ำได้ แต่ไม่ถี่กว่า REPEAT_S และต้องกดรับทราบให้เงียบได้ ไม่งั้นคนจะเลิกฟัง
#            (alarm fatigue) รับทราบแล้วเสียงเงียบ แต่ไฟยังบอกสถานะต่อไปจนกว่าต้นเหตุจะหาย
# ลองเล่น  : หมุน VR1 จากซ้ายสุดไปขวาสุด ดูไฟ RGB จอไฟ จอภาพ และฟังเสียงเปลี่ยนไปพร้อมกัน
#            ค้างไว้ที่ "อันตราย" นับว่าเสียงดังซ้ำทุกกี่วินาที แล้วกด SW5 (ปุ่มล่าง) = รับทราบ
# ของบนบอร์ด: VR1 = ตัวเลือกสถานะ 0-3 (แทนผลตัดสินจาก Part 2) · ไฟ RGB_GREEN / RGB_BLUE / RGB_RED
#            (เหลือง = RGB_RED + RGB_GREEN ติดพร้อมกัน) · จอไฟ RGB 16x8 · ลำโพง
#            SW5 (ปุ่มล่าง) = buttons.pressed(0) = รับทราบ
#            ไฟ RGB ใช้แค่ on()/off() และจังหวะกะพริบ: เฟิร์มแวร์มี led.brightness(0-100) ด้วย
#            แต่ไฟล์นี้ไม่ใช้เป็นภาษาสถานะ เพราะ Emulator แสดงไฟหรี่ค้างไว้ไม่ได้
# ในฟาร์ม  : ไฟเสาบนตู้ควบคุมปั๊ม ไฟหน้าโรงเรือน และแจ้งเตือนในแอป ใช้ภาษาเดียวกันหมด
#            คนงานที่ยืนไกลเห็นไฟแดงกะพริบเร็วก็รู้ทันทีว่าต้องวิ่งมา ไม่ต้องเดินมาอ่านจอ
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (คำวิ่งบนจอไฟใน Emulator ขึ้นแค่เฟรมแรก บนบอร์ดวิ่งจริง)

import buttons
import gpio
import pots
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
NAMES = ("ปกติ", "เฝ้าดู", "เตือน", "อันตราย")
LEDS = (("RGB_GREEN",), ("RGB_BLUE",), ("RGB_RED", "RGB_GREEN"), ("RGB_RED",))   # แดง + เขียว = เหลือง
BLINK_MS = (0, 0, 1000, 500)   # 0 = ติดนิ่ง · ตัวเลข = สลับติด/ดับทุกกี่ ms (ไม่ต่ำกว่า 500 จอภาพจึงตามทัน)
MATRIX = (rgbmatrix.GREEN, rgbmatrix.BLUE, rgbmatrix.YELLOW, rgbmatrix.RED)
WORDS = ("", "", "WARN", "ALARM")   # "" = จอไฟเติมสีเต็มแผ่นนิ่ง ๆ - คำ = วิ่งวน (อังกฤษ/ตัวเลขเท่านั้น)
HOW = ("เขียว ติดนิ่ง - เงียบ", "ฟ้า ติดนิ่ง - ติ๊ด", "เหลือง กะพริบช้า - เตือน", "แดง กะพริบเร็ว - เตือนซ้ำ")
REPEAT_S = 10        # "อันตราย" ดังซ้ำได้ไม่ถี่กว่านี้ จนกว่าจะกดรับทราบ
HYST = 3             # กันสถานะกระพือตอนลูกบิดค้างอยู่ที่ขอบช่วง (%)
SAMPLE_MS = 20       # อ่านปุ่มและลูกบิดทุกกี่ ms (ปุ่มกรองสั่นทุกครั้งที่อ่าน จึงต้องอ่านถี่)
TICK_MS = 1000       # อัปเดตตัวนับถอยหลังบนจอทุกกี่ ms (เลขเปลี่ยนวินาทีละครั้ง จึงไม่ต้องถี่กว่านี้)
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
COLS = (COL_OK, COL_INFO, COL_WARN, COL_BAD)   # สีบนจอภาพ ตรงกับไฟ RGB และจอไฟ


# ---- 2) ฮาร์ดแวร์ ----
# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


def led_named(name):
    # หา LED ด้วยชื่อ ไม่ใช่เลข: บน Dev Kit ดวง LED1/LED2 (เลข 0, 1) อยู่บน SoM
    # มองไม่เห็น ดวงที่เห็นคือ RGB_RED / RGB_GREEN / RGB_BLUE
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def show_light(leds, names, lit):
    # ไฟ RGB: ดวงที่อยู่ในชุดสีของสถานะติด ดวงอื่นดับ (lit = False คือจังหวะดับของการกะพริบ)
    for n in leds:
        if leds[n]:
            leds[n].value(1 if lit and n in names else 0)


def show_matrix(state):
    # จอไฟ 16x8 เขียนเฉพาะตอนสถานะเปลี่ยน · คำสั่งวาดทุกตัวหยุดคำวิ่งที่ค้างอยู่ให้เอง
    # state = -1 คือดับจอ (fill รับสี 1-7 เท่านั้น ดับต้องใช้ clear)
    try:
        if state < 0:
            rgbmatrix.clear()
        elif WORDS[state]:
            rgbmatrix.scroll(WORDS[state], MATRIX[state], 80)
        else:
            rgbmatrix.fill(MATRIX[state])
    except OSError:
        pass


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def pick_state(pct, now):
    # VR1 0-100 % -> สถานะ 0-3 ช่วงละ 25 %  ยังอยู่ในช่วงเดิม (เผื่อขอบ HYST) = คงสถานะเดิม
    if now * 25 - HYST <= pct < (now + 1) * 25 + HYST:
        return now
    return min(3, pct // 25)


def sound_for(old, new):
    # เสียงดังเฉพาะตอนแย่ลง หรือตอนกลับมาปกติ · ดีขึ้นแต่ยังไม่ปกติ = เงียบ (None)
    if new == 0:
        return "good" if old else None                   # กลับมาปกติ = good
    if new > old:
        return "tap" if new == 1 else "bad"              # เฝ้าดู = tap - เตือน/อันตราย = bad
    return None


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("สถานะหนึ่ง ภาษาเดียว", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("หมุน VR1 เลือกสถานะ - SW5 = รับทราบ", x=12, y=38, color=COL_DIM, value=16)
    card(12, 64, 300, 274, "ตอนนี้")
    w = {"led": ui.Led(x=28, y=96, w=80, h=80, color=COL_OK)}
    w["name"] = ui.Label(" ", x=124, y=116, color=COL_TEXT, value=28)
    w["msg"] = ui.Label(" ", x=28, y=204, color=COL_TEXT, value=16)   # นับถอยหลังเสียงซ้ำ / รับทราบแล้ว
    card(322, 64, 458, 274, "สถานะ - สี - รูปแบบ - เสียง")
    for i in range(4):                               # แถวละสถานะ: ชื่อ + คำบนจอไฟ (สีของสถานะ) แล้วบรรทัดรูปแบบ
        ui.Label(NAMES[i] + "  " + WORDS[i], x=338, y=94 + i * 58, color=COLS[i], value=20)
        ui.Label(HOW[i], x=338, y=122 + i * 58, color=COL_DIM, value=16)
    w["help"] = ui.Label("แย่ลง/กลับมาปกติ = ดัง - ดีขึ้นเฉย ๆ = เงียบ", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_state(w, state):
    w["led"].color(COLS[state])
    w["led"].value(1)
    w["name"].color(COLS[state])
    w["name"].text(NAMES[state])
    w["msg"].text(" ")


# ---- 6) โปรแกรมหลัก ----
def main():
    w = build_screen()
    leds = {n: led_named(n) for n in ("RGB_RED", "RGB_GREEN", "RGB_BLUE")}
    state, acked, lit, was = -1, False, True, False
    t0 = t_show = t_blink = t_rep = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        old = max(state, 0)                                    # รอบแรก state = -1 นับเป็น "ปกติ" (เริ่มแบบเงียบ)
        new = pick_state(pots.read(0) * 100 // 4095, old)      # 1) VR1 เป็น 0-100 % แล้วตัดสินสถานะ
        if new != state:                                       # 2) สถานะเปลี่ยน: ทุกช่องทางเปลี่ยนพร้อมกัน
            notes = sound_for(old, new)
            state, acked, lit, t_blink, t_rep = new, False, True, now, now
            show_light(leds, LEDS[state], True)
            show_matrix(state)
            show_state(w, state)
            if notes:
                beep(notes)
        if BLINK_MS[state] and time.ticks_diff(now, t_blink) >= BLINK_MS[state]:
            lit, t_blink = not lit, now                        # 3) จังหวะกะพริบ: ไฟ RGB กับ Led บนจอพร้อมกัน
            show_light(leds, LEDS[state], lit)
            w["led"].value(1 if lit else 0)
        down = buttons.pressed(0)                              # SW5 กรองสั่นทุกครั้งที่อ่าน จึงอ่านทุก 20 ms
        if down and not was and state >= 2 and not acked:     # ขอบกด SW5 = รับทราบ
            acked = True
            w["msg"].text("รับทราบ: เงียบ ไฟยังอยู่")
            beep("tap")
        was = down
        if state == 3 and not acked and time.ticks_diff(now, t_rep) >= REPEAT_S * 1000:
            t_rep = now                                        # 4) เตือนซ้ำ แต่ไม่ถี่กว่า REPEAT_S
            beep("bad")
        if time.ticks_diff(now, t_show) >= TICK_MS:            # 5) นับถอยหลังบนจอ วินาทีละครั้ง
            t_show = now
            if state == 3 and not acked:
                w["msg"].text("ดังซ้ำอีก %d วิ" % (REPEAT_S - time.ticks_diff(now, t_rep) // 1000))
            ui.poll()
        time.sleep_ms(SAMPLE_MS)
    show_light(leds, (), False)
    show_matrix(-1)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    beep("good")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: หมุน VR1 จาก "อันตราย" ลงมา "เตือน" แล้วกลับขึ้น "อันตราย" จะได้ยินเสียงกี่ครั้ง เพราะอะไร
# 2) ตั้ง REPEAT_S = 3 แล้วค้างที่ "อันตราย" สักครึ่งนาที รำคาญหรือยัง ถ้าเป็นคนงานในฟาร์มจะทำอย่างไรกับเครื่องนี้
# 3) ตั้ง HYST = 0 แล้วค่อย ๆ หมุน VR1 ค้างไว้ตรงรอยต่อ 50 % ฟังว่าเกิดอะไรขึ้น (นี่คือเหตุที่ต้องมีช่องกันกระพือ)
