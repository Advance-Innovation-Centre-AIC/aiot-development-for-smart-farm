# sf1_02_crop_comfort.py - พืชของเรา "สบายดี" ไหม
#
# ภารกิจ   : ตั้งช่วงอุณหภูมิและความชื้นที่พืชชอบ แล้วให้บอร์ดตัดสินทุกวินาทีว่า
#            พืช "สบาย / เริ่มเครียด / แย่แล้ว" พร้อมไฟสีบนบอร์ดบอกสถานะ
# ลองเล่น  : ปัดวงล้อด้านซ้ายของจอเพื่อเลือกพืชของกลุ่ม แล้วเป่าลม/จับบอร์ดให้หลุดช่วง
#            ดูว่าไฟสถานะ หน้าพืช และกราฟเปลี่ยนตอนไหน
#            (ตั้ง TEMP_OFFSET ให้ตรงกับห้องก่อน ไม่งั้นพืชทุกชนิดจะ "ร้อนไป" ตลอด)
# ของบนบอร์ดที่ใช้ : SHT40 = อุณหภูมิ + ความชื้น, ไฟ RGB_RED บนบอร์ด = พืชมีปัญหา
#            จอไฟ RGB 16x8 = "หน้าพืช": ยิ้มเขียว / หน้าเฉยเหลือง / หน้าเศร้าแดง
#            ลำโพงส่งเสียงเฉพาะตอนสถานะเปลี่ยน (แย่ลง = เสียงเตือน, ดีขึ้น = เสียงเย้)
# บนจอ     : วงล้อเลือกพืช (Roller), วงแหวนคะแนน (Arc), ไฟสถานะ 3 ดวง (Led),
#            กราฟอุณหภูมิเทียบช่วงที่พืชชอบ (Chart)
# แนวคิด AIoT: Sense (อ่าน) -> Decide (ตัดสินด้วยกฎ) -> Act (ไฟ/จอ/เสียง)
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator

import gpio
import math
import rgbmatrix
import sensors
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
# ช่วงที่พืชชอบ (ตัวเลขตั้งต้นสำหรับการเรียน ไม่ใช่คำแนะนำทางเกษตรกรรม)
# แต่ละบรรทัด = (ชื่อ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)  เพิ่มพืชของกลุ่มต่อท้ายได้เลย
CROPS = (
    ("มะเขือเทศ", 20, 30, 60, 80),
    ("ผักสลัด", 15, 25, 50, 70),
    ("เห็ดนางฟ้า", 22, 28, 80, 95),
    ("กล้วยไม้", 22, 32, 60, 80),
)
START_CROP = "มะเขือเทศ"   # พืชที่เลือกไว้ตอนเริ่ม (เปลี่ยนระหว่างรันได้ด้วยวงล้อบนจอ)
RUN_MS = 120000
TICK_MS = 1000
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์ในห้อง (หรืออุณหภูมิที่ผู้สอนประกาศ) แล้วใส่ค่าชดเชย เช่น -9.5
                     # (ห้องแอร์ปกติ ~25-28 C)
                     # ตั้งแล้ว ความชื้นจะถูกแปลงเป็นของห้องให้เองด้วย (ดู room_humidity)
HUM_FIX = True       # แปลงความชื้นเป็นของห้อง (ดู room_humidity) ถ้าเทียบไฮโกรมิเตอร์ในห้องแล้วสูงเกินจริง ให้ตั้ง False
CHART_MAX_C = 50     # กราฟอุณหภูมิ 0-50 C
SOUND_GAP_MS = 3000  # เสียงเตือนห่างกันอย่างน้อย 3 วินาที (ค่าอยู่ตรงขอบช่วงจะได้ไม่ร้องรัว)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)
MOODS = ("สบายดี :)", "เริ่มเครียด", "แย่แล้ว!")


# ---- 2) ฮาร์ดแวร์ ----

# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def read_climate():
    # คืน (อุณหภูมิ, ความชื้น) ของห้อง (ชดเชยแล้ว) ถ้าอ่านไม่ได้คืน (None, None)
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
    # หา LED ด้วยชื่อ ไม่ใช่เลข: บน Dev Kit ดวง LED1/LED2 (เลข 0, 1) อยู่บน SoM
    # มองไม่เห็น ดวงที่เห็นคือ RGB_RED / RGB_GREEN / RGB_BLUE
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def show_led(led, level, tick):
    # ไฟบนบอร์ด: สบาย = ดับ, เครียด = กะพริบ, แย่ = ติดค้าง
    if led is None:
        return
    if level == 0:
        led.off()
    elif level == 1 and tick % 2 == 0:
        led.off()
    else:
        led.on()


# หน้าพืช 8x8 จุด วางกลางจอไฟ RGB (16x8)  "#" = จุดติด
FACES = (
    ("..####..", ".#....#.", "#.#..#.#", "#......#",
     "#.#..#.#", "#..##..#", ".#....#.", "..####.."),     # 0 สบาย (ยิ้ม)
    ("..####..", ".#....#.", "#.#..#.#", "#......#",
     "#......#", "#.####.#", ".#....#.", "..####.."),     # 1 เครียด (หน้าเฉย)
    ("..####..", ".#....#.", "#.#..#.#", "#......#",
     "#..##..#", "#.#..#.#", ".#....#.", "..####.."),     # 2 แย่ (หน้าเศร้า)
)
FACE_COLORS = (rgbmatrix.GREEN, rgbmatrix.YELLOW, rgbmatrix.RED)


def draw_face(level):
    # วาดทั้งจอไฟ RGB ในคำสั่งเดียว (blit: 64 ไบต์ จุดละ 4 บิต)
    buf = bytearray(64)
    color = FACE_COLORS[level]
    for y, row in enumerate(FACES[level]):
        for i, ch in enumerate(row):
            if ch == "#":
                x = 4 + i
                buf[y * 8 + (x >> 1)] |= (color << 4) if (x & 1) else color
    try:
        rgbmatrix.blit(buf)
    except OSError:
        pass                    # จอไฟ RGB ตอบไม่ทัน: ข้ามภาพนี้ไป ไม่ให้โปรแกรมหยุด


# ---- 3) สมอง (ตัดสินใจ) ----
def find_crop(name):
    # คืนลำดับของพืชใน CROPS ถ้าสะกดไม่ตรง คืน 0 (พืชตัวแรก) แทนการพัง
    for i in range(len(CROPS)):
        if CROPS[i][0] == name:
            return i
    return 0


def judge(t, h, crop):
    # คืน (ระดับ, เหตุผล)  0 = สบาย, 1 = เริ่มเครียด, 2 = แย่แล้ว (หลุดช่วงไปไกล)
    name, t_lo, t_hi, h_lo, h_hi = crop
    problems = []
    if t < t_lo:
        problems.append("หนาวไป")
    if t > t_hi:
        problems.append("ร้อนไป")
    if h < h_lo:
        problems.append("แห้งไป")
    if h > h_hi:
        problems.append("ชื้นไป")
    if not problems:
        return 0, "อยู่ในช่วงที่ชอบ"
    far = (t < t_lo - 3) or (t > t_hi + 3) or (h < h_lo - 10) or (h > h_hi + 10)
    return (2 if far else 1), " + ".join(problems)


def sat_pressure(t):
    # ความดันไอน้ำอิ่มตัว (hPa) ที่อุณหภูมิ t C (สูตร Magnus)
    return 6.112 * math.exp(17.62 * t / (243.12 + t))


def room_humidity(h_raw, t_raw, t_room):
    # อากาศอุ่นขึ้นรอบเซนเซอร์ ความชื้นสัมพัทธ์จึงอ่านได้ต่ำกว่าห้อง
    # ไอน้ำในอากาศเท่าเดิม แต่ห้องเย็นกว่า จึงแปลงกลับด้วยอัตราส่วนความดันไออิ่มตัว
    return min(100.0, h_raw * sat_pressure(t_raw) / sat_pressure(t_room))


def comfort_score(good, total):
    # คะแนนความสบาย 0-100 = สัดส่วนครั้งที่ "สบาย" จากทุกครั้งที่อ่าน
    return good * 100 // max(1, total)


