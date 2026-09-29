# cp2_05_dew_point_guard.py - หลักการ 2.5: ฟีเจอร์จากฟิสิกส์ จุดน้ำค้าง (dew point) เตือนหยดน้ำเกาะ
#
# หลักการ  : จุดน้ำค้าง Td = อุณหภูมิที่อากาศก้อนนี้ต้องเย็นลงถึง ไอน้ำในอากาศจึงกลั่นเป็นหยดน้ำ
#            ถ้าอุณหภูมิอากาศ T ห่างจาก Td ไม่เกิน RISK_C = ผิวที่เย็นกว่าอากาศนิดเดียว (ใบพืช หลังคา) ก็เปียก
#            dsp.dew_point(T, RH) คำนวณด้วยสูตร Magnus ในภาษา C (คืนองศา C)
#            dsp.heat_index(T, RH) = อุณหภูมิที่ "รู้สึก" (C) · dsp.comfort_zone(T, RH) = หมวดความสบายของคน
#            (คืนคำภาษาอังกฤษ cold hot dry humid comfortable acceptable)
# ทำไม Td ใช้คู่ดิบ: บอร์ดอุ่นจากชิปของตัวเอง SHT40 จึงอ่าน T สูงไปและ RH ต่ำไป แต่ไอน้ำในอากาศก้อนนั้นเท่าเดิม
#            คู่ (T ดิบ, RH ดิบ) วัดที่จุดอุ่นจุดเดียวกัน จึงให้ Td ของอากาศได้โดยไม่ต้องชดเชย
#            นี่เป็นเหตุผลทางฟิสิกส์ ยังไม่ได้เทียบกับไฮโกรมิเตอร์อ้างอิงบนบอร์ดจริง
#            T ของอากาศ = T ดิบ + TEMP_OFFSET แล้วคำนวณ RH ของอากาศกลับจาก Td ด้วยสูตร Magnus ใน Python
# ลองเล่น  : หายใจรดเซนเซอร์ SHT40 (ความชื้นพุ่ง) ดูช่องห่าง T - Td แคบลงจนขึ้น "เสี่ยงหยดน้ำเกาะ"
#            ตั้ง TEMP_OFFSET ให้ตรงกับเทอร์โมมิเตอร์ในห้อง (เช่น -3.0) แล้วรันใหม่
#            ดู RH ของอากาศขยับห่างจาก RH ดิบ ขณะที่ Td เท่าเดิม
# ของบนบอร์ด: SHT40 (อุณหภูมิ + ความชื้น) · ลำโพง (ดังตอนเริ่มเสี่ยง / หายเสี่ยง) · จอไฟ RGB (ฟ้าทั้งจอ = เสี่ยง)
# ในฟาร์ม  : โรงเรือนผักหรือเห็ด ตอนเช้ามืดอากาศเย็นลงใกล้ Td หยดน้ำเกาะใบนาน ๆ เสี่ยงโรคเชื้อรา
#            ระบบจึงเปิดพัดลมหรือช่องระบายเมื่อ T - Td แคบ ไม่ใช่ดูแค่ RH สูง
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#            ใน Emulator อุณหภูมิและความชื้นเป็นค่าจำลองที่ตั้งเองได้ และไม่มีความร้อนจากตัวบอร์ด

import dsp
import math
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
RISK_C = 2.0         # T - Td ไม่เกินกี่องศา = เสี่ยงหยดน้ำเกาะ
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์ในห้อง แล้วใส่ค่าชดเชย เช่น -3.0
A, B = 17.27, 237.7  # ค่าคงที่สูตร Magnus ชุดเดียวกับที่ dsp.dew_point ใช้ในเฟิร์มแวร์
TICK_MS = 1000       # อ่าน SHT40 และอัปเดตจอทุก 1 วินาที
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_sht40():
    # คืน (T ดิบ, RH ดิบ) หรือ None ถ้าลองครบ 3 ครั้งแล้วยังอ่านไม่ได้ (บัสไม่ว่างบางจังหวะ)
    for _ in range(3):
        try:
            return sensors.sht40.temperature(), sensors.sht40.humidity()
        except Exception:
            time.sleep_ms(50)
    return None


def show_matrix(risk):
    # จอไฟ RGB: เสี่ยง = ฟ้าทั้งจอ ไม่เสี่ยง = ดับ (clear เพราะ fill รับเฉพาะสี 1-7)
    try:
        if risk:
            rgbmatrix.fill(rgbmatrix.BLUE)
        else:
            rgbmatrix.clear()
    except OSError:
        pass


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def rh_at(td, t):
    # RH (%) ของอากาศที่อุณหภูมิ t ซึ่งมีจุดน้ำค้าง td: อัตราส่วนความดันไออิ่มตัว es(td) / es(t)
    # es(x) = 6.112 exp(A x / (B + x)) เลข 6.112 หักกันเองจึงไม่ต้องใส่
    return min(100.0, 100 * math.exp(A * td / (B + td) - A * t / (B + t)))


