# cp1_04_calibrate.py - หลักการ 1.4: สอบเทียบก่อนเชื่อ (ชดเชย ตั้งศูนย์ สองจุด)
#
# หลักการ  : ค่าจากเซนเซอร์ = ค่าจริง + ความเพี้ยน สองแบบที่แก้ได้ด้วยเลขง่าย ๆ
#            ชดเชย (offset) = บวก/ลบค่าคงที่ เช่น SHT40 วัดความอุ่นของบอร์ดเองติดมาด้วย -> TEMP_OFFSET
#            ตั้งศูนย์ (tare) = จำท่าตอนนี้ไว้เป็น 0 แล้วลบออกทุกครั้ง
#            บอร์ดที่วางนิ่งบนโต๊ะก็อาจเอียงอยู่แล้ว (ดูค่าดิบบนจอก่อนกด SW5)
#            สองจุด (two-point) = จำค่าดิบตอน "แห้ง" = 0 % และตอน "เปียก" = 100 %
#            แล้วค่าระหว่างนั้น = (raw - แห้ง) / (เปียก - แห้ง) x 100 แก้ทั้งชดเชยและอัตราขยายในครั้งเดียว
#            ถ้าจุดแห้งกับจุดเปียกใกล้กันเกินไป ห้ามหาร (หารด้วยศูนย์ หรือได้ค่าเพี้ยนมาก)
# ลองเล่น  : 1) ดูการ์ด SHT40 ค่าดิบเทียบค่าชดเชย แล้วแก้ TEMP_OFFSET ให้ตรงเทอร์โมมิเตอร์ในห้อง
#            2) วางบอร์ดตามปกติ กด SW5 (ปุ่มล่าง) = ท่านี้คือศูนย์ แล้วลองเอียง ดูค่าหักศูนย์
#            3) ใช้ VR1 แทน "หัววัดความชื้นดิน": หมุนไปตำแหน่งหนึ่ง กด SW6 (ปุ่มบน) = จุดแห้ง
#               หมุนไปอีกตำแหน่ง กด SW6 = จุดเปียก จากนั้นหมุนดู % ที่สอบเทียบแล้ว กด SW6 อีกครั้ง = เริ่มใหม่
# ของบนบอร์ด: SHT40 (อุณหภูมิ), IMU BMI270 + dsp.tilt() (มุมเอียง), ลูกบิด VR1, ปุ่ม SW5/SW6, ลำโพง
# ในฟาร์ม  : หัววัดความชื้นดินแต่ละตัวให้ค่าดิบไม่เท่ากัน จึงสอบเทียบกับดินแห้งและดินอิ่มน้ำของแปลงจริง
#            หัววัดบางรุ่นให้ค่าลดลงเมื่อดินเปียก สูตรสองจุดนี้ใช้ได้ทั้งสองทิศ
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#            ใน Emulator อุณหภูมิและมุมเอียงเป็นค่าจำลองจากแผง ไม่ใช่ค่าจากบอร์ดจริง

import buttons
import dsp
import pots
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบเทอร์โมมิเตอร์ในห้องแล้วใส่ค่าชดเชย เช่น -9.5
MIN_SPAN = 100       # จุดแห้งกับจุดเปียกต้องห่างกันเกินกี่ count จึงยอมคำนวณ
CLIMATE_MS = 2000    # อ่าน SHT40 ทุก 2 วินาที (อุณหภูมิเปลี่ยนช้า)
SAMPLE_MS = 20       # อ่านปุ่มทุกกี่ ms (เฟิร์มแวร์กรองสั่น 50 ms ทุกครั้งที่อ่าน จึงต้องอ่านถี่)
TICK_MS = 500        # อัปเดตจอทุกกี่ ms
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_temp_raw():
    # อุณหภูมิดิบจาก SHT40 (ยังไม่ชดเชย) ลอง 3 ครั้ง อ่านไม่ได้ = None
    for _ in range(3):
        try:
            return sensors.sht40.temperature_humidity()[0]
        except Exception:
            time.sleep_ms(50)
    return None


