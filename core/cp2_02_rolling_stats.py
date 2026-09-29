# cp2_02_rolling_stats.py - หลักการ 2.2: สถิติของหน้าต่างเลื่อน บอกว่าค่าล่าสุด "ผิดปกติแค่ไหน"
#
# หลักการ  : เก็บ WIN ค่าล่าสุดไว้ในรายการ (list) ค่าใหม่เข้าท้าย ค่าเก่าสุดออกหัว = หน้าต่างเลื่อน
#            จากหน้าต่างได้ ค่าเฉลี่ย SD (ค่าแกว่งปกติ) ต่ำสุด สูงสุด และความชันต่อนาที
#            z = (ค่าล่าสุด - ค่าเฉลี่ย) / SD = ห่างจากปกติกี่ SD ถ้าสัญญาณรบกวนเป็นรูประฆังคว่ำ
#            ราว 95 % ของค่าปกติอยู่ใน +-2 SD ค่าที่หลุด Z_MAX จึงน่าสงสัย
#            z เทียบค่าล่าสุดกับ WIN ค่า "ก่อนหน้า" (ไม่รวมตัวเอง) ค่าแปลกจึงไม่ไปดึงค่าเฉลี่ยของตัวเอง
#            โมดูล dsp ไม่มีค่าเฉลี่ย/SD/z ให้ เราเขียนเองราว 10 บรรทัด (สูตร Welford)
# ลองเล่น  : เริ่มที่ VR1 รอให้เก็บครบ 30 ค่า (15 วิ) แล้วหมุน VR1 เร็ว ๆ -> z พุ่ง ไฟแดงติด
#            หมุนค้างไว้ที่ใหม่ -> หน้าต่างค่อย ๆ "ลืม" ของเก่า z กลับเข้าใกล้ 0
#            กด SW5 สลับเป็นอุณหภูมิ SHT40 รอครบ 30 ค่า แล้วเป่าลมหายใจใส่เซนเซอร์ -> z พุ่ง
# ของบนบอร์ด: ลูกบิด VR1 · SHT40 (อุณหภูมิ) · ปุ่ม SW5 (ปุ่มล่าง) = สลับที่มา · ลำโพง
# ในฟาร์ม  : อุณหภูมิเล้าไก่ขึ้นเร็วผิดปกติ (พัดลมดับ) เห็นได้จาก z ก่อนจะถึงเกณฑ์ตายตัว
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (Emulator: อุณหภูมิมาจากแถบเลื่อนบนแผง
#            มีสั่นเล็ก ๆ ที่โปรแกรมจำลองใส่ไว้ ไม่ใช่สั่นของเซนเซอร์จริง)

import buttons
import math
import pots
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
WIN = 30             # หน้าต่างกี่ค่า (30 x 0.5 วิ = 15 วิล่าสุด)
Z_MAX = 2.5          # |z| เกินเท่านี้ = ผิดปกติ ไฟแดงบนจอติด
BAND_SD = 2          # เส้นเหลืองบนกราฟ = ค่าเฉลี่ย +- กี่ SD
SD_MIN = (1.0, 0.1)  # SD ต่ำสุดที่ยอมหาร (VR1 เป็น %, อุณหภูมิเป็น C) ค่านิ่งสนิท SD เกือบ 0 หารแล้ว z พุ่งเกินจริง
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง เทียบเทอร์โมมิเตอร์ในห้องแล้วใส่ค่าชดเชย เช่น -9.5
TICK_MS = 500        # เก็บ 1 ค่าและอัปเดตจอทุกกี่ ms
SAMPLE_MS = 20       # อ่านปุ่มทุกกี่ ms (ระหว่างรอ)
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

SRC = ("VR1", "SHT40")       # ที่มา 0 = ลูกบิด (ค่าเริ่ม ใช้ในห้องเรียนสะดวก), 1 = อุณหภูมิ
UNIT = ("%", "C")
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_value(src):
    # ค่าล่าสุดของที่มา src: VR1 = 0-100 %, SHT40 = องศา C (ชดเชยแล้ว) อ่านไม่ได้คืน None
    if src == 0:
        return pots.read(0) * 100 / 4095
    for _ in range(3):                  # SHT40 อ่านพลาดได้บางจังหวะ (บัสไม่ว่าง) จึงลองซ้ำ
        try:
            return sensors.sht40.temperature() + TEMP_OFFSET
        except Exception:
            time.sleep_ms(50)
    return None


def wait_click(ms, last):
    # รอ ms แต่อ่าน SW5 ทุก SAMPLE_MS: เฟิร์มแวร์กรองสั่นทุกครั้งที่เราอ่าน pressed() อ่านห่างไปจะพลาดการแตะ
    # คืน (มีการกดลงใหม่ไหม, สถานะปุ่มล่าสุด)
    hit, t0 = False, time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        d = bool(buttons.pressed(0))
        hit = hit or (d and not last)
        last = d
        time.sleep_ms(SAMPLE_MS)
    return hit, last


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def mean_sd(win):
    # Welford: เดินผ่านหน้าต่างรอบเดียว ได้ค่าเฉลี่ยและ SD (แบบตัวอย่าง หารด้วย n - 1)
    # ไม่ต้องบวกกำลังสองก้อนใหญ่แล้วลบกัน ซึ่งทำให้ทศนิยมหายบน float 32 บิตของบอร์ด
    n, mean, m2 = 0, 0.0, 0.0
    for x in win:
        n += 1
        d = x - mean
        mean += d / n
        m2 += d * (x - mean)
    return mean, (math.sqrt(m2 / (n - 1)) if n > 1 else 0.0)


def slope_per_min(win):
    # ความชัน = (ค่าเฉลี่ยครึ่งหลัง - ค่าเฉลี่ยครึ่งแรก) / ระยะห่างของกลางสองครึ่ง (นับเป็นค่า)
    # ทนสั่นกว่า "ค่าท้าย - ค่าหัว" เพราะเฉลี่ยครึ่งละหลายค่า แล้วแปลงจากต่อ 1 ค่าเป็นต่อนาที
    h = len(win) // 2
    return (sum(win[-h:]) - sum(win[:h])) / h / (len(win) - h) * 60000 / TICK_MS


def zscore(x, mean, sd, sd_min):
    # z = ห่างจากค่าเฉลี่ยกี่ SD (บวก = สูงกว่าปกติ ลบ = ต่ำกว่าปกติ)
    return (x - mean) / max(sd, sd_min)


def to_chart(v, src, base):
    # กราฟรับจำนวนเต็ม 0-100: VR1 = % ตรง ๆ, อุณหภูมิ = กลางกราฟคือค่าตอนเริ่ม ช่องละ 0.1 C (ขอบ +-5 C)
    if src == 1:
        v = 50 + (v - base) * 10
    return int(round(max(0, min(100, v))))


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
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
    ui.Label("หน้าต่างเลื่อน -> z", x=12, y=6, color=COL_TEXT, value=24)
    w = {"scale": ui.Label(" ", x=12, y=38, color=COL_DIM, value=16)}
    ch = line_chart(12, 66, 400, 272, 0, 100, COL_INFO)
    w["ch"], w["hi"], w["lo"] = ch, ch.add_series(COL_WARN), ch.add_series(COL_WARN)
    card(424, 66, 356, 272, "เทียบ %d ค่าก่อนหน้า  ไฟ = |z| เกิน %.1f" % (WIN, Z_MAX))
    w["led"] = ui.Led(x=436, y=100, w=48, h=48, color=COL_BAD, value=0)
    w["z"] = ui.Label(" ", x=500, y=108, color=COL_TEXT, value=28)
    w["now"] = ui.Label(" ", x=436, y=170, color=COL_INFO, value=20)
    w["ms"] = ui.Label(" ", x=436, y=214, color=COL_WARN, value=16)
    w["mm"] = ui.Label(" ", x=436, y=248, color=COL_TEXT, value=16)
    w["help"] = ui.Label(BTN_NAMES[0] + " = สลับ VR1/SHT40   เป่าลม -> z พุ่ง", x=12, y=352,
                         color=COL_DIM, value=16)
    ui.poll()
    return w


def show_source(w, src, base):
    w["scale"].text("ฟ้า = " + SRC[src] + "  เหลือง = เฉลี่ย +-%d SD  " % BAND_SD +
                    ("กราฟ 0-100 %" if src == 0 else "กลาง %.1f C" % base))


def show_stats(w, src, x, win, mean, sd, z):
    f = "%.1f" if src == 0 else "%.2f"
    u = " " + UNIT[src]
    w["now"].text((SRC[src] + " " + f) % x + u)
    if len(win) < WIN:
        w["z"].text("เก็บ %d/%d" % (len(win), WIN))
    else:
        w["z"].text("z = %+.1f" % z)
        w["ms"].text(("เฉลี่ย " + f + "  SD " + f) % (mean, sd) + u)
        w["mm"].text(("ต่ำ " + f + "  สูง " + f + "  ชัน %+.2f/นาที") % (min(win), max(win), slope_per_min(win)))


# ---- 6) โปรแกรมหลัก ----
def main():
    w = build_screen()
    src, win, alarm, last = 0, [], False, False
    base = read_value(1) or 0.0          # ค่าอุณหภูมิตอนเริ่ม = กลางกราฟเมื่อสลับไป SHT40
    show_source(w, src, base)
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        x = read_value(src)                                  # 1) อ่าน
        if x is None:
            w["now"].text("อ่านไม่ได้")
        else:
            mean, sd = mean_sd(win) if len(win) > 1 else (x, 0.0)   # 2) สถิติของหน้าต่างก่อนหน้า
            z = zscore(x, mean, sd, SD_MIN[src]) if len(win) >= WIN else 0.0
            now_alarm = abs(z) > Z_MAX                        # 3) ตัดสิน
            if now_alarm and not alarm:
                beep("bad")                                 # alert: เฉพาะขอบขาขึ้น
            if now_alarm != alarm:
                w["led"].value(1 if now_alarm else 0)
            alarm = now_alarm
            show_stats(w, src, x, win, mean, sd, z)           # 4) โชว์
            band = BAND_SD * max(sd, SD_MIN[src])
            w["ch"].set_next(0, to_chart(x, src, base))
            w["ch"].set_next(w["hi"], to_chart(mean + band, src, base))
            w["ch"].set_next(w["lo"], to_chart(mean - band, src, base))
            win.append(x)                                     # ค่าใหม่เข้าท้าย
            if len(win) > WIN:
                win.pop(0)                                    # ค่าเก่าสุดออกหัว (list ธรรมดา ไม่ใช้ deque)
        ui.poll()
        hit, last = wait_click(TICK_MS, last)
        if hit:                                               # SW5 = สลับที่มา เริ่มเก็บใหม่
            src, win = 1 - src, []
            if src == 1:
                base = read_value(1) or base
            show_source(w, src, base)
            w["ms"].text(" ")
            w["mm"].text(" ")
            beep("tap")
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: หน้าต่างมีค่า 25.0 สลับกับ 25.2 (เฉลี่ย 25.1 SD ราว 0.1) ค่าใหม่ 25.6 ได้ z เท่าไร ไฟติดไหม
# 2) ตั้ง WIN = 10 แล้วตั้ง WIN = 60 หมุน VR1 ค้างไว้ที่ใหม่ z กลับเข้าใกล้ 0 เร็วหรือช้าต่างกันอย่างไร
# 3) ตั้ง SD_MIN = (0.05, 0.01) ใช้ VR1 ไม่แตะลูกบิดเลย 15 วิ แล้วแตะเบา ๆ z เป็นอย่างไร
#    (ค่านิ่งสนิทหารด้วย SD เกือบ 0 ขยับนิดเดียวก็ "ผิดปกติ" ห้ามตั้ง 0 เพราะหารด้วย 0 โปรแกรมจะหยุด)
