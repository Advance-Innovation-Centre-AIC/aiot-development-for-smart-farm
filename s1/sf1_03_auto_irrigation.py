# sf1_03_auto_irrigation.py - ระบบรดน้ำอัตโนมัติ (จำลอง)
#
# ภารกิจ   : ใช้ลูกบิด VR1 แทน "เซนเซอร์ความชื้นดิน" และลูกบิด VR2 แทน "ปุ่มตั้งเกณฑ์"
#            ถ้าดินแห้งกว่าเกณฑ์ -> เปิดปั๊ม (ไฟบนบอร์ดติด) รดจนดินชื้นพอ -> ปิดปั๊ม
#            ลูกบิด VR3 = น้ำในถัง ถ้าถังเหลือน้อยกว่า 10 % ปั๊มจะไม่ยอมเดิน (กันปั๊มไหม้)
# ลองเล่น  : หมุน VR1 ลงช้า ๆ (ดินแห้งลง) ดูว่าปั๊มติดที่ค่าไหน แล้วหมุนกลับขึ้น
#            สังเกตว่าปั๊ม "ไม่สั่นไปมา" ที่ขอบเกณฑ์ เพราะเราใช้ hysteresis (ช่องกันกระพือ)
#            ดูในกราฟ: ปั๊มเปิดตอนเส้นฟ้าต่ำกว่าเส้นแดง และปิดตอนเส้นฟ้าสูงกว่าเส้นเขียว
#            แตะสวิตช์บนจอให้เป็น "มือ" แล้วกด SW5 (ปุ่มล่าง) หนึ่งครั้ง = รดน้ำเอง MANUAL_S วินาที (กดอีกครั้ง = หยุด)
# ของบนบอร์ดที่ใช้ : ลูกบิด VR1-VR3, ปุ่ม SW5 (ปุ่มล่าง) (กด = เริ่ม/หยุดรดน้ำเอง) และ SW6 (ปุ่มบน) (ล้างตัวนับ),
#            ไฟ RGB_BLUE บนบอร์ด = รีเลย์ปั๊มน้ำ, ลำโพง (ปั๊มเปิด/ปิด และถังหมด)
#            จอไฟ RGB: ซ้าย = ความชื้นดิน (เส้นแดง = เกณฑ์)  กลาง = น้ำไหล  ขวา = น้ำในถัง
# บนจอ     : แถบ + ไม้บรรทัด (Bar + Scale), ไฟปั๊ม (Led), ไฟ Led ติดตอนปั๊มเดิน,
#            สวิตช์อัตโนมัติ/มือ (Switch), กราฟความชื้นดินเทียบเกณฑ์ (Chart)
# แนวคิด    : ในฟาร์มจริง ปั๊มที่เปิด-ปิดถี่ ๆ จะพังเร็วและเปลืองไฟ
# ในงานจริง : VR1 = โหนดเซนเซอร์ความชื้นดินไร้สาย ส่งค่ามาทาง MQTT
#             VR3 = เซนเซอร์ระดับน้ำในถังไร้สาย
#             ไฟสีฟ้า "ปั๊ม" = PLC/รีเลย์ต่อ Wi-Fi ที่รับคำสั่งเปิดปั๊มทาง MQTT แล้วขับปั๊มจริง
#             Dev Kit ของเรา = Smart HMI ที่คิดและสั่งงาน  (คาบ 2 เราจะต่อของพวกนี้ผ่าน MQTT จริง)
# บอร์ด     : TESAIoT Dev Kit (ลูกบิด VR1-VR4 บนบอร์ดฐาน) และ BENTO Emulator (แผง TESAIoT DEV KIT)