def read_tilt():
    # (roll, pitch) เป็นองศา จากทิศของแรงโน้มถ่วง อ่าน IMU ไม่ได้ = None
    try:
        m = sensors.bmi270.motion()
        return dsp.tilt(m[0], m[1], m[2])
    except Exception:
        return None


def read_probe():
    # VR1 แทนหัววัดความชื้นดิน: เลขดิบ 0-4095
    return pots.read(0)


def just_pressed(i, prev):
    # True ครั้งเดียวตอนปุ่ม i (0 = SW5 ปุ่มล่าง, 1 = SW6 ปุ่มบน) เพิ่งถูกกดลง
    # ต้องเรียกทุกรอบ (ทุก SAMPLE_MS) เพราะการกรองสั่นเดินหน้าเฉพาะตอนที่อ่าน
    d = bool(buttons.pressed(i))
    e = d and not prev[i]
    prev[i] = d
    return e


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
def tare(angle, zero):
    # หักศูนย์ แล้วห่อผลให้อยู่ในช่วง -180..180 องศา (เช่น 179 - (-179) = -2 ไม่ใช่ 358)
    return (angle - zero + 180) % 360 - 180


def two_point(raw, dry, wet):
    # สอบเทียบสองจุด: dry -> 0 %, wet -> 100 % ใช้ได้ทั้งหัววัดที่ค่าขึ้นหรือลงเมื่อเปียก
    # จุดยังไม่ครบ หรือสองจุดใกล้กันเกิน MIN_SPAN = None (ไม่หาร)
    if dry is None or wet is None or abs(wet - dry) < MIN_SPAN:
        return None
    return max(0, min(100, (raw - dry) * 100 / (wet - dry)))


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
STEPS = ("หมุน VR1 ไปจุดแห้ง กด SW6", "หมุน VR1 ไปจุดเปียก กด SW6", "ครบสองจุด  SW6 = เริ่มใหม่")


