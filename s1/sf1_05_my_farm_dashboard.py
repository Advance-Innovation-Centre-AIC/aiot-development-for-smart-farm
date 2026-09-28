# sf1_05_my_farm_dashboard.py - ภารกิจกลุ่ม: แผงควบคุมฟาร์มของเรา
#
# ภารกิจ   : รวมสิ่งที่ทำมาทั้งคาบเป็นแผงเดียว: อากาศในโรงเรือน + ความชื้นดิน (VR1)
#            + ปั๊มอัตโนมัติ + คะแนนสุขภาพฟาร์ม แล้ว "แต่งให้เป็นฟาร์มของกลุ่มคุณ"
# สิ่งที่ต้องแก้ : ทุกบรรทัดที่มีคำว่า TODO (ชื่อฟาร์ม พืช เกณฑ์ ค่าชดเชย กฎ)
# ลองเล่น  : เป่าลม จับบอร์ด หมุน VR1 แล้วดูคะแนนสุขภาพฟาร์มเปลี่ยน
#            แตะแถบด้านบนเพื่อสลับหน้า: ภาพรวม / กราฟ · เหตุการณ์ล่าสุดขึ้นใต้คะแนน (ปั๊มเปิด-ปิด, ฟาร์มแย่ลง-ดีขึ้น)
# ของบนบอร์ดที่ใช้ : SHT40, ลูกบิด VR1, ไฟ RGB_BLUE = ปั๊ม, SW5 (ปุ่มล่าง) กดค้าง = รดน้ำเอง,
#            SW6 (ปุ่มบน) = สลับจอไฟ RGB ระหว่างคะแนนสุขภาพกับอุณหภูมิ
#            ลำโพงมีเสียงตอนปั๊มเปิด/ปิด และตอนฟาร์มแย่ลง/ดีขึ้น
# บนจอ     : แท็บหลายหน้า (Tabview), วงแหวนคะแนน (Arc), ไฟปั๊ม (Led),
#            กราฟ 2 เส้น (Chart)
# ในงานจริง : ถ้าฟาร์มคุณมีเซนเซอร์ไร้สายและ PLC ต่อ Wi-Fi บอร์ดนี้คือแผงควบคุมกลาง (Smart IoT Gateway, คาบ 2)
# ส่งงาน    : ถ่ายรูปจอตอนคะแนนสูงสุดและต่ำสุด แนบในใบงาน
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator
# หน่วยความจำ: ไฟล์นี้ใหญ่สุดในคาบ บอร์ดเหลือ RAM ให้โปรแกรมไม่มาก ถ้าเพิ่มของแล้วขึ้น MemoryError
#            ให้ตัดของที่ไม่ใช้ออกก่อน (คอมเมนต์ไม่กินหน่วยความจำ โค้ดกิน)

import buttons
import gpio
import math
import pots
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
# ชื่อฟาร์ม + พืช รวมกันยาวได้ราว 40 ตัวอักษรไทย (จอรับได้ 126 ไบต์ ไทยตัวละ 3 ไบต์)
FARM_NAME = "ฟาร์มกลุ่มที่ ?"   # TODO: ตั้งชื่อฟาร์มของกลุ่ม
CROP = "มะเขือเทศ"             # TODO: พืชหรือสัตว์ที่ดูแล
T_LO, T_HI = 20, 30            # TODO: ช่วงอุณหภูมิที่เหมาะ (C)
H_LO, H_HI = 60, 80            # TODO: ช่วงความชื้นอากาศที่เหมาะ (%)
SOIL_MIN = 40                  # TODO: ดินแห้งกว่านี้ให้รดน้ำ (%)
SOIL_HYST = 5                  # ช่องกันกระพือของปั๊ม (%) เหมือน sf1_03
TEMP_OFFSET = 0.0    # TODO: บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์ในห้อง (หรืออุณหภูมิที่ผู้สอนประกาศ) แล้วใส่ค่าชดเชย เช่น -9.5
                     # (ห้องแอร์ปกติ ~25-28 C) ใช้ค่าเดียวกับที่กลุ่มหาได้ใน sf1_01
                     # ตั้งแล้ว ความชื้นจะถูกแปลงเป็นของห้องให้เองด้วย (ดู room_humidity)
HUM_FIX = True       # แปลงความชื้นเป็นของห้อง (ดู room_humidity) ถ้าเทียบไฮโกรมิเตอร์ในห้องแล้วสูงเกินจริง ให้ตั้ง False
RUN_MS = 300000
TICK_MS = 500
TAB_BAR_H = 44       # ความสูงแถบแท็บ (พิกเซล) ต้องสูงพอให้นิ้วแตะได้
ALERT_GAP_MS = 3000  # ฟาร์มเปลี่ยนโซน: เตือน/จดได้ไม่ถี่กว่าทุก 3 วินาที (กันกระพือตอนค่าอยู่ตรงขอบ)
BTN_NAMES = ("SW5", "SW6")   # ชื่อที่พิมพ์บนบอร์ด: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
ZONE_COLORS = (COL_OK, COL_WARN, COL_BAD)
MATRIX_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)