def air(t_raw, rh_raw):
    # คืน (T อากาศ, Td, RH อากาศ): Td จากคู่ดิบ T อากาศจากค่าชดเชย RH อากาศคำนวณกลับจาก Td
    t = t_raw + TEMP_OFFSET
    td = dsp.dew_point(t_raw, rh_raw)
    return t, td, rh_at(td, t)


def at_risk(t, td):
    # เสี่ยงหยดน้ำเกาะเมื่ออากาศห่างจากจุดน้ำค้างไม่เกิน RISK_C
    return t - td <= RISK_C


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
    ui.Label("จุดน้ำค้าง: เตือนหยดน้ำเกาะ", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("เสี่ยงเมื่อ T - Td <= %.1f C" % RISK_C, x=12, y=38, color=COL_DIM, value=16)
    card(12, 64, 250, 120, "อากาศ T (C)")
    w = {"t": ui.Seg7(text="0", x=24, y=94, w=150, h=56, color=COL_WARN)}
    card(272, 64, 250, 120, "จุดน้ำค้าง Td (C)")
    w["td"] = ui.Seg7(text="0", x=284, y=94, w=150, h=56, color=COL_INFO)
    card(532, 64, 248, 120, "ห่าง T - Td")
    w["gap"] = ui.Seg7(text="0", x=544, y=94, w=130, h=56, color=COL_OK)
    w["led"] = ui.Led(x=700, y=102, w=40, h=40, color=COL_BAD, value=0)
    card(12, 194, 380, 144, "ความชื้น และฟีเจอร์อื่น")
    w["rh"] = ui.Label(" ", x=24, y=224, color=COL_TEXT, value=16)
    w["rh2"] = ui.Label(" ", x=24, y=252, color=COL_TEXT, value=16)
    w["hi"] = ui.Label(" ", x=24, y=280, color=COL_DIM, value=16)
    w["state"] = ui.Label(" ", x=24, y=308, color=COL_OK, value=16)
    ui.Label("ส้ม = T   ฟ้า = Td", x=422, y=194, color=COL_DIM, value=14)
    w["chart"] = line_chart(422, 216, 358, 122, 0, 50, COL_WARN)
    w["s_td"] = w["chart"].add_series(COL_INFO)
    w["help"] = ui.Label("หายใจรด SHT40 แล้วดูช่องห่าง T - Td", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, t_raw, rh_raw, t, td, rh, risk):
    w["t"].text("%.1f" % t)
    w["td"].text("%.1f" % td)
    w["gap"].text("%.1f" % (t - td))
    w["gap"].color(COL_BAD if risk else COL_OK)
    w["led"].value(1 if risk else 0)
    w["rh"].text("RH ดิบ %.0f %% (ที่ %.1f C)" % (rh_raw, t_raw))
    w["rh2"].text("RH อากาศ %.0f %% (คำนวณจาก Td)" % rh)
    w["hi"].text("รู้สึกเหมือน %.1f C  คน: %s" % (dsp.heat_index(t, rh), dsp.comfort_zone(t, rh)))
    w["state"].text("เสี่ยงหยดน้ำเกาะ - เปิดพัดลม" if risk else "ปลอดภัย")
    w["state"].color(COL_BAD if risk else COL_OK)
    w["chart"].set_next(0, int(round(t)))
    w["chart"].set_next(w["s_td"], int(round(td)))


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    show_matrix(False)
    risk, fails = None, 0
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        got = read_sht40()                                   # 1) วัด
        if got is None:
            fails += 1
            w["help"].text("อ่าน SHT40 ไม่ได้ %d ครั้ง" % fails)
        else:
            t, td, rh = air(got[0], got[1])                  # 2) ฟีเจอร์ + ตัดสิน
            was, risk = risk, at_risk(t, td)
            if risk != was:                                  # 3) ทำ: เฉพาะตอนสถานะเปลี่ยน
                show_matrix(risk)
                if was is not None:
                    beep("bad" if risk else "good")   # alert / ok
            show(w, got[0], got[1], t, td, rh, risk)        # 4) โชว์
        ui.poll()
        time.sleep_ms(TICK_MS)
    show_matrix(False)
    beep("good")                                            # done
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: ห้อง 30 C ความชื้น 70 % จุดน้ำค้างราวกี่องศา (ลองคิดคร่าว ๆ: RH ลด 5 % = Td ลดราว 1 C)
# 2) ตั้ง TEMP_OFFSET = -3.0 แล้วรันใหม่ อะไรเปลี่ยน อะไรไม่เปลี่ยน (T, Td, RH ดิบ, RH อากาศ) เพราะอะไร
# 3) ตั้ง RISK_C = 4.0 (เตือนเร็วขึ้น) แล้วหายใจรดเซนเซอร์ เทียบจำนวนครั้งที่เตือนกับ RISK_C = 2.0
