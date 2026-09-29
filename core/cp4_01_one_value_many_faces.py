# cp4_01_one_value_many_faces.py - หลักการ 4.1: ค่าเดียว หลายหน้าตา เลือก widget ตามชนิดข้อมูล
#
# หลักการ  : ตัวเลขตัวเดียว (VR1 = ความชื้นดิน 0-100 %) แสดงได้ 7 แบบ แต่ละแบบเหมาะกับข้อมูลคนละชนิด
#            ตัวเลข -> Label / Seg7 · เปอร์เซ็นต์ -> Bar / Arc · หน้าปัด -> Scale + เข็ม
#            สถานะ -> Led · ประวัติ -> Chart
#            widget สร้างครั้งเดียว (ข้อมูลอยู่ฝั่งจอ) แล้วส่งค่าใหม่ไปเฉพาะตอนค่าเปลี่ยนเกิน DEAD_BAND
# ลองเล่น  : หมุน VR1 ช้า ๆ ดูทั้ง 7 หน้าตาขยับพร้อมกัน แล้วถามตัวเองว่าอันไหนอ่านรู้เรื่องเร็วที่สุด
#            ปล่อย VR1 นิ่ง ๆ ตัวนับ "ส่งไปจอ" ต้องหยุดนับ (ค่าไม่เปลี่ยน = ไม่ต้องส่ง) แต่กราฟยังเดินต่อ
#            หมุนลงจนต่ำกว่า 25 % ไฟสถานะจะสว่างเต็มที่และมีเสียงเตือนหนึ่งครั้ง
# ของบนบอร์ด: ลูกบิด VR1 (pots.read(0)) · ลำโพง (เสียงสั้นตอนสถานะเปลี่ยน)
# ในฟาร์ม  : ค่าเดียวกันบนจอหน้าตู้ควบคุม: คนเดินผ่านดูไฟสถานะ คนดูแลอ่านตัวเลข
#            หัวหน้าฟาร์มดูกราฟย้อนหลังว่าดินแห้งเร็วแค่ไหน
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (ลูกบิด VR1 บนแผงจำลอง)
#            จอบน Emulator วาดด้วยเบราว์เซอร์ หน้าตาใกล้เคียงแต่ไม่เหมือนจอบอร์ดทุกพิกเซล

import pots
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
DEAD_BAND = 1        # ค่าต้องต่างจากที่โชว์อยู่อย่างน้อยเท่านี้ (%) จึงส่งไปจอ
DRY_BELOW = 25       # ต่ำกว่านี้ = ดินแห้ง (ไฟสถานะสว่างสุด)
WARN_BELOW = 40      # ต่ำกว่านี้ = เริ่มแห้ง
NEEDLE_LEN = 40      # ความยาวเข็มหน้าปัด (พิกเซล) สั้นกว่าวงตัวเลข จะได้ไม่บังเลข
TICK_MS = 500        # อ่านค่าและอัปเดตจอทุกกี่ ms (ถี่กว่านี้จอกะพริบ)
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1) (ไฟล์นี้ไม่ใช้ปุ่ม)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def soil_percent():
    # VR1 แทนเซนเซอร์ความชื้นดิน: 0 = แห้งสนิท, 100 = แฉะ
    return pots.read(0) * 100 // 4095


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def changed(v, shown):
    # ต้องส่งค่าใหม่ไปจอไหม: ยังไม่เคยโชว์ หรือห่างจากที่โชว์อยู่ตั้งแต่ DEAD_BAND ขึ้นไป
    return shown is None or abs(v - shown) >= DEAD_BAND


def zone_of(v):
    # สถานะของดิน: 0 = ชื้นพอ, 1 = เริ่มแห้ง, 2 = แห้ง
    if v < DRY_BELOW:
        return 2
    if v < WARN_BELOW:
        return 1
    return 0


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
ZONE_NAMES = ("ชื้นพอ", "เริ่มแห้ง", "แห้ง!")
BRIGHT = (80, 170, 255)      # ความสว่าง Led ต่อสถานะ (เฟิร์มแวร์ให้ต่ำสุดราว 80 ไฟจึงไม่ดับมืด)