import buttons
import gpio
import pots
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
HYST = 5             # ช่องกันกระพือ (%) ปั๊มปิดเมื่อชื้นเกิน เกณฑ์ + HYST
TANK_MIN = 10        # น้ำในถังต่ำกว่านี้ (%) ห้ามเดินปั๊ม
FLOW_L_PER_S = 0.5   # สมมติปั๊มจ่าย 0.5 ลิตร/วินาที
AUTO_AT_START = True # สวิตช์บนจอเริ่มที่ "อัตโนมัติ"
MANUAL_S = 10        # กด SW5 (ปุ่มล่าง) หนึ่งครั้ง = รดน้ำเองนานกี่วินาที (กดอีกครั้ง = หยุดก่อน)
BTN_NAMES = ("SW5", "SW6")   # ชื่อที่พิมพ์บนบอร์ด: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน
RUN_MS = 180000
TICK_MS = 500
CHART_EVERY = 1      # ใส่จุดในกราฟทุกกี่รอบ (1 = ทุก 0.2 วินาที) กราฟ 400 จุดจึงย้อนหลังได้ราว 80 วินาที

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def soil_percent():
    return pots.read(0) * 100 // 4095      # VR1: 0 = แห้งสนิท, 100 = แฉะ


def threshold_percent():
    return 20 + pots.read(1) * 50 // 4095  # VR2: ตั้งเกณฑ์ได้ 20-70 %


def tank_percent():
    return pots.read(2) * 100 // 4095      # VR3: น้ำในถัง 0-100 %


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


def set_pump(pump, running):
    # ไฟสีฟ้าบนบอร์ดแทน "รีเลย์ปั๊มน้ำ" (ฟาร์มจริงต่อรีเลย์ที่ขาเดียวกันนี้)
    if pump is None:
        return
    if running:
        pump.on()
    else:
        pump.off()


class Button:
    # ปุ่มบนฐานบอร์ด (0 = SW5 (ปุ่มล่าง), 1 = SW6 (ปุ่มบน)) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสัญญาณสั่น: ต้องอ่านเห็น "กด" สองครั้งห่างกันเกิน 50 ms จึงนับว่ากดจริง
    # ถ้าอ่านรอบละครั้ง (ทุกครึ่งวินาที) การกดแบบแตะจะหายไปเฉย ๆ
    # เราจึงอ่านปุ่มบ่อย ๆ ระหว่างรอ (ดู wait_ms) แล้วจำไว้ว่า "เพิ่งถูกกด"

    def __init__(self, index):
        self.index = index
        self.down = False       # ตอนนี้กดค้างอยู่ไหม
        self.clicked = False    # ถูกกดลงมาใหม่ ตั้งแต่ถามครั้งก่อนไหม

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        # True ครั้งเดียวต่อการกดหนึ่งครั้ง (กดค้างไว้ก็ไม่นับซ้ำ)
        fired = self.clicked
        self.clicked = False
        return fired


def wait_ms(ms, btns):
    # รอ ms มิลลิวินาที แต่ระหว่างรอก็อ่านปุ่มทุก 20 ms เพื่อไม่พลาดการกดสั้น ๆ
    t0 = time.ticks_ms()
    while True:
        for b in btns:
            b.sample()
        left = ms - time.ticks_diff(time.ticks_ms(), t0)
        if left <= 0:
            return
        time.sleep_ms(min(20, left))


def put(buf, x, y, c):
    # ตั้งสีจุด (x, y) ในเฟรม 64 ไบต์ของจอไฟ RGB (จุดละ 4 บิต)
    i = y * 8 + (x >> 1)
    if x & 1:
        buf[i] = (buf[i] & 0x0F) | (c << 4)
    else:
        buf[i] = (buf[i] & 0xF0) | c