def score_color(score):
    if score >= 80:
        return COL_OK
    if score >= 50:
        return COL_WARN
    return COL_BAD


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


def crop_text(crop):
    name, t_lo, t_hi, h_lo, h_hi = crop
    return ("พืช: " + name + "   ชอบ " + str(t_lo) + "-" + str(t_hi) + " C, " +
            str(h_lo) + "-" + str(h_hi) + " %RH")


def build_picker(w, idx):
    card(12, 64, 196, 276, "เลือกพืช (ปัด)")
    # ไม่ตั้ง value= ตอนสร้าง Roller: value= ของมันคือขนาดฟอนต์ด้วย เลือกแถวด้วย .value() ทีหลัง
    roller = ui.Roller(x=24, y=94, w=172, h=236, color=COL_TEXT)
    for crop in CROPS:
        roller.add_option(crop[0])
    roller.value(idx)
    w["roller"] = roller


def build_now_card(w):
    card(218, 64, 300, 190, "ตอนนี้")
    w["lbl_t"] = ui.Label("อุณหภูมิ -- C", x=230, y=92, color=COL_TEXT, value=24)
    w["lbl_h"] = ui.Label("ความชื้น -- %", x=230, y=124, color=COL_TEXT, value=24)
    w["mood"] = ui.Label("...", x=230, y=162, color=COL_WARN, value=28)
    w["why"] = ui.Label("", x=230, y=206, color=COL_DIM, value=16)


def build_score_card(w):
    card(528, 64, 252, 190, "คะแนนความสบาย")
    w["arc"] = ui.Arc(x=540, y=90, w=104, h=104, min=0, max=100, value=100)
    w["arc"].color(COL_OK)
    w["score"] = ui.Label("100", x=660, y=118, color=COL_TEXT, value=28)
    w["leds"] = []
    names = ("สบาย", "เครียด", "แย่")
    for i in range(3):          # ไฟสถานะ 3 ดวง ติดทีละดวงเหมือนแผงควบคุมจริง
        x = 540 + i * 80
        w["leds"].append(ui.Led(x=x, y=202, w=20, h=20, color=LEVEL_COLORS[i], value=0))
        ui.Label(names[i], x=x + 26, y=202, color=COL_DIM, value=14)
    ui.Label("100 = สบายทุกครั้งที่อ่าน", x=540, y=230, color=COL_DIM, value=14)


def build_chart(w):
    # กราฟ: เส้นส้ม = อุณหภูมิ, เส้นเขียว 2 เส้น = ขอบล่าง/บนของช่วงที่พืชชอบ
    w["chart"] = line_chart(218, 262, 380, 76, 0, CHART_MAX_C, COL_WARN)
    w["s_temp"] = 0
    w["s_lo"] = w["chart"].add_series(COL_OK)
    w["s_hi"] = w["chart"].add_series(COL_OK)
    ui.Label("เส้นส้ม = อุณหภูมิ", x=608, y=266, color=COL_WARN, value=14)
    ui.Label("เส้นเขียว = ช่วงที่ชอบ", x=608, y=290, color=COL_OK, value=14)


def build_screen(idx):
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("พืชของเราสบายดีไหม", x=12, y=6, color=COL_TEXT, value=24)
    w = {"crop": ui.Label(crop_text(CROPS[idx]), x=12, y=38, color=COL_INFO, value=16)}
    build_picker(w, idx)
    build_now_card(w)
    build_score_card(w)
    build_chart(w)
    w["status"] = ui.Label("ปัดวงล้อด้านซ้ายเพื่อเปลี่ยนพืช", x=12, y=352,
                           color=COL_DIM, value=16)
    ui.poll()
    return w


def picked_crop(w, idx):
    # อ่านเหตุการณ์จากจอ: ถ้าปัดวงล้อเลือกพืชใหม่ คืนลำดับใหม่ ไม่งั้นคืนลำดับเดิม
    # (Roller ส่ง value_changed พร้อมลำดับแถว เริ่มที่ 0)
    for ev in ui.poll():
        if ev["handle"] == w["roller"].id() and ev["type"] == "value_changed":
            if 0 <= ev["value"] < len(CROPS):
                idx = ev["value"]
    return idx