# ---- 2) ฮาร์ดแวร์ ----
def read_climate():
    """คืน (อุณหภูมิ, ความชื้น) ของห้อง (ชดเชยแล้ว) ถ้าอ่านไม่ได้คืน (None, None)"""
    for _ in range(3):                  # อ่านพลาดได้บางจังหวะ (บัสไม่ว่าง) จึงลองซ้ำ
        try:
            t_raw = sensors.sht40.temperature()
            h = sensors.sht40.humidity()
            break
        except Exception:
            time.sleep_ms(20)
    else:
        return None, None
    t = t_raw + TEMP_OFFSET
    if TEMP_OFFSET != 0 and HUM_FIX:
        h = room_humidity(h, t_raw, t)
    return t, h


def led_named(name):
    """หา LED ด้วยชื่อ ไม่ใช่เลข: บน Dev Kit ดวง LED1/LED2 (เลข 0, 1) อยู่บน SoM
    มองไม่เห็น ดวงที่เห็นคือ RGB_RED / RGB_GREEN / RGB_BLUE"""
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def set_pump(pump, running):
    if pump is not None:
        pump.on() if running else pump.off()


class Button:
    """ปุ่มบนฐานบอร์ด (0 = SW5 ปุ่มล่าง, 1 = SW6 ปุ่มบน) ที่ไม่พลาดการกดสั้น ๆ
    เฟิร์มแวร์กรองสัญญาณสั่น: ต้องอ่านเห็น "กด" สองครั้งห่างกันเกิน 50 ms จึงนับว่ากดจริง
    เราจึงอ่านปุ่มบ่อย ๆ ระหว่างรอ (ดู wait_ms) แล้วจำไว้ว่า "เพิ่งถูกกด" """

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
        """True ครั้งเดียวต่อการกดหนึ่งครั้ง (กดค้างไว้ก็ไม่นับซ้ำ)"""
        fired = self.clicked
        self.clicked = False
        return fired


def wait_ms(ms, btns):
    """รอ ms มิลลิวินาที แต่ระหว่างรอก็อ่านปุ่มทุก 20 ms เพื่อไม่พลาดการกดสั้น ๆ"""
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


def matrix_show(show_temp, t, score, zone, shown):
    """จอไฟ RGB: คะแนนสุขภาพ (สีตามโซน) หรืออุณหภูมิ (สีฟ้า) วาดใหม่เฉพาะตอนเปลี่ยน"""
    if show_temp and t is not None:
        want = (int(t + 0.5), rgbmatrix.CYAN)
    else:
        want = (score, MATRIX_COLORS[zone])
    if want != shown:
        rgbmatrix.score(want[0], want[1])
    return want


# ---- 3) สมอง (ตัดสินใจ) ----
def pump_decision(pump_on, soil):
    """กฎปั๊มแบบมีช่องกันกระพือ: เปิดเมื่อดินแห้งกว่า SOIL_MIN ปิดเมื่อชื้นเกิน SOIL_MIN + SOIL_HYST"""
    if not pump_on and soil < SOIL_MIN:
        return True
    if pump_on and soil > SOIL_MIN + SOIL_HYST:
        return False
    return pump_on


def room_humidity(h_raw, t_raw, t_room):
    """อากาศอุ่นขึ้นรอบเซนเซอร์ ความชื้นสัมพัทธ์จึงอ่านได้ต่ำกว่าห้อง
    ไอน้ำเท่าเดิมแต่ห้องเย็นกว่า จึงคูณด้วยอัตราส่วนความดันไออิ่มตัว (สูตร Magnus)"""
    def es(t):
        return 6.112 * math.exp(17.62 * t / (243.12 + t))
    return min(100.0, h_raw * es(t_raw) / es(t_room))


def health(t, h, soil):
    """คะแนนสุขภาพฟาร์ม 0-100 (TODO: ปรับน้ำหนักตามความสำคัญของงานกลุ่ม)"""
    if t is None or h is None:
        return 0
    score = 100
    if t < T_LO or t > T_HI:
        score -= 35
    if h < H_LO or h > H_HI:
        score -= 25
    if soil < SOIL_MIN:
        score -= 40
    return max(0, score)


def zone_of(score):
    """0 = ดี (80 ขึ้นไป), 1 = พอใช้, 2 = แย่ (ต่ำกว่า 50)"""
    if score >= 80:
        return 0
    return 1 if score >= 50 else 2


