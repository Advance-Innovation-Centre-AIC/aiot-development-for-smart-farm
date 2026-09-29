# cp4_02_style_at_runtime.py - หลักการ 4.2: สไตล์เปลี่ยนตามค่า ตอนโปรแกรมรัน (ชุดปุ่มปรับที่มีจริง)
#
# หลักการ  : การ์ดใบเดิม แต่งใหม่ระหว่างรันด้วยปุ่มปรับเท่าที่เฟิร์มแวร์มีจริง:
#            color() = สีตามโซนของค่า · prop(PROP_SCALE_NEEDLE_COLOR) = สีเข็มตามโซน · ticks() = ขีดหน้าปัด
#            show()/hide() = แถบเตือน · disable()/enable() = ปุ่มที่ห้ามกดตอนถังต่ำ · size() = ไฟเตือนเต้น
#            Panel ตั้งขอบได้ตอนสร้าง: color = พื้น, min = สีขอบ, max = มุมโค้ง, value = ความหนาขอบ
#            ส่วนชนิดฟอนต์ ความโปร่งใส และไล่สี ไม่มีให้ปรับบนเฟิร์มแวร์นี้
#            ทุกคำสั่งแต่งจอส่งเฉพาะตอนสถานะเปลี่ยน ไม่ส่งซ้ำทุกรอบ
# ลองเล่น  : หมุน VR1 (ความชื้นดิน) ลงช้า ๆ ดูสีเปลี่ยน เขียว -> ส้ม -> แดง แถบเตือนโผล่ ไฟเตือนเต้น
#            หมุน VR2 (น้ำในถัง) ลงต่ำกว่า 20 % ปุ่ม "เปิดปั๊ม" จะเป็นสีเทาและกดไม่ติด
# ของบนบอร์ด: ลูกบิด VR1, VR2 · ลำโพง (เสียงตอนโซนเปลี่ยนและตอนกดปุ่มบนจอ)
# ในฟาร์ม  : จอหน้าตู้ปั๊ม: สีบอกว่าต้องรีบไหม ปุ่มที่ใช้ไม่ได้ต้องดูออกว่าใช้ไม่ได้ ไม่ใช่กดแล้วเงียบ
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#            Emulator ยังไม่มี disable()/enable() ปุ่มบน Emulator จึงไม่เป็นสีเทา
#            แต่โปรแกรมยังเช็กถังซ้ำในสมองก่อนสั่ง ปั๊มจึงไม่เดินตอนถังต่ำทั้งสองที่

import pots
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
DRY_BELOW = 25       # ต่ำกว่านี้ = ดินแห้ง (แดง + แถบเตือน + ไฟเต้น)
WARN_BELOW = 40      # ต่ำกว่านี้ = เริ่มแห้ง (ส้ม)
TANK_MIN = 20        # น้ำในถังต่ำกว่านี้ (%) = ห้ามเปิดปั๊ม
TICKS_TOTAL = 11     # จำนวนขีดบนหน้าปัด
TICKS_MAJOR = 2      # มีตัวเลขทุกกี่ขีด (11 ขีด ทุก 2 = 0 20 40 60 80 100)
NEEDLE_LEN = 50      # ความยาวเข็มหน้าปัด (พิกเซล)
CARD_RADIUS = 18     # มุมโค้งของการ์ด (Panel max=)
CARD_BORDER_W = 3    # ความหนาขอบการ์ด (Panel value=)
LED_SMALL, LED_BIG = 36, 50   # ขนาดไฟเตือนตอนปกติ / ตอนเต้น
LOOP_MS = 30         # ดึงเหตุการณ์จากจอทุกกี่ ms (คิวบนจอจุ 16 เต็มแล้วทิ้งของใหม่)
TICK_MS = 500        # อ่านลูกบิดและแต่งจอทุกกี่ ms
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน (ไฟล์นี้ไม่ใช้ปุ่มบนบอร์ด)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD, COL_ALARM_BG = 0x171B22, 0x3A1416
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def soil_percent():
    return pots.read(0) * 100 // 4095      # VR1: 0 = แห้งสนิท, 100 = แฉะ


