# sf2_02_greenhouse_report.py - โรงเรือนรายงานตัวออกไปให้เจ้าของฟาร์มดูทุก 5 วินาที
#
# ภารกิจ   : วัดอากาศ (SHT40 DPS368) ความเอียง (IMU) และลูกบิดจำลอง ดิน VR1 แสง VR3 ถังน้ำ VR4
#            ทุก 1 วินาที แล้วส่งรายงานผ่าน MQTT ทุก 5 วินาที ให้หน้าเว็บและแอปของกลุ่มในชั่วโมงที่ 2
#            เปิด farm_web.html?team=<เลขกลุ่ม> บนมือถือ แล้วดูค่าฟาร์มขึ้นเป็นการ์ด
# ลองเล่น  : เป่าลมหายใจใส่บอร์ด หมุนลูกบิด ตะแคงบอร์ด แล้วดูว่ามือถือเห็นช้ากว่าจอบอร์ดกี่วินาที
#            แตะแถบด้านบนเพื่อสลับหน้า: ตอนนี้ / กราฟ / ใบที่ส่ง
# ของบนบอร์ดที่ใช้ : SHT40 = อุณหภูมิ + ความชื้น · DPS368 = ความกดอากาศ · BMI270 = ความเอียง
#            VR1 = ดิน · VR3 = แสง · VR4 = ถังน้ำ (สามตัวนี้เป็นค่าจำลอง จึงส่งคีย์ sim บอกคนรับ)
#            SW4 = ส่งเดี๋ยวนี้ · SW5 = กดเรียกเจ้าของฟาร์ม (ส่งเข้าหัวข้อ event)
#            จอไฟ RGB 16x8 = นับใบที่ส่ง · ลำโพงดังตอนส่งออก (ปิดได้ด้วย SOUND = False)
# บนจอ     : แท็บ 3 หน้า (Tabview), ตัวเลขใหญ่ (Seg7), หลอดลูกบิด (Bar), กราฟบอร์ด/คลาวด์ (Chart),
#            ตารางใบที่ส่ง (Table), ไฟออนไลน์ (Led), วงหมุนตอนต่อเน็ต (Spinner), หลอดนับถอยหลังใบถัดไป (Bar)
# แนวคิด AIoT: Sense -> Send  วัดถี่ได้ แต่ส่งห่าง ๆ เพราะการส่งกวน broker ของทั้งห้อง
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator (MQTT ใน Emulator เป็นแบบจำลอง)
# ระวัง     : broker.hivemq.com พอร์ต 1883 ไม่เข้ารหัส ใครก็อ่านหัวข้อเราได้ ห้ามส่งของลับ
#            "ทุก 5 วินาที" คือถามว่าถึงเวลาหรือยัง ไม่ใช่ time.sleep(5) · คีย์ทั้งหมดอยู่ใน app/MQTT_CONTRACT_th.md

import buttons
import json
import math
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "bento-teamXX"            # Hotspot มือถือของกลุ่ม
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
TEAM = "teamXX"                       # ผู้สอนแจก team01 ถึง team20 ห้ามซ้ำกลุ่มอื่น
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์/แอปอากาศ แล้วใส่ค่าชดเชย เช่น -7.0
HUM_OFFSET = 0.0     # (ใช้ค่าเดียวกับที่กลุ่มหาได้ในคาบ 1)

BROKER = "broker.hivemq.com"          # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
ROOT = "bento-aiot"                   # หน้าเว็บของคอร์สแม่ฟังชื่อนำหน้านี้
CLIENT_ID = "bento-farm-" + TEAM      # ต้องไม่ซ้ำกับใครบน broker ทั้งโลก
TOPIC = ROOT + "/" + TEAM + "/telemetry"
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
READ_MS, SEND_MS, POLL_MS = 1000, 5000, 100
RUN_MS = 1800000                      # ส่งนาน 30 นาที พอให้ชั่วโมงที่ 2 มีข้อมูลเข้าแอป
SOUND = True                          # 20 บอร์ดดังพร้อมกันหนวกหู ตั้ง False = ดังเฉพาะตอนกด SW4
ZERO_SAMPLES = 5                      # ตอนเริ่มอ่าน IMU 5 ครั้งเฉลี่ยเป็น "ท่าตั้งต้น" (บอร์ดวางเอียงอยู่แล้วราว 39 องศา)
LOG_ROWS = 5
TAB_BAR_H = 44

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_climate():
    """คืน (อุณหภูมิ, ความชื้น) ที่ชดเชยแล้ว ตัวที่อ่านไม่ได้เป็น None"""
    t = h = None
    try:
        t = sensors.sht40.temperature() + TEMP_OFFSET
        h = max(0.0, min(100.0, sensors.sht40.humidity() + HUM_OFFSET))
    except Exception:
        pass
    return t, h