def draw_farm(soil, th, tank, running):
    # วาดจอไฟ RGB ทั้งจอ: ซ้าย = ดิน, กลาง = น้ำไหล, ขวา = น้ำในถัง
    buf = bytearray(64)
    s_rows = soil * 8 // 100            # ความชื้นดิน 0-8 แถว (นับจากล่าง)
    t_rows = tank * 8 // 100            # น้ำในถัง 0-8 แถว
    th_row = 7 - min(7, th * 8 // 100)  # แถวของเส้นเกณฑ์
    for y in range(8):
        for x in range(7):
            if y >= 8 - s_rows:
                put(buf, x, y, rgbmatrix.BLUE if soil >= th else rgbmatrix.YELLOW)
        for x in range(11, 16):
            if y >= 8 - t_rows:
                put(buf, x, y, rgbmatrix.CYAN if tank >= TANK_MIN else rgbmatrix.RED)
        if running:
            put(buf, 8, y, rgbmatrix.GREEN)
            put(buf, 9, y, rgbmatrix.GREEN)
    for x in range(7):
        put(buf, x, th_row, rgbmatrix.RED)
    try:
        rgbmatrix.blit(buf)
    except OSError:
        pass                    # จอไฟ RGB ตอบไม่ทัน: ข้ามภาพนี้ไป ไม่ให้โปรแกรมหยุด


def matrix_update(soil, th, tank, running, drawn):
    # วาดจอไฟ RGB ใหม่เฉพาะตอนภาพจะเปลี่ยนจริง แล้วคืนภาพที่วาดอยู่
    frame = (soil * 8 // 100, th * 8 // 100, tank * 8 // 100, soil < th,
             tank >= TANK_MIN, running)
    if frame != drawn:
        draw_farm(soil, th, tank, running)
    return frame


# ---- 3) สมอง (ตัดสินใจ) ----
def pump_decision(pump_on, soil, th):
    # กฎปั๊มแบบมีช่องกันกระพือ (hysteresis):
    # เปิดเมื่อดินแห้งกว่าเกณฑ์, ปิดเมื่อชื้นเกิน เกณฑ์ + HYST, ระหว่างนั้นคงสถานะเดิม
    if not pump_on and soil < th:
        return True
    if pump_on and soil > th + HYST:
        return False
    return pump_on


def should_run(auto_wants, manual, tank_ok):
    # ปั๊มเดินจริง = (กฎอัตโนมัติสั่ง หรือ สั่งรดเองด้วย SW5 (ปุ่มล่าง)) และ น้ำในถังพอ
    return (auto_wants or manual) and tank_ok


# ---- 4) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทั้ง 5 ไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_soil_card(w):
    card(12, 64, 470, 166, "ความชื้นดิน (VR1 = โหนดไร้สาย)")
    w["bar_soil"] = ui.Bar(x=24, y=94, w=446, h=26, min=0, max=100, value=0)
    w["bar_soil"].color(COL_INFO)
    ruler = ui.Scale(x=24, y=122, w=446, h=34, color=COL_DIM, min=0, max=100)
    ruler.ticks(11, 2)          # ไม้บรรทัดใต้แถบ: เลข 0 20 40 60 80 100 (Scale ไม่มีเข็ม)
    w["lbl_soil"] = ui.Label("-- %", x=24, y=160, color=COL_TEXT, value=20)
    w["lbl_rule"] = ui.Label("", x=24, y=192, color=COL_WARN, value=16)


def build_pump_card(w):
    card(492, 64, 288, 166, "ปั๊มน้ำ (PLC ต่อ Wi-Fi)")
    w["led"] = ui.Led(x=504, y=94, w=36, h=36, color=COL_INFO, value=0)
    w["pump_lbl"] = ui.Label("ปิด", x=552, y=96, color=COL_DIM, value=28)
    w["switch"] = ui.Switch(x=504, y=142, w=64, h=32, value=1 if AUTO_AT_START else 0)
    w["mode"] = ui.Label("", x=578, y=146, color=COL_TEXT, value=16)
    w["stats"] = ui.Label("เปิด 0 ครั้ง  น้ำ 0.0 ลิตร", x=504, y=190, color=COL_DIM, value=16)


def build_chart(w):
    # กราฟ: ฟ้า (ชุด 0) = ความชื้นดิน, แดง = เกณฑ์เปิดปั๊ม, เขียว = เกณฑ์ปิดปั๊ม
    ui.Label("ฟ้า = ดิน   แดง = เกณฑ์เปิด   เขียว = เกณฑ์ปิด", x=12, y=236,
             color=COL_DIM, value=14)
    w["chart"] = line_chart(12, 258, 400, 80, 0, 100, COL_INFO)
    w["s_soil"] = 0
    w["s_on"] = w["chart"].add_series(COL_BAD)
    w["s_off"] = w["chart"].add_series(COL_OK)


def build_tank_card(w):
    card(422, 240, 358, 98, "น้ำในถัง (VR3 = เซนเซอร์ระดับน้ำไร้สาย)")
    w["bar_tank"] = ui.Bar(x=434, y=272, w=334, h=20, min=0, max=100, value=0)
    w["lbl_tank"] = ui.Label("-- %", x=434, y=300, color=COL_INFO, value=16)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ระบบรดน้ำอัตโนมัติ", x=12, y=6, color=COL_TEXT, value=24)
    # ในงานจริง ลูกบิดคือเซนเซอร์ไร้สาย และไฟสีฟ้าคือ PLC ที่รับคำสั่งผ่าน MQTT (คาบ 2)
    ui.Label("งานจริง: VR1, VR3 = เซนเซอร์ไร้สาย  ไฟปั๊ม = PLC (MQTT)", x=12, y=38,
             color=COL_DIM, value=16)
    w = {}
    build_soil_card(w)
    build_pump_card(w)
    build_chart(w)
    build_tank_card(w)
    w["help"] = ui.Label(BTN_NAMES[0] + " (ล่าง) = รดเอง/หยุด   " + BTN_NAMES[1] + " (บน) = ล้างตัวนับ",
                         x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def read_auto_switch(w, auto):
    # อ่านเหตุการณ์จากจอ: สวิตช์ส่ง toggled พร้อมค่า 1 = อัตโนมัติ, 0 = มือ
    for ev in ui.poll():
        if ev["handle"] == w["switch"].id() and ev["type"] == "toggled":
            auto = ev["value"] == 1
            ui.sfx(ui.SFX_UI_MOVE)
    return auto


def announce_pump(w, running):
    # ตอนปั๊มเพิ่งเปิด/ปิด (ไม่ใช่ทุกรอบ): เสียงหนึ่งครั้ง
    ui.sfx(ui.SFX_UI_START if running else ui.SFX_UI_BACK)


def show_soil(w, soil, th):
    dry = soil < th
    w["bar_soil"].value(soil)
    w["bar_soil"].color(COL_BAD if dry else COL_INFO)
    w["lbl_soil"].text(str(soil) + " %  " + ("ดินแห้ง!" if dry else "ดินชื้นพอ"))
    w["lbl_rule"].text("เปิดปั๊มเมื่อต่ำกว่า " + str(th) + " % (VR2)  ปิดเมื่อเกิน " +
                       str(th + HYST) + " %")


def show_pump(w, running, tank_ok, auto, runs, water_l):
    w["led"].value(1 if running else 0)             # Led: 0 = หรี่ (ไม่ดับมืด)
    w["pump_lbl"].text("เปิด" if running else ("ถังหมด" if not tank_ok else "ปิด"))
    w["pump_lbl"].color(COL_OK if running else (COL_BAD if not tank_ok else COL_DIM))
    w["mode"].text("อัตโนมัติ" if auto else "มือ: กด " + BTN_NAMES[0] + " รดน้ำ")
    w["stats"].text("เปิด " + str(runs) + " ครั้ง  น้ำ %.1f ลิตร" % water_l)


def show_tank(w, tank, tank_ok):
    w["bar_tank"].value(tank)
    w["bar_tank"].color(COL_INFO if tank_ok else COL_BAD)
    w["lbl_tank"].text(str(tank) + " %" + ("" if tank_ok else "  น้อยเกินไป!"))
    w["lbl_tank"].color(COL_INFO if tank_ok else COL_BAD)


def show_chart(w, soil, th):
    w["chart"].set_next(w["s_soil"], soil)
    w["chart"].set_next(w["s_on"], th)
    w["chart"].set_next(w["s_off"], min(100, th + HYST))


# ---- 5) โปรแกรมหลัก ----
def finish(w, pump, runs, water_l):
    # จบรอบ: ปิดปั๊ม ล้างจอไฟ RGB บอกวิธีเล่นใหม่ และพิมพ์สรุปลง Console
    set_pump(pump, False)
    try:
        rgbmatrix.clear()
    except OSError:
        pass
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()
    print("ปั๊มเปิด", runs, "ครั้ง ใช้น้ำ %.1f ลิตร" % water_l)


def main():
    w = build_screen()
    pump = led_named("RGB_BLUE")
    sw5, sw6 = Button(0), Button(1)   # SW5 = ปุ่มล่าง, SW6 = ปุ่มบน
    auto = AUTO_AT_START
    manual, manual_t0 = False, 0   # สั่งรดเองด้วย SW5 อยู่ไหม และเริ่มเมื่อไร
    pump_on = False       # สิ่งที่กฎอัตโนมัติอยากทำ (ตามความชื้นดิน)
    running = False       # ปั๊มเดินจริงไหม
    tank_was_ok = tank_percent() >= TANK_MIN   # ถังพร่องตั้งแต่เริ่ม = ไม่ส่งเสียงเตือนทันที
    runs, water_l, tick = 0, 0.0, 0
    drawn = None          # ภาพที่จอไฟ RGB วาดอยู่ (วาดใหม่เฉพาะตอนเปลี่ยน)
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        auto = read_auto_switch(w, auto)                              # 0) สวิตช์บนจอ
        soil, th, tank = soil_percent(), threshold_percent(), tank_percent()  # 1) อ่าน
        pump_on = pump_decision(pump_on, soil, th)                    # 2) ตัดสิน
        tank_ok = tank >= TANK_MIN
        if sw5.pressed_now():                                         # SW5 (ปุ่มล่าง) กดหนึ่งครั้ง = เริ่ม/หยุดรดเอง
            manual, manual_t0 = not manual, time.ticks_ms()
        if manual and time.ticks_diff(time.ticks_ms(), manual_t0) > MANUAL_S * 1000:
            manual = False                                            # ครบเวลาแล้วหยุดเอง กันลืมปิด
        now_running = should_run(auto and pump_on, manual, tank_ok)
        if now_running != running:                                    # 3) ทำ
            announce_pump(w, now_running)
            if now_running:
                runs += 1
        running = now_running
        if tank_was_ok and not tank_ok:
            ui.sfx(ui.SFX_GAME_OVER)                                  # ถังหมด!
        tank_was_ok = tank_ok
        if sw6.pressed_now():                                         # SW6 (ปุ่มบน) = ล้างตัวนับ
            runs, water_l = 0, 0.0
            ui.sfx(ui.SFX_UI_SELECT)
        if running:
            water_l += FLOW_L_PER_S * TICK_MS / 1000
        set_pump(pump, running)
        drawn = matrix_update(soil, th, tank, running, drawn)
        show_soil(w, soil, th)                                        # 4) โชว์
        show_pump(w, running, tank_ok, auto, runs, water_l)
        show_tank(w, tank, tank_ok)
        if tick % CHART_EVERY == 0:
            show_chart(w, soil, th)
        tick += 1
        wait_ms(TICK_MS, (sw5, sw6))       # รอ แต่ยังคอยฟังปุ่ม

    finish(w, pump, runs, water_l)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ตั้ง HYST = 0 แล้วหมุน VR1 ค้างไว้ตรงขอบเกณฑ์ นับว่าปั๊มเปิดกี่ครั้ง เทียบกับ HYST = 5
#    (ดูในกราฟ: HYST = 0 เส้นแดงกับเส้นเขียวทับกันเป็นเส้นเดียว)
# 2) เพิ่มกฎ "ห้ามรดน้ำตอนเที่ยง" โดยใช้ลูกบิด VR4 แทนเวลาของวัน (0-4095 = 0-24 นาฬิกา)
#    เขียนเป็นฟังก์ชันในส่วน 3) แล้วส่งผลเข้า should_run()
# 3) ให้น้ำในถังลดลงจริงตามน้ำที่ใช้ (ถัง 100 ลิตร) แทนการหมุน VR3 เอง