def advice_parts(t, h, soil):
    """รายการคำแนะนำจากสถานะตอนนี้ (TODO: เพิ่มคำแนะนำของกลุ่มคุณเอง)"""
    tips = []
    if t is not None and t > T_HI:
        tips.append("เปิดพัดลม/พ่นหมอก")
    if t is not None and t < T_LO:
        tips.append("ปิดม่านกันลม")
    if h is not None and h < H_LO:
        tips.append("เพิ่มความชื้น")
    if h is not None and h > H_HI:
        tips.append("เปิดระบายอากาศ")
    if soil < SOIL_MIN:
        tips.append("กำลังรดน้ำ")
    return tips


# ---- 4) หน้าจอ ----
def line_chart(x, y, w, h, lo, hi, color, parent=None):
    """กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)"""
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_overview_tab(w, tab):
    """หน้า "ภาพรวม" (พิกัดนับจากมุมซ้ายบนของหน้าแท็บ ไม่ใช่ของจอ)"""
    w["arc"] = ui.Arc(x=0, y=0, w=150, h=150, min=0, max=100, value=0, parent=tab)
    w["score"] = ui.Label("0", x=50, y=54, color=COL_TEXT, value=28, parent=tab)
    ui.Label("สุขภาพฟาร์ม", x=20, y=156, color=COL_INFO, value=16, parent=tab)
    w["air"] = ui.Label("อากาศ -- C  -- %RH", x=170, y=0, color=COL_TEXT, value=24, parent=tab)
    ui.Label("ความชื้นดิน (VR1)", x=170, y=40, color=COL_INFO, value=16, parent=tab)
    w["bar_soil"] = ui.Bar(x=170, y=66, w=360, h=24, min=0, max=100, value=0, parent=tab)
    w["lbl_soil"] = ui.Label("-- %", x=544, y=62, color=COL_TEXT, value=20, parent=tab)
    w["led"] = ui.Led(x=170, y=104, w=32, h=32, color=COL_INFO, value=0, parent=tab)
    w["lbl_pump"] = ui.Label("ปั๊มน้ำ: ปิด", x=214, y=108, color=COL_DIM, value=20, parent=tab)
    w["event"] = ui.Label(BTN_NAMES[1] + " (บน) = สลับจอไฟ RGB", x=170, y=156, color=COL_DIM, value=16, parent=tab)