def read_pressure():
    try:
        return sensors.dps368.pressure()
    except Exception:
        return None


def read_motion():
    """คืน (ax, ay, az) หน่วย m/s2 ถ้าอ่าน IMU ไม่ได้คืน None"""
    try:
        ax, ay, az, _, _, _ = sensors.bmi270.motion()
        return ax, ay, az
    except Exception:
        return None


def measure_zero():
    """ตอนเริ่ม: เฉลี่ยท่าที่บอร์ดวางอยู่เป็น "ศูนย์" ความเอียงนับจากท่านี้ ไม่ใช่จากแนวราบ"""
    total, n = [0.0, 0.0, 0.0], 0
    for _ in range(ZERO_SAMPLES):
        m = read_motion()
        if m is not None:
            total = [total[i] + m[i] for i in range(3)]
            n += 1
        time.sleep_ms(200)
    return [v / n for v in total] if n else None


def knob_percent(i):
    """ลูกบิด VR1-VR4 (i = 0-3) เป็น 0-100 %  บอร์ดไม่มี pots.percent() จึงคิดเอง"""
    return pots.read(i) * 100 // 4095


class Edge:
    """จับจังหวะ "เพิ่งกด" ของปุ่ม: กดหนึ่งครั้ง = ทำงานหนึ่งครั้ง แม้จะกดค้างไว้"""

    def __init__(self):
        self.was_down = False

    def pressed_now(self, is_down):
        fired = is_down and not self.was_down
        self.was_down = is_down
        return fired


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def valid_team(team):
    return len(team) == 6 and team[:4] == "team" and team[4:].isdigit() and team != "team00"


def r1(x):
    return None if x is None else round(x, 1)


def tilt_from(zero, m):
    """มุม (องศา) ระหว่างท่าตอนเริ่มกับท่าตอนนี้ = บอร์ดเอียงไปจากเดิมเท่าไร (ทิศไหนก็ได้)"""
    if zero is None or m is None:
        return None
    dot = sum(zero[i] * m[i] for i in range(3))
    size = math.sqrt(sum(v * v for v in zero)) * math.sqrt(sum(v * v for v in m))
    return 0 if size == 0 else int(math.degrees(math.acos(max(-1.0, min(1.0, dot / size)))) + 0.5)


def farm_reading(t, h, p, m, knobs):
    """รวมทุกค่าของฟาร์มเป็น dict ลำดับคีย์ตามสัญญา MQTT ข้อ 3.1  ตัวไหนอ่านไม่ได้เป็น None"""
    d = {"temp_c": r1(t), "rh": r1(h), "hpa": r1(p), "az": None if m is None else round(m[2], 2)}
    d["soil"], d["light"], d["tank"] = knobs          # ลูกบิดจำลอง VR1 VR3 VR4
    return d


def build_payload(n, d, manual):
    """รายงานหนึ่งใบ: มี id กับ n เสมอ · sim บอกตรง ๆ ว่าคีย์ไหนมาจากลูกบิดจำลอง · by บอกว่าส่งเพราะอะไร"""
    body = {"id": TEAM, "n": n}
    body.update(d)
    body["sim"], body["by"] = "soil light tank", "sw4" if manual else "timer"
    return body


def build_call():
    """SW5 เป็น "เหตุการณ์" ไม่ใช่รายงาน จึงส่งเข้าอีกหัวข้อ"""
    return {"id": TEAM, "event": "sw5", "msg": "call"}


# ---- 4) เครือข่าย ----
def connect_farm(w):
    """บันไดสามขั้น WiFi -> IP -> broker ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน"""
    show_status(w, "กำลังต่อ WiFi จอจะนิ่งสักครู่", COL_WARN)
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้ ตรวจชื่อวงกับรหัส"
    show_status(w, "ได้ IP แล้ว กำลังต่อ broker", COL_WARN)
    try:
        linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        linked = False
    return "" if linked else "broker ไม่ตอบ เน็ตกันพอร์ต 1883?"


def publish_json(topic, obj):
    """คืน "ok" / "refused" (broker ไม่รับใบนี้) / "lost" (สายหลุด: publish โยน OSError ไม่ใช่คืน False)"""
    try:
        return "ok" if mqtt.publish(topic, json.dumps(obj)) else "refused"
    except OSError:
        return "lost"


