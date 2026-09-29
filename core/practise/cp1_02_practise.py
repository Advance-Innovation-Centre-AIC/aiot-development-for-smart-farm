# cp1_02_practise.py - แบบฝึกเติมโค้ด (Code Quest ระดับ 3 โบนัส): แปลงช่วงและจำกัดขอบ
#
# วิธีเล่น  : ไฟล์นี้เหมือน cp1_02_knob_scaling.py ทุกอย่าง ยกเว้น map_range() และ clamp()
#            ในส่วน "3) สมอง" ที่เว้นช่อง ____ (ขีดล่างสี่ตัว) ไว้ 4 ช่อง: A, B (ใน map_range) และ C, D (ใน clamp)
#            เติมให้ครบแล้วรัน โปรแกรมจะตรวจ 6 กรณีก่อนเปิดจอ (self_test)
#            ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง
#            ยังไม่ถูก = Console บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ)
# ถ้าเจอ   : NameError: name '____' isn't defined = ยังมีช่องที่ไม่ได้เติม (ตั้งใจให้หยุดชัด ๆ แบบนี้)
# ทบทวน    : แปลงช่วงแบบเส้นตรง = เอา "ระยะที่เดินมาแล้ว" ในช่วงเข้า (x - จุดเริ่ม) หารด้วยความกว้างช่วงเข้า
#            ได้เป็นสัดส่วน 0..1 แล้วคูณความกว้างช่วงออก บวกจุดเริ่มของช่วงออก
#            clamp = ต่ำกว่าขอบล่างให้เป็นขอบล่าง สูงกว่าขอบบนให้เป็นขอบบน นอกนั้นคงเดิม
# ติดขัด?  : รันไฟล์ตัวอย่างเต็ม cp1_02_knob_scaling.py เพื่อไปต่อก่อน แล้วค่อยกลับมาเทียบกับของตัวเอง
# เฉลย     : โจทย์โบนัส ไม่มีเฉลยในโฟลเดอร์นี้ ลองเองให้สุดก่อน แล้วถามผู้สอน
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#
# (ทำจาก cp1_02_knob_scaling.py fffe781eab8e)

import buttons
import pots
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
DEADBAND = 20        # raw ต้องขยับเกินกี่ count ตัวเลขบนจอจึงยอมเปลี่ยน (20 count ราว 0.5 % ของช่วง)
RAW_LO = 0           # raw ที่ปลายซ้ายสุดของลูกบิด (ถ้าหมุนสุดแล้วไม่ถึง 0 ให้ใส่เลขที่อ่านได้จริง)
RAW_HI = 4095        # raw ที่ปลายขวาสุดของลูกบิด
SAMPLE_MS = 20       # อ่านปุ่มทุกกี่ ms (เฟิร์มแวร์กรองสั่น 50 ms ทุกครั้งที่อ่าน จึงต้องอ่านถี่)
TICK_MS = 500        # อ่านลูกบิดและอัปเดตจอทุกกี่ ms
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

# ลูกบิดแต่ละตัว: (ชื่อบนการ์ด, หน่วย, ค่าต่ำสุด, ค่าสูงสุด, รูปแบบตัวเลข, จำนวนขีดบนไม้บรรทัด)
KNOBS = (("VR1 ความชื้นดิน", "%", 0, 100, "%.0f", 11),
         ("VR2 อุณหภูมิเป้า", "C", 15, 40, "%.1f", 11),
         ("VR3 น้ำในถัง", "ลิตร", 0, 500, "%.0f", 11),
         ("VR4 ตั้งเวลารดน้ำ", "นาที", 0, 60, "%.0f", 13))

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_raw(i):
    # เลขดิบของลูกบิด i (0 = VR1 ... 3 = VR4) ได้ 0-4095 (ไม่มี pots.percent() บนบอร์ด)
    return pots.read(i)


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def map_range(x, in_lo, in_hi, out_lo, out_hi):
    # แปลงช่วงแบบเส้นตรง: in_lo -> out_lo, in_hi -> out_hi (ค่าระหว่างนั้นเป็นสัดส่วนเดียวกัน)
    # ช่อง A: x เดินมาจาก "จุดไหน" ของช่วงเข้า?   ช่อง B: ความกว้างช่วงเข้า = อะไร - in_lo ?
    return out_lo + (x - ____) * (out_hi - out_lo) / (____ - in_lo)


def clamp(v, lo, hi):
    # ค่าที่หลุดช่วง (เช่น raw เกิน RAW_HI) ถูกตัดให้อยู่ที่ขอบ lo..hi
    # ช่อง C: v มากกว่า "อะไร" จึงต้องตัด?   ช่อง D: อยู่ในช่วงแล้ว ควรคืนค่าอะไร?
    return lo if v < lo else (hi if v > ____ else ____)


def self_test():
    # ตรวจ map_range() และ clamp() 6 กรณีก่อนเปิดจอ: (ฟังก์ชัน, อาร์กิวเมนต์, ค่าที่ควรได้)
    for f, a, want in ((map_range, (0, 0, 4095, 0, 100), 0),
                       (map_range, (4095, 0, 4095, 0, 500), 500),
                       (map_range, (1500, 1000, 2000, 0, 60), 30),     # ช่วงเข้าไม่ได้เริ่มที่ 0
                       (map_range, (2000, 0, 4000, 15, 40), 27.5),     # ช่วงออกไม่ได้เริ่มที่ 0
                       (clamp, (120, 0, 100), 100),
                       (clamp, (42, 0, 100), 42)):
        try:
            got = f(*a)
        except ZeroDivisionError:
            got = None                     # หารด้วยศูนย์ = ความกว้างช่วงเข้าผิด (ดูช่อง B)
        if got is None or abs(got - want) > 0.01:
            print("ยังไม่ถูก:", "map_range" if f is map_range else "clamp", a, "ควรได้", want, "แต่ได้", got)
            raise SystemExit
    print("ผ่าน! map_range และ clamp ถูกทั้ง 6 กรณี")