def build_screen():
    """สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง"""
    ui.screen()
    time.sleep_ms(200)
    ui.Label(FARM_NAME + " - " + CROP, x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    # Tabview กินพื้นที่แค่ที่เราให้ จบที่ y=338 เพราะมุมขวาล่างเป็นปุ่ม Console ของเฟิร์มแวร์
    tabs = ui.Tabview(x=12, y=44, w=768, h=294, value=TAB_BAR_H)
    build_overview_tab(w, tabs.add_tab("ภาพรวม"))
    # หน้า "กราฟ": 2 เส้น สีตรงกับคำอธิบายด้านขวา
    tab = tabs.add_tab("กราฟ")
    w["chart"] = line_chart(0, 0, 400, 180, 0, 100, COL_WARN, tab)
    w["s_soil"] = w["chart"].add_series(COL_OK)
    ui.Label("ส้ม = อุณหภูมิ\nเขียว = ชื้นดิน", x=420, y=10, color=COL_TEXT, value=16, parent=tab)
    # คำแนะนำอยู่นอกแท็บ จึงเห็นได้ทุกหน้า - ของที่ต้องเห็นตลอดห้ามซ่อนในแท็บ
    w["advice"] = ui.Label(" ", x=12, y=352, color=COL_WARN, value=20)
    ui.poll()
    return w


def note_event(w, sec, what, score, sound):
    """มีเหตุการณ์: เสียงหนึ่งครั้ง + ขึ้นบรรทัด "ล่าสุด" ใต้คะแนน (เรียกเฉพาะตอนเกิดเหตุ ไม่ใช่ทุกรอบ)"""
    ui.sfx(sound)
    w["event"].color(COL_TEXT)
    w["event"].text("วินาที %d: %s (คะแนน %d)" % (sec, what, score))


def show(w, t, h, soil, running, score, zone):
    w["arc"].value(score)
    w["arc"].color(ZONE_COLORS[zone])
    w["score"].text(str(score))
    w["air"].text("อากาศ " + ("--" if t is None else "%.1f" % t) + " C  " +
                  ("--" if h is None else "%.0f" % h) + " %RH")
    w["bar_soil"].value(soil)
    w["bar_soil"].color(COL_BAD if soil < SOIL_MIN else COL_INFO)
    w["lbl_soil"].text(str(soil) + " %")
    w["led"].value(1 if running else 0)             # Led: 0 = หรี่ (ไม่ดับมืด)
    w["lbl_pump"].text("ปั๊มน้ำ: เปิด" if running else "ปั๊มน้ำ: ปิด")
    w["lbl_pump"].color(COL_OK if running else COL_DIM)
    if t is not None:
        w["chart"].set_next(0, int(max(0, min(100, t))))   # เส้นส้ม (ชุด 0)
    w["chart"].set_next(w["s_soil"], soil)
    tips = advice_parts(t, h, soil)
    more = " (+%d)" % (len(tips) - 1) if len(tips) > 1 else ""      # จอรับได้ 126 ไบต์ จึงโชว์ข้อแรกข้อเดียว
    w["advice"].text("แนะนำ: " + (tips[0] + more if tips else "ทุกอย่างปกติ"))
    w["advice"].color(COL_WARN if tips else COL_OK)


# ---- 5) โปรแกรมหลัก ----
def main():
    w = build_screen()
    pump = led_named("RGB_BLUE")   # ไฟสีฟ้าบนบอร์ด = ปั๊มน้ำ
    sw5, sw6 = Button(0), Button(1)
    pump_on = running = show_temp = False   # show_temp: จอไฟ RGB โชว์อุณหภูมิแทนคะแนน
    shown = zone_was = None
    last_alert = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        sec = time.ticks_diff(time.ticks_ms(), t0) // 1000
        t, h = read_climate()                                 # 1) อ่าน
        soil = pots.read(0) * 100 // 4095                     # VR1: 0 = แห้งสนิท, 100 = แฉะ
        score = health(t, h, soil)                            # 2) คิด
        zone = zone_of(score)
        pump_on = pump_decision(pump_on, soil)
        now_running = pump_on or sw5.down                     # กฎอัตโนมัติ หรือกด SW5 (ปุ่มล่าง) ค้าง
        if now_running != running:                           # 3) ทำ เฉพาะตอนเปลี่ยน
            note_event(w, sec, "ปั๊มเปิด" if now_running else "ปั๊มปิด", score,
                       ui.SFX_UI_START if now_running else ui.SFX_UI_BACK)
            set_pump(pump, now_running)
        running = now_running
        if zone_was is None:
            zone_was = zone
        elif zone != zone_was and time.ticks_diff(time.ticks_ms(), last_alert) > ALERT_GAP_MS:
            worse = zone > zone_was                           # เปลี่ยนโซนจริง ไม่ใช่กระพือ
            note_event(w, sec, "ฟาร์มแย่ลง" if worse else "ฟาร์มดีขึ้น", score,
                       ui.SFX_UI_DENY if worse else ui.SFX_PONG_WIN)
            zone_was = zone
            last_alert = time.ticks_ms()
        if sw6.pressed_now():                                 # SW6 (ปุ่มบน) = สลับจอไฟ RGB
            show_temp = not show_temp
            ui.sfx(ui.SFX_UI_MOVE)
        shown = matrix_show(show_temp, t, score, zone, shown)
        show(w, t, h, soil, running, score, zone)             # 4) โชว์
        ui.poll()          # แตะแท็บ LVGL สลับหน้าให้เอง เราแค่ดึงเหตุการณ์ออกจากคิว
        wait_ms(TICK_MS, (sw5, sw6))       # รอ แต่ยังคอยฟังปุ่ม

    set_pump(pump, False)                  # จบรอบ: ปิดปั๊ม ล้างจอไฟ RGB บอกวิธีเล่นใหม่
    try:
        rgbmatrix.clear()
    except OSError:
        pass
    w["advice"].color(COL_WARN)
    w["advice"].text("จบรอบ - กด Program to Device เพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ (ภารกิจกลุ่ม) -----
# 1) แก้ทุก TODO ให้เป็นฟาร์มของกลุ่ม (รวม TEMP_OFFSET ที่วัดได้จาก sf1_01)
# 2) เพิ่มของลงหน้า "ภาพรวม" จากไฟล์ก่อนหน้า: มุมเอียง (sf1_04) หรือความกดอากาศ (sf1_01)
#    หรือใช้ลูกบิดที่เหลือ: VR3 = น้ำในถัง (จาก sf1_03), VR4 = แสงแดด/เวลาของวัน
#    (ไฟล์นี้ใกล้เพดานหน่วยความจำของบอร์ดแล้ว เพิ่มทีละนิดแล้วลองรัน ถ้า MemoryError ให้ตัดของอื่นออก)
# 3) เพิ่มเหตุการณ์ของกลุ่ม เช่น note_event(w, sec, "ร้อนเกิน", score, ui.SFX_UI_DENY)
#    ตอนอุณหภูมิเพิ่งเกิน T_HI (จดเฉพาะตอน "เพิ่งเกิน" ไม่ใช่ทุกรอบ)
# 4) คิดชื่อโปรเจกต์ของกลุ่ม: ฟาร์มนี้ยังขาดอะไร ถ้าต่ออินเทอร์เน็ตได้จะทำอะไรเพิ่ม