# ---- 5) หน้าจอ ----
def build_now_tab(w, tab):
    """หน้า "ตอนนี้" (พิกัดนับจากมุมซ้ายบนของหน้าแท็บ ไม่ใช่ของจอ)"""
    names = ("อุณหภูมิ C", "ความชื้น %", "ความกด hPa")
    cols = (COL_OK, COL_INFO, COL_WARN)
    w["segs"] = []
    for i in range(3):
        ui.Label(names[i], x=i * 180, y=0, color=COL_DIM, value=16, parent=tab)
        w["segs"].append(ui.Seg7(text="--", x=i * 180, y=26, w=160, h=52, color=cols[i], parent=tab))
    ui.Label("ลูกบิดจำลอง", x=560, y=0, color=COL_DIM, value=16, parent=tab)
    w["bars"], w["knob_lbls"] = [], []
    for i, name in enumerate(("ดิน", "แสง", "ถัง")):
        ui.Label(name, x=560, y=30 + i * 34, color=COL_TEXT, value=14, parent=tab)
        w["bars"].append(ui.Bar(x=600, y=32 + i * 34, w=100, h=18, min=0, max=100, value=0, parent=tab))
        w["knob_lbls"].append(ui.Label("--", x=708, y=30 + i * 34, color=COL_TEXT, value=14, parent=tab))
    w["tilt"] = ui.Label("เอียงจากตอนเริ่ม -- องศา", x=0, y=100, color=COL_TEXT, value=20, parent=tab)
    ui.Label("ส่ง az (m/s2) ให้คนรับ แต่จอโชว์มุมเทียบท่าตอนเริ่ม เพราะบอร์ดวางเอียงอยู่แล้ว",
             x=0, y=132, color=COL_DIM, value=14, parent=tab)


def build_chart_tab(w, tab):
    """หน้า "กราฟ" 15-40 C ไม่ใช่ 0-100 เพราะช่วงกว้างเกินทำให้เส้นแบนจนมองไม่เห็นว่าค่าขยับ"""
    w["chart"] = ui.Chart(x=0, y=0, w=720, h=156, color=COL_INFO, min=15, max=40, parent=tab)
    w["s_read"] = 0
    w["s_sent"] = w["chart"].add_series(COL_OK)
    ui.Label("ฟ้า = บอร์ดเห็นทุก 1 วิ", x=0, y=166, color=COL_INFO, value=14, parent=tab)
    ui.Label("เขียว = คลาวด์เห็นทุก 5 วิ (ค่าที่ส่งล่าสุด)", x=240, y=166, color=COL_OK, value=14, parent=tab)


def build_sent_tab(w, tab):
    table = ui.Table(x=0, y=0, w=720, h=196, cols=4, rows=LOG_ROWS + 1, parent=tab)
    for col, width in enumerate((110, 190, 190, 230)):
        table.col_width(col, width)
    table.add_row("ใบที่", "วินาที", "อุณหภูมิ C", "ส่งเพราะ")
    w["table"] = table


def build_screen():
    """สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง"""
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โรงเรือนรายงานตัว", x=12, y=6, color=COL_TEXT, value=24)
    w = {"online": ui.Led(x=250, y=12, w=20, h=20, color=COL_OK, value=0),
         "status": ui.Label("กำลังเริ่ม", x=280, y=12, color=COL_DIM, value=16),
         "sent": ui.Label("ส่งแล้ว 0 ใบ", x=600, y=12, color=COL_TEXT, value=16),
         "spin": ui.Spinner(x=744, y=4, w=36, h=36)}
    tabs = ui.Tabview(x=12, y=44, w=768, h=294, color=COL_CARD, value=TAB_BAR_H)
    build_now_tab(w, tabs.add_tab("ตอนนี้"))
    build_chart_tab(w, tabs.add_tab("กราฟ"))
    build_sent_tab(w, tabs.add_tab("ใบที่ส่ง"))
    ui.Label("ส่งใบถัดไปใน", x=12, y=352, color=COL_DIM, value=16)
    w["next"] = ui.Bar(x=130, y=356, w=300, h=18, min=0, max=SEND_MS, value=0)
    w["next"].color(COL_INFO)          # สีตอนสร้างใช้ไม่ได้กับ Bar ต้องตั้งหลังสร้าง
    show_table(w, [])
    ui.poll()
    return w


def show_status(w, msg, col):
    w["status"].color(col)
    w["status"].text(msg)
    ui.poll()


def show_now(w, d, tilt):
    for i, key in enumerate(("temp_c", "rh", "hpa")):
        w["segs"][i].text("--" if d[key] is None else "%.1f" % d[key])     # Seg7 รับข้อความ ไม่ใช่ตัวเลข
    for i, key in enumerate(("soil", "light", "tank")):
        w["bars"][i].value(d[key])
        w["knob_lbls"][i].text(str(d[key]))
    w["tilt"].text("เอียงจากตอนเริ่ม " + ("--" if tilt is None else str(tilt)) + " องศา")