def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("สอบเทียบก่อนเชื่อ", x=12, y=6, color=COL_TEXT, value=24)
    card(12, 44, 378, 150, "1) ชดเชย: SHT40 (C)")
    ui.Label("ดิบ + TEMP_OFFSET = ค่าที่ใช้", x=24, y=78, color=COL_DIM, value=16)
    w = {"t_fix": ui.Label("--", x=24, y=110, color=COL_OK, value=24)}
    card(402, 44, 378, 150, "2) ตั้งศูนย์: มุมเอียง (องศา)")
    w["a_raw"] = ui.Label("ดิบ --", x=414, y=78, color=COL_WARN, value=20)
    w["a_fix"] = ui.Label("--", x=414, y=116, color=COL_OK, value=20)
    w["zero"] = ui.Label("ศูนย์ = 0, 0 (กด SW5)", x=414, y=160, color=COL_DIM, value=14)
    card(12, 202, 768, 136, "3) สองจุด: VR1 = หัววัดดิน")
    w["p_raw"] = ui.Label("raw --", x=24, y=232, color=COL_WARN, value=20)
    w["pts"] = ui.Label(" ", x=200, y=236, color=COL_TEXT, value=16)
    w["pct"] = ui.Label("-- %", x=560, y=226, color=COL_OK, value=28)
    w["bar"] = ui.Bar(x=24, y=270, w=744, h=18, min=0, max=100, value=0)
    w["bar"].color(COL_OK)          # สีตอนสร้างใช้ไม่ได้กับ Bar ต้องตั้งหลังสร้าง
    w["step"] = ui.Label(" ", x=24, y=300, color=COL_WARN, value=16)
    w["help"] = ui.Label("SW5 (ล่าง) = ตั้งศูนย์   SW6 (บน) = แห้ง / เปียก / เริ่มใหม่",
                         x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def ang(a):
    return "R %.1f  P %.1f" % a


def show_points(w, dry, wet):
    # จุดแห้ง จุดเปียก และขั้นต่อไป (0 = รอจุดแห้ง, 1 = รอจุดเปียก, 2 = ครบ)
    w["pts"].text("แห้ง %s   เปียก %s" % ("--" if dry is None else dry, "--" if wet is None else wet))
    w["step"].text(STEPS[0 if dry is None else (1 if wet is None else 2)])


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    prev = [False, False]                # ปุ่มรอบก่อนกดอยู่ไหม (SW5, SW6)
    zero = (0.0, 0.0)                    # ท่าที่นับเป็นศูนย์ (roll, pitch)
    dry = wet = None                     # จุดแห้ง จุดเปียก (เลขดิบของ VR1)
    show_points(w, dry, wet)
    t0 = t_show = time.ticks_ms()
    t_air = time.ticks_add(t0, -CLIMATE_MS)
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        if just_pressed(0, prev):                              # SW5 = ท่าตอนนี้คือศูนย์
            a = read_tilt()
            if a:
                zero = a
                w["zero"].text("ศูนย์ = " + ang(zero))
                beep("start")                                  # ok
        if just_pressed(1, prev):                              # SW6 = แห้ง -> เปียก -> เริ่มใหม่
            raw = read_probe()
            if dry is not None and wet is None and abs(raw - dry) < MIN_SPAN:
                w["step"].text("ใกล้จุดแห้งเกิน")
                beep("bad")                                  # alert: ไม่รับจุดนี้ (กันหารด้วยศูนย์)
            else:
                if dry is None:
                    dry = raw
                    beep("tap")                                     # press: ได้จุดแห้ง
                elif wet is None:
                    wet = raw
                    beep("good")                              # done: ครบสองจุด
                else:
                    dry = wet = None
                    beep("tap")                                     # press: เริ่มใหม่
                show_points(w, dry, wet)
        if time.ticks_diff(now, t_air) >= CLIMATE_MS:          # 1) ชดเชย: ทุก 2 วินาที
            t_air = now
            t = read_temp_raw()
            if t is not None:
                w["t_fix"].text("%.1f %+.1f = %.1f" % (t, TEMP_OFFSET, t + TEMP_OFFSET))
        if time.ticks_diff(now, t_show) >= TICK_MS:            # ทุกครึ่งวินาที
            t_show = now
            a = read_tilt()                                    # 2) ตั้งศูนย์
            if a:
                w["a_raw"].text("ดิบ " + ang(a))
                w["a_fix"].text(ang((tare(a[0], zero[0]), tare(a[1], zero[1]))))
            raw = read_probe()                                 # 3) สองจุด
            pct = two_point(raw, dry, wet)
            p = 0 if pct is None else int(round(pct))          # widget รับเฉพาะจำนวนเต็ม
            w["p_raw"].text("raw %d" % raw)
            w["pct"].text("-- %" if pct is None else "%d %%" % p)
            w["bar"].value(p)
            ui.poll()
        time.sleep_ms(SAMPLE_MS)
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: เก็บจุดแห้งที่ raw 3000 และจุดเปียกที่ raw 1000 แล้วหมุน VR1 ไปที่ 2000 ได้กี่ %
# 2) หาค่า TEMP_OFFSET ของบอร์ดคุณ: เทียบค่าดิบกับเทอร์โมมิเตอร์ในห้อง แล้วใส่ผลต่าง (เช่น -9.5) รันใหม่
# 3) ตั้ง MIN_SPAN = 0 แล้วกด SW6 สองครั้งติดกันโดยไม่หมุน VR1 เกิดอะไรขึ้น
#    (สองจุดเท่ากันพอดี = ZeroDivisionError โปรแกรมหยุด · ต่างกันนิดเดียว = % กระโดด 0 <-> 100)
#    ด่าน MIN_SPAN กันทั้งสองแบบ
