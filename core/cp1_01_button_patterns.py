# cp1_01_button_patterns.py - หลักการ 1.1: ปุ่มหนึ่งปุ่ม อ่านได้สามแบบ (ระดับ ขอบ รูปแบบ)
#
# หลักการ  : "ระดับ" = ตอนนี้กดอยู่ไหม · "ขอบ" = เพิ่งเปลี่ยนจากปล่อยเป็นกด
#            "รูปแบบ" = คลิก กดค้าง ดับเบิลคลิก ซึ่งต้องดูเวลาประกอบ
#            เฟิร์มแวร์กรองสัญญาณสั่นของปุ่ม (debounce 50 ms) ทุกครั้งที่เราเรียก buttons.pressed()
#            จึงต้องอ่านถี่ (ทุก SAMPLE_MS) ส่วนขอบและรูปแบบเป็นงานของ Python
# ลองเล่น  : กด SW5 หรือ SW6 สั้น ๆ = คลิก · กดค้างเกิน 0.8 วิ = ค้าง · กดสองครั้งติดกันเร็ว ๆ = ดับเบิล
#            ดูไฟบนจอ (ระดับ) ตัวเลขใหญ่ (จำนวนขอบ) และแถวล่าง (รูปแบบ) เปลี่ยนตามกัน
# ของบนบอร์ด: ปุ่ม SW5 (ปุ่มล่าง) = buttons.pressed(0) · SW6 (ปุ่มบน) = buttons.pressed(1) · ลำโพง
# ในฟาร์ม  : ปุ่มเดียวบนตู้ควบคุม กดสั้น = ดูค่า กดค้าง = สั่งปั๊ม (กันกดพลาด) ดับเบิล = รับทราบเตือน
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator

import buttons
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
LONG_MS = 800        # กดค้างนานเท่านี้ขึ้นไป = "ค้าง"
DOUBLE_MS = 400      # ปล่อยครั้งที่สองภายในเวลานี้หลังครั้งแรก = "ดับเบิล"
SAMPLE_MS = 10       # อ่านปุ่มทุกกี่ ms (ถี่พอจะไม่พลาดการแตะสั้น ๆ)
TICK_MS = 500        # อัปเดตตัวเลขบนจอทุกกี่ ms (ถี่กว่านี้จอกะพริบ)
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def is_down(i):
    # ระดับของปุ่ม i (0 = SW5, 1 = SW6): True = กดอยู่
    # เฟิร์มแวร์กรองสั่นตอนเราเรียกฟังก์ชันนี้ ถ้าเรียกห่างเกิน การแตะสั้น ๆ จะหายไป
    return bool(buttons.pressed(i))


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
class Pattern:
    # ตัวจับรูปแบบของปุ่มหนึ่งปุ่ม ป้อน (กดอยู่ไหม, เวลา ms) ทุกรอบ ได้ "click" "long" "double" หรือ None
    def __init__(self):
        self.down = False
        self.t_down = 0
        self.t_up = None     # เวลาปล่อยของคลิกแรกที่ยังรอดูว่าจะกลายเป็นดับเบิลไหม
        self.held = False    # ครั้งนี้นับเป็น "ค้าง" ไปแล้ว ปล่อยแล้วไม่ต้องนับคลิกซ้ำ
        self.edges = 0       # จำนวนขอบขาลง (ปล่อย -> กด)

    def feed(self, down, now):
        ev = None
        if down and not self.down:                       # ขอบ: เพิ่งกดลง
            self.edges += 1
            self.t_down, self.held = now, False
        elif down and not self.held and time.ticks_diff(now, self.t_down) >= LONG_MS:
            self.held, self.t_up, ev = True, None, "long"
        elif self.down and not down and not self.held:   # ขอบ: เพิ่งปล่อยหลังกดสั้น
            if self.t_up is not None and time.ticks_diff(now, self.t_up) <= DOUBLE_MS:
                self.t_up, ev = None, "double"
            else:
                self.t_up = now                          # รอดูก่อนว่าจะมีครั้งที่สองไหม
        elif not down and self.t_up is not None and time.ticks_diff(now, self.t_up) > DOUBLE_MS:
            self.t_up, ev = None, "click"                # หมดเวลารอ = คลิกเดี่ยว
        self.down = down
        return ev


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
NAMES = {"click": "คลิก", "long": "ค้าง", "double": "ดับเบิล"}
SOUNDS = {"click": "tap", "double": "start", "long": "good"}   # ชื่อเสียงใน TUNES