def show_table(w, rows):
    for r in range(LOG_ROWS):
        row = rows[r] if r < len(rows) else ("-", "-", "-", "-")
        for c in range(4):
            w["table"].cell(r + 1, c, row[c])


# ---- 6) โปรแกรมหลัก ----
def stop(w, msg):
    rgbmatrix.clear()
    w["spin"].hide()
    w["online"].value(0)
    show_status(w, msg, COL_BAD)
    print("หยุดที่:", msg)
    raise SystemExit


def on_sent(w, body, sec, rows, manual):
    """ใบนี้ออกไปแล้ว: นับ จอไฟ เสียง ตาราง (เรียกเฉพาะตอนส่งสำเร็จ ไม่ใช่ทุกรอบ)"""
    n = body["n"]
    w["sent"].text("ส่งแล้ว " + str(n) + " ใบ")
    rgbmatrix.score(n, rgbmatrix.GREEN)              # เขียนจอไฟเฉพาะตอนเลขเปลี่ยน
    if SOUND or manual:
        ui.sfx(ui.SFX_FLAPPY_SCORE)
    rows.insert(0, (str(n), str(sec), "--" if body["temp_c"] is None else "%.1f" % body["temp_c"], body["by"]))
    del rows[LOG_ROWS:]
    show_table(w, rows)
    print("ส่ง:", json.dumps(body))


def main():
    w = build_screen()
    if not valid_team(TEAM):
        stop(w, "แก้ TEAM เป็นเลขกลุ่มก่อน เช่น team05")
    zero = measure_zero()                             # วางบอร์ดนิ่ง ๆ ตอนเริ่ม 1 วินาที
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    w["spin"].hide()
    w["online"].value(1)
    show_status(w, "ส่งเข้า " + TOPIC, COL_OK)
    send_btn, call_btn = Edge(), Edge()
    sent, last_t, d, rows, first = 0, None, {}, [], True
    t_read = t_send = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        # นาฬิกาสามเรือนในลูปเดียว: ปุ่มทุก 0.1 วิ วัดทุก 1 วิ ส่งทุก 5 วิ (คอร์สแม่ s10/05)
        manual = send_btn.pressed_now(buttons.pressed(0))
        if call_btn.pressed_now(buttons.pressed(1)):
            if publish_json(TOPIC_EVENT, build_call()) == "lost":
                stop(w, "สายหลุด ส่งไม่ออก")
            ui.sfx(ui.SFX_UI_SELECT)
        if first or time.ticks_diff(now, t_read) >= READ_MS:
            t_read = now
            t, h = read_climate()
            m = read_motion()
            knobs = (knob_percent(0), knob_percent(2), knob_percent(3))
            d = farm_reading(t, h, read_pressure(), m, knobs)
            show_now(w, d, tilt_from(zero, m))
            if d["temp_c"] is not None:
                w["chart"].set_next(w["s_read"], int(d["temp_c"]))
            if last_t is not None:                    # เส้นเขียว = ค่าที่ส่งล่าสุด จึงเป็นขั้นบันได
                w["chart"].set_next(w["s_sent"], int(last_t))
        if first or manual or time.ticks_diff(now, t_send) >= SEND_MS:
            first, t_send = False, now
            body = build_payload(sent + 1, d, manual)
            result = publish_json(TOPIC, body)
            if result == "lost":
                stop(w, "สายหลุด ส่งไม่ออก")
            if result == "ok":
                sent, last_t = sent + 1, d["temp_c"]
                on_sent(w, body, time.ticks_diff(now, t0) // 1000, rows, manual)
            else:
                show_status(w, "ใบที่ " + str(sent + 1) + " ถูกปฏิเสธ", COL_WARN)
        w["next"].value(time.ticks_diff(time.ticks_ms(), t_send))
        ui.poll()
        time.sleep_ms(POLL_MS)

    mqtt.disconnect()
    rgbmatrix.clear()
    w["online"].value(0)
    show_status(w, "จบ ส่งไป " + str(sent) + " ใบ", COL_DIM)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เพิ่มคีย์ "crop" ใน build_payload เป็นชื่อพืชภาษาอังกฤษ เช่น "tomato" แล้วดูว่าหน้าเว็บมีการ์ดใหม่
# 2) ส่งเฉพาะเมื่ออุณหภูมิเปลี่ยนเกิน 0.3 C จากใบล่าสุด นับว่าจำนวนใบลดลงเท่าไร
#    ใบ้: ฟาร์ม 1,000 แห่งส่งทุก 5 วิ = 200 ใบต่อวินาที เข้าระบบเดียว