def tank_percent():
    return pots.read(1) * 100 // 4095      # VR2: น้ำในถัง 0-100 %


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def zone_of(v):
    # 0 = ปกติ, 1 = เริ่มแห้ง, 2 = แห้ง
    if v < DRY_BELOW:
        return 2
    if v < WARN_BELOW:
        return 1
    return 0


def pump_allowed(tank):
    # กฎความปลอดภัยอยู่ในสมอง ไม่ได้อยู่ที่สีปุ่ม: ปุ่มเทาเป็นแค่การบอกคน
    return tank >= TANK_MIN


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
ZONE_COL = (COL_OK, COL_WARN, COL_BAD)
ZONE_NAMES = ("ปกติ", "เริ่มแห้ง", "แห้ง!")


def set_needle(scale, value):
    # Scale วาดแค่ขีดกับตัวเลข ไม่มีเข็มในตัว เราสั่งเข็มเอง:
    # ความยาวเข็มอยู่ 16 บิตบน ค่าที่ชี้อยู่ 16 บิตล่าง
    scale.prop(ui.PROP_SCALE_NEEDLE, (NEEDLE_LEN << 16) | (int(value) & 0xFFFF))


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("สไตล์เปลี่ยนตามค่า", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("color() size() show() hide() disable() ticks() prop()", x=12, y=38,
             color=COL_DIM, value=16)
    w = {}
    # การ์ดดิน - Panel: color = พื้น, min = สีขอบ, max = มุมโค้ง, value = ความหนาขอบ (ตั้งได้ตอนสร้าง)
    w["card"] = ui.Panel(x=12, y=64, w=470, h=274, color=COL_CARD, min=COL_INFO,
                         max=CARD_RADIUS, value=CARD_BORDER_W)
    ui.Label("ความชื้นดิน", x=28, y=74, color=COL_INFO, value=16)
    gauge = ui.Scale(x=28, y=104, w=160, h=160, color=COL_TEXT, min=0, max=100)
    gauge.prop(ui.PROP_SCALE_MODE, ui.SCALE_ROUND_IN)
    gauge.ticks(TICKS_TOTAL, TICKS_MAJOR)     # ticks(ทั้งหมด, มีเลขทุกกี่ขีด)
    w["gauge"] = gauge
    w["num"] = ui.Label("-- %", x=210, y=104, color=COL_TEXT, value=28)
    w["bar"] = ui.Bar(x=210, y=156, w=256, h=22, min=0, max=100, value=0)
    w["zone"] = ui.Label(" ", x=210, y=192, color=COL_TEXT, value=20)
    w["led"] = ui.Led(x=210, y=236, w=LED_SMALL, h=LED_SMALL, color=COL_BAD, value=1)   # ไฟเต้นด้วย size()
    ui.Label("ปรับไม่ได้: ฟอนต์ ความโปร่งใส ไล่สี", x=28, y=308, color=COL_DIM, value=14)
    # แถบเตือน: สร้างไว้ก่อนแล้วซ่อน โผล่ด้วย show() ตอนดินแห้ง
    banner = ui.Panel(x=494, y=64, w=286, h=56, color=COL_BAD, min=COL_BAD, max=8, value=1)
    btxt = ui.Label("ดินแห้ง! รดน้ำ", x=510, y=76, color=COL_TEXT, value=24)
    banner.hide()
    btxt.hide()
    w["banner"] = (banner, btxt)
    w["tank"] = ui.Label(" ", x=494, y=140, color=COL_INFO, value=16)      # น้ำในถัง (VR2)
    w["tank_bar"] = ui.Bar(x=494, y=172, w=286, h=18, min=0, max=100, value=0)
    w["btn"] = ui.Button("เปิดปั๊ม", x=494, y=236, w=286, h=56, color=COL_INFO)
    w["note"] = ui.Label(" ", x=494, y=302, color=COL_DIM, value=16)
    w["help"] = ui.Label("VR1 = ดิน  VR2 = ถัง  แตะปุ่มได้", x=12, y=352,
                         color=COL_DIM, value=16)
    ui.poll()
    return w


def restyle(w, z):
    # แต่งการ์ดตามโซน: สีตัวเลข สีแถบ สีเข็ม พื้นการ์ด และแถบเตือน (เรียกเฉพาะตอนโซนเปลี่ยน)
    c = ZONE_COL[z]
    w["num"].color(c)
    w["bar"].color(c)
    w["gauge"].prop(ui.PROP_SCALE_NEEDLE_COLOR, c)
    w["zone"].color(c)
    w["zone"].text(ZONE_NAMES[z])
    w["card"].color(COL_ALARM_BG if z == 2 else COL_CARD)   # color() ของ Panel = สีพื้น
    for o in w["banner"]:
        if z == 2:
            o.show()
        else:
            o.hide()


# ---- 6) โปรแกรมหลัก ----
def main():
    w = build_screen()
    shown = zone = locked = tank_shown = None
    big = False
    runs = tank = 0
    t_show = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        evs = ui.poll()                                # 0) เหตุการณ์จากจอ: ดึงจนคิวว่าง (ครั้งละ <= 8)
        while evs:
            for ev in evs:                             # เช็กทั้ง id และ type
                if ev["handle"] == w["btn"].id() and ev["type"] == "clicked":
                    if pump_allowed(tank):             # สมองเช็กถังซ้ำ (Emulator กดได้แม้ถังต่ำ)
                        runs += 1
                        beep("tap")
                        w["note"].text("สั่ง %d ครั้ง" % runs)
                    else:
                        beep("bad")
            evs = ui.poll()
        now = time.ticks_ms()
        if time.ticks_diff(now, t_show) >= TICK_MS:
            t_show = now
            v, tank = soil_percent(), tank_percent()   # 1) อ่าน
            if v != shown:                             # 2) แต่งจอเฉพาะที่เปลี่ยน
                w["num"].text("%d %%" % v)
                w["bar"].value(v)
                set_needle(w["gauge"], v)
                shown = v
            z = zone_of(v)
            if z != zone:
                if zone is not None and z > zone:
                    beep("bad")                      # แย่ลง = เสียงเตือน
                elif zone is not None:
                    beep("start")                      # ดีขึ้น = เสียงโอเค
                restyle(w, z)
                zone = z
            if tank != tank_shown:
                w["tank_bar"].value(tank)
                w["tank"].text("น้ำในถัง %d %%" % tank)
                tank_shown = tank
            lk = not pump_allowed(tank)
            if lk != locked:                           # ถังต่ำ = ปุ่มเทาและเงียบ (disable) ถังพอ = enable
                try:
                    w["btn"].disable() if lk else w["btn"].enable()
                except AttributeError:
                    pass                               # Emulator ยังไม่มี disable()/enable()
                w["note"].color(COL_BAD if lk else COL_DIM)
                w["note"].text("ถังต่ำ: ห้ามกด" if lk else "ถังพอ: กดได้")
                locked = lk
            b = (not big) if z == 2 else False         # ไฟเต้น: ตอนแห้ง สลับใหญ่/เล็กทุกรอบ
            if b != big:                               # size() เฉพาะตอนขนาดเปลี่ยน
                s = LED_BIG if b else LED_SMALL
                w["led"].size(s, s)
                big = b
        time.sleep_ms(LOOP_MS)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เปลี่ยนเกณฑ์โซน WARN_BELOW / DRY_BELOW แล้วดูว่าสีเปลี่ยนที่ค่าไหน
#    ลองเปลี่ยนขนาดตัวเลขใหญ่ value=28 เป็น 20 (ขนาดที่มี: 14 16 20 24 28)
# 2) ตั้ง TICKS_TOTAL = 21 และ TICKS_MAJOR = 5 หน้าปัดจะมีตัวเลขกี่ตัว เดาก่อนรัน
#    แล้วลองตั้ง CARD_RADIUS = 0 กับ CARD_BORDER_W = 8 ดูหน้าตาการ์ด
# 3) ย้ายแถบถังน้ำด้วย w["tank_bar"].pos(494, 210) หลังสร้าง หรือเพิ่ม w["btn"].size(286, 80) ตอนถังพอ
#    แล้วคิดว่าการขยายปุ่มตอนใช้ได้ ช่วยคนหน้าตู้หรือกวนสายตา