def show(w, t, h, level, why, score, crop):
    w["lbl_t"].text("อุณหภูมิ %.1f C" % t)
    w["lbl_h"].text("ความชื้น %.1f %%" % h)
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(MOODS[level])
    w["why"].text(why)
    w["arc"].value(score)
    w["arc"].color(score_color(score))
    w["score"].text(str(score))
    for i in range(3):
        w["leds"][i].value(1 if i == level else 0)    # 0 = หรี่ (ไม่ดับมืด)
    w["chart"].set_next(w["s_temp"], int(max(0, min(CHART_MAX_C, t))))
    w["chart"].set_next(w["s_lo"], int(crop[1]))   # กราฟรับเฉพาะจำนวนเต็ม
    w["chart"].set_next(w["s_hi"], int(crop[2]))


def show_sensor_error(w):
    w["mood"].color(COL_BAD)
    w["mood"].text("อ่านเซนเซอร์ไม่ได้")


# ---- 5) โปรแกรมหลัก ----
def finish(w, led, name, good, total):
    # จบรอบ: ดับไฟ ล้างจอไฟ RGB บอกวิธีเล่นใหม่ และพิมพ์คะแนนลง Console
    if led is not None:
        led.off()
    try:
        rgbmatrix.clear()
    except OSError:
        pass
    w["status"].color(COL_WARN)
    w["status"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()
    print("คะแนนความสบายของ", name, "=", comfort_score(good, total), "จาก", total, "ครั้ง")


def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    idx = find_crop(START_CROP)
    if CROPS[idx][0] != START_CROP:
        print("ไม่รู้จักพืช", START_CROP, "- ใช้", CROPS[idx][0], "แทน (สะกดให้ตรงกับใน CROPS)")
    w = build_screen(idx)
    led = led_named("RGB_RED")     # ไฟแดงบนบอร์ด = พืชมีปัญหา
    good = total = 0
    last_level = None
    last_sound = time.ticks_ms()
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        new_idx = picked_crop(w, idx)                   # 0) ผู้ใช้เปลี่ยนพืชไหม
        if new_idx != idx:
            idx = new_idx
            good = total = 0                           # พืชใหม่ = เริ่มนับคะแนนใหม่
            w["crop"].text(crop_text(CROPS[idx]))
            beep("tap")
        t, h = read_climate()                           # 1) อ่าน
        if t is None:
            show_sensor_error(w)
            time.sleep_ms(TICK_MS)
            continue
        level, why = judge(t, h, CROPS[idx])            # 2) ตัดสิน
        total += 1
        if level == 0:
            good += 1
        show(w, t, h, level, why, comfort_score(good, total), CROPS[idx])   # 3) โชว์
        if level != last_level:                         # 4) ทำ: เฉพาะตอนสถานะเปลี่ยน
            draw_face(level)
            quiet = time.ticks_diff(time.ticks_ms(), last_sound) > SOUND_GAP_MS
            if last_level is not None and quiet:
                beep("bad" if level > last_level else "good")
                last_sound = time.ticks_ms()
            last_level = level
        show_led(led, level, total)
        time.sleep_ms(TICK_MS)

    finish(w, led, CROPS[idx][0], good, total)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เพิ่มพืชของกลุ่มคุณลงใน CROPS (หาช่วงที่เหมาะจากอินเทอร์เน็ต) แล้วปัดวงล้อหาให้เจอ
# 2) ถ้าพืช "แย่แล้ว" ติดกัน 5 ครั้ง ให้ขึ้นข้อความเตือนตัวใหญ่ว่า "เปิดพัดลม/พ่นหมอก!"
#    (เก็บตัวนับไว้ใน main() แล้วใช้ w["status"] โชว์)
# 3) ออกแบบหน้าพืชของกลุ่มเองใน FACES (เช่น ใบไม้เหี่ยว) หรือแต่งเสียงเองด้วย
#    ui.tone(โน้ต, ui.WAVE_SINE, ความดัง 0-127, มิลลิวินาที)  เช่น ui.tone(72, ui.WAVE_SINE, 38, 200)  (38 = ดังราว 30%)