def to_unit(raw, k):
    # เลขดิบ -> ค่าที่มีหน่วยของลูกบิด k
    return clamp(map_range(raw, RAW_LO, RAW_HI, k[2], k[3]), k[2], k[3])


def step_size(k):
    # raw ขยับ 1 count = ค่าที่มีหน่วยขยับเท่าไร
    return (k[3] - k[2]) / (RAW_HI - RAW_LO)


def moved(raw, shown, band):
    # ช่องกันกะพริบ: ยอมเปลี่ยนเลขบนจอเมื่อ raw ห่างจาก raw ที่จอใช้อยู่เกิน band
    return shown is None or abs(raw - shown) > band


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_knob_card(k, x, y):
    card(x, y, 378, 134, k[0] + " (" + k[1] + ")")
    c = {"val": ui.Label("--", x=x + 12, y=y + 30, color=COL_TEXT, value=28)}
    c["raw"] = ui.Label("raw --", x=x + 170, y=y + 30, color=COL_WARN, value=16)
    ui.Label("1 ขั้น = %.4f " % step_size(k) + k[1], x=x + 170, y=y + 52, color=COL_DIM, value=14)
    c["bar"] = ui.Bar(x=x + 12, y=y + 76, w=354, h=14, min=k[2], max=k[3], value=k[2])
    c["bar"].color(COL_INFO)        # สีตอนสร้างใช้ไม่ได้กับ Bar ต้องตั้งหลังสร้าง
    ruler = ui.Scale(x=x + 12, y=y + 94, w=354, h=32, color=COL_DIM, min=k[2], max=k[3])
    ruler.ticks(k[5], 2)            # ไม้บรรทัดใต้แถบ ขีดใหญ่ทุก 2 ขีด (Scale ไม่มีเข็ม)
    c["txt"] = ""
    return c


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ลูกบิด: เลขดิบ -> ค่าที่มีหน่วย", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("ค่า = ต่ำสุด + (raw - RAW_LO) x ช่วง / (RAW_HI - RAW_LO) แล้ว clamp", x=12, y=38,
             color=COL_DIM, value=16)
    w = {"cards": [build_knob_card(KNOBS[0], 12, 62), build_knob_card(KNOBS[1], 402, 62),
                   build_knob_card(KNOBS[2], 12, 204), build_knob_card(KNOBS[3], 402, 204)]}
    w["help"] = ui.Label(" ", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_band(w, band):
    w["help"].text(BTN_NAMES[0] + " (ล่าง) = เปิด/ปิดกันกะพริบ   ตอนนี้: " +
                   ("%d count" % band if band else "ปิด"))


def show_raw(c, raw, skips):
    # เขียนป้าย raw เฉพาะตอนข้อความเปลี่ยน
    s = "raw %d  ข้าม %d" % (raw, skips)
    if s != c["txt"]:
        c["txt"] = s
        c["raw"].text(s)


def show_value(c, k, v):
    c["val"].text(k[4] % v + " " + k[1])
    c["bar"].value(int(round(v)))                # widget รับเฉพาะจำนวนเต็ม


# ---- 6) โปรแกรมหลัก ----
def main():
    self_test()                            # ตรวจ map_range() และ clamp() ก่อน ไม่ผ่าน = หยุดตรงนี้
    w = build_screen()
    band = DEADBAND
    show_band(w, band)
    shown = [None, None, None, None]       # raw ที่ตัวเลขบนจอใช้อยู่ (None = ยังไม่เคยแสดง)
    last = [None, None, None, None]        # raw ที่อ่านได้รอบก่อน
    skips = [0, 0, 0, 0]
    was = False
    t0 = t_show = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        down = bool(buttons.pressed(0))
        if down and not was:                                   # ขอบกดลงของ SW5
            band = 0 if band else DEADBAND
            show_band(w, band)
            if band:
                beep("start")                                  # ok: เปิดกันกะพริบ
            else:
                beep("tap")                                         # press: ปิด
        was = down
        if time.ticks_diff(now, t_show) >= TICK_MS:
            t_show = now
            for i in range(4):
                raw = read_raw(i)                              # 1) อ่านเลขดิบ
                k, c = KNOBS[i], w["cards"][i]
                if moved(raw, shown[i], band):                 # 2) ขยับพอไหม
                    shown[i] = raw
                    show_value(c, k, to_unit(raw, k))          # 3) แปลงหน่วยแล้วโชว์
                elif raw != last[i]:
                    skips[i] += 1                              # raw เปลี่ยน แต่ตัวเลขบนจอไม่เปลี่ยน
                last[i] = raw
                show_raw(c, raw, skips[i])
            ui.poll()
        time.sleep_ms(SAMPLE_MS)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: หมุน VR1 ไปกลางช่วง read() ได้ประมาณ ____ -> ความชื้นดิน ____ %
# 2) ตั้ง RAW_LO = 500 และ RAW_HI = 3500 แล้วหมุน VR1 สุดทั้งสองทาง ตัวเลขหยุดที่เท่าไร
#    (นี่คืองานของ clamp) ลองลบ clamp ออกจาก to_unit() แล้วดูว่าจะได้ค่าที่เป็นไปไม่ได้แค่ไหน
# 3) ตั้ง DEADBAND = 200 แล้วหมุนช้า ๆ ตัวเลขกระโดดทีละเท่าไร ค่าไหนพอดีกับงานรดน้ำ