def card(x, title, caption):
    # การ์ดหนึ่งใบต่อหนึ่งหน้าตา: หัวเรื่อง = ชื่อ widget, บรรทัดล่าง = ชนิดข้อมูลที่เหมาะ
    ui.Panel(x=x, y=64, w=186, h=186, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=70, color=COL_INFO, value=16)
    ui.Label(caption, x=x + 12, y=226, color=COL_DIM, value=14)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def set_needle(scale, value):
    # Scale วาดแค่ขีดกับตัวเลข ไม่มีเข็มในตัว เราสั่งเข็มเอง:
    # ความยาวเข็มอยู่ 16 บิตบน ค่าที่ชี้อยู่ 16 บิตล่าง
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ค่าเดียว หลายหน้าตา", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("VR1 = ความชื้นดิน 0-100 %  เลือก widget ตามชนิดข้อมูล", x=12, y=38,
             color=COL_DIM, value=16)
    w = {}
    card(12, "Label + Seg7", "ตัวเลข: อ่านค่าแม่น")
    w["num"] = ui.Label("-- %", x=24, y=100, color=COL_TEXT, value=28)
    w["seg"] = ui.Seg7(text="0", x=24, y=156, w=160, h=40, color=COL_INFO)
    card(206, "Bar + Arc", "เปอร์เซ็นต์: เต็มแค่ไหน")
    w["bar"] = ui.Bar(x=218, y=100, w=162, h=20, min=0, max=100, value=0)
    w["bar"].color(COL_INFO)           # สีของ Bar ต้องตั้งหลังสร้าง
    w["arc"] = ui.Arc(x=254, y=130, w=90, h=90, min=0, max=100, value=0)
    w["arc"].color(COL_INFO)
    card(400, "Scale + เข็ม", "หน้าปัด: อยู่ช่วงไหน")
    gauge = ui.Scale(x=430, y=94, w=126, h=126, color=COL_TEXT, min=0, max=100)
    gauge.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)   # หน้าปัดกลม ตัวเลขอยู่ด้านใน
    gauge.ticks(11, 2)                # 11 ขีด มีเลขทุก 2 ขีด = 0 20 40 60 80 100
    gauge.prop(ui.PROP_SCALE_NEEDLE_COLOR, COL_WARN)
    w["gauge"] = gauge
    card(594, "Led", "สถานะ: ดีหรือไม่ดี")
    w["led"] = ui.Led(x=656, y=104, w=60, h=60, color=COL_BAD, value=1)
    w["zone"] = ui.Label(" ", x=606, y=184, color=COL_TEXT, value=20)
    w["chart"] = line_chart(12, 262, 400, 76, 0, 100, COL_INFO)
    ui.Label("Chart = ประวัติ: ขึ้นหรือลง", x=424, y=266, color=COL_DIM, value=14)
    w["sent"] = ui.Label(" ", x=424, y=296, color=COL_TEXT, value=16)
    w["help"] = ui.Label("หมุน VR1 ช้า ๆ  ปล่อยนิ่ง = ไม่ต้องส่งค่าไปจอ", x=12, y=352,
                         color=COL_DIM, value=16)
    ui.poll()
    return w


def show_value(w, v):
    # หกหน้าตาของค่าเดียว (เรียกเฉพาะตอนค่าเปลี่ยน)
    w["num"].text("%d %%" % v)
    w["seg"].text(str(v))
    w["bar"].value(v)
    w["arc"].value(v)
    set_needle(w["gauge"], v)


def show_zone(w, z):
    # ไฟสถานะ: สว่างมาก = ต้องสนใจ (เรียกเฉพาะตอนสถานะเปลี่ยน)
    w["led"].prop(ui.PROP_LED_BRIGHTNESS, BRIGHT[z])
    w["zone"].text(ZONE_NAMES[z])
    w["zone"].color((COL_OK, COL_WARN, COL_BAD)[z])


# ---- 6) โปรแกรมหลัก ----
def main():
    w = build_screen()
    shown = zone = None
    reads = sent = 0
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        v = soil_percent()                             # 1) อ่าน
        reads += 1
        if changed(v, shown):                          # 2) ตัดสิน: เปลี่ยนพอจะส่งไหม
            show_value(w, v)                           # 3) โชว์ เฉพาะตอนเปลี่ยน
            shown = v
            sent += 1
        z = zone_of(v)
        if z != zone:
            if zone is not None and z > zone:
                beep("bad")                          # แย่ลง = เสียงเตือน (alert)
            elif zone is not None:
                beep("start")                          # ดีขึ้น = เสียงโอเค (ok)
            show_zone(w, z)
            zone = z
        w["chart"].set_next(0, v)                      # กราฟคือเวลา จึงเติมจุดทุกรอบ แม้ค่าไม่เปลี่ยน
        w["sent"].text("อ่าน %d ครั้ง ส่งไปจอ %d ครั้ง" % (reads, sent))
        while ui.poll():                               # ระบายคิวเหตุการณ์ให้ว่าง (ไฟล์นี้ไม่ได้ใช้ แต่คิวเต็มแล้วจะทิ้งของใหม่)
            pass
        time.sleep_ms(TICK_MS)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: ตั้ง DEAD_BAND = 5 แล้วหมุน VR1 ช้า ๆ ตัวนับ "ส่งไปจอ" จะโตเร็วขึ้นหรือช้าลง
#    ตัวเลขบนจอจะกระโดดทีละเท่าไร และกราฟยังละเอียดเหมือนเดิมไหม (เพราะอะไร)
# 2) เปลี่ยน WARN_BELOW และ DRY_BELOW แล้วดูว่าไฟสถานะเปลี่ยนที่ค่าไหน
#    ลองเปลี่ยนขนาดตัวเลขใหญ่ value=28 เป็น 20 (ขนาดที่มี: 14 16 20 24 28)
# 3) ย้ายการ์ด Led ไปไว้ซ้ายสุดด้วย pos() หลังสร้าง แล้วถามเพื่อนว่าอ่านสถานะเร็วขึ้นไหม