def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_button_card(i, x):
    card(x, 66, 378, 272, BTN_NAMES[i] + (" (ปุ่มล่าง)" if i == 0 else " (ปุ่มบน)"))
    c = {"led": ui.Led(x=x + 14, y=100, w=40, h=40, color=COL_OK, value=0)}
    c["lvl"] = ui.Label("ระดับ: ปล่อย", x=x + 66, y=108, color=COL_TEXT, value=20)
    ui.Label("ขอบกดลง", x=x + 14, y=154, color=COL_DIM, value=16)
    c["seg"] = ui.Seg7(text="0", x=x + 14, y=178, w=150, h=56, color=COL_INFO)
    c["cnt"] = ui.Label("คลิก 0  ค้าง 0  ดับเบิล 0", x=x + 14, y=246, color=COL_TEXT, value=16)
    c["last"] = ui.Label(" ", x=x + 14, y=280, color=COL_WARN, value=28)
    return c


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ปุ่มเดียว อ่านได้ 3 แบบ", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("ระดับ -> ขอบ -> รูปแบบ  (กรองสั่น 50 ms ในเฟิร์มแวร์)", x=12, y=38,
             color=COL_DIM, value=16)
    w = {"cards": [build_button_card(0, 12), build_button_card(1, 402)]}
    w["help"] = ui.Label("สั้น = คลิก  ค้าง 0.8 วิ = ค้าง  2 ครั้งเร็ว = ดับเบิล", x=12, y=352,
                         color=COL_DIM, value=16)
    ui.poll()
    return w


def show_level(c, down):
    c["led"].value(1 if down else 0)
    c["lvl"].text("ระดับ: กด" if down else "ระดับ: ปล่อย")


def show_counts(c, p, counts):
    c["seg"].text(str(p.edges))
    c["cnt"].text("คลิก %d  ค้าง %d  ดับเบิล %d" % (counts["click"], counts["long"], counts["double"]))


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    pats = [Pattern(), Pattern()]
    counts = [{"click": 0, "long": 0, "double": 0}, {"click": 0, "long": 0, "double": 0}]
    t0 = t_show = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        for i in (0, 1):
            down = is_down(i)                                  # 1) อ่านระดับ
            c, p = w["cards"][i], pats[i]
            if down != p.down:
                show_level(c, down)                            # ไฟบนจอเปลี่ยนเฉพาะตอนระดับเปลี่ยน
            ev = p.feed(down, now)                             # 2) ตัดสินรูปแบบ
            if ev:
                counts[i][ev] += 1                             # 3) ทำ: เสียงหนึ่งครั้งต่อเหตุการณ์
                c["last"].text(NAMES[ev] + "!")
                beep(SOUNDS[ev])
        if time.ticks_diff(now, t_show) >= TICK_MS:            # 4) โชว์ตัวเลข ทุกครึ่งวินาที
            t_show = now
            for i in (0, 1):
                show_counts(w["cards"][i], pats[i], counts[i])
            ui.poll()
        time.sleep_ms(SAMPLE_MS)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: กดสองครั้งห่างกัน 0.7 วิ ขณะที่ DOUBLE_MS = 400 จะนับเป็นอะไร กี่ครั้ง
# 2) ตั้ง LONG_MS = 2000 แล้วลองกดค้าง 1 วิ ได้ผลอะไร เพราะอะไร
# 3) ตั้ง SAMPLE_MS = 300 แล้วแตะปุ่มเร็ว ๆ นับว่าพลาดไปกี่ครั้ง (อ่านช้าเกิน = ไม่เห็นการกด)
