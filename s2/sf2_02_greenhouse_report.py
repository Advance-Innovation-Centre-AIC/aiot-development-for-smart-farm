# sf2_02_greenhouse_report.py - โรงเรือนรายงานตัวออกไปให้เจ้าของฟาร์มดูทุก 5 วินาที
#
# ภารกิจ   : วัดอากาศ (SHT40 DPS368) ความเอียง (IMU) และลูกบิดจำลอง ดิน VR1 แสง VR3 ถังน้ำ VR4
#            ทุก 1 วินาที แล้วส่งรายงานผ่าน MQTT ทุก 5 วินาที ให้หน้าเว็บและแอปของกลุ่มในชั่วโมงที่ 2
#            เปิด farm_web.html?team=<เลขกลุ่ม> บนมือถือ แล้วดูค่าฟาร์มขึ้นเป็นการ์ด
# ลองเล่น  : เป่าลมหายใจใส่บอร์ด หมุนลูกบิด ตะแคงบอร์ด แล้วดูว่ามือถือเห็นช้ากว่าจอบอร์ดกี่วินาที
# ของบนบอร์ดที่ใช้ : SHT40 = อุณหภูมิ + ความชื้น · DPS368 = ความกดอากาศ · BMI270 = ความเอียง
#            VR1 = ดิน · VR3 = แสง · VR4 = ถังน้ำ (สามตัวนี้เป็นค่าจำลอง จึงส่งคีย์ sim บอกคนรับ)
#            SW5 (ปุ่มล่าง) = ส่งเดี๋ยวนี้ · SW6 (ปุ่มบน) = กดเรียกเจ้าของฟาร์ม (ส่งเข้าหัวข้อ event)
#            จอไฟ RGB 16x8 = นับใบที่ส่ง · ลำโพงดังตอนส่งออก (ปิดได้ด้วย SOUND = False)
# บนจอ     : ตัวเลขใหญ่ (Seg7), หลอดลูกบิด (Bar), กราฟบอร์ดกับคลาวด์ (Chart), ไฟออนไลน์ (Led),
#            หลอดนับถอยหลังใบถัดไป (Bar)
# แนวคิด AIoT: Sense -> Send  วัดถี่ได้ แต่ส่งห่าง ๆ เพราะการส่งกวน broker ของทั้งห้อง
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator (MQTT ใน Emulator เป็นแบบจำลอง)
# ระวัง     : broker.hivemq.com พอร์ต 1883 ไม่เข้ารหัส ใครก็อ่านหัวข้อเราได้ ห้ามส่งของลับ
#            "ทุก 5 วินาที" คือถามว่าถึงเวลาหรือยัง ไม่ใช่ time.sleep(5) · คีย์ทั้งหมดอยู่ใน app/MQTT_CONTRACT_th.md

import buttons
import json
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                       # เลขกลุ่มที่ผู้สอนแจก เช่น team05 ห้ามซ้ำกลุ่มอื่น
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์ แล้วใส่ค่าชดเชย เช่น -7.0 (ค่าเดียวกับคาบ 1)

BROKER = "broker.hivemq.com"          # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
CLIENT_ID = "bento-farm-" + TEAM      # ต้องไม่ซ้ำกับใครบน broker ทั้งโลก
TOPIC = "bento-aiot/" + TEAM + "/telemetry"
TOPIC_EVENT = "bento-aiot/" + TEAM + "/event"
BTN_NAMES = ("SW5", "SW6")            # ปุ่มล่าง = pressed(0), ปุ่มบน = pressed(1) ตามตัวอักษรบนแผง
READ_MS, SEND_MS, POLL_MS = 1000, 5000, 100
RUN_MS = 1800000                      # ส่งนาน 30 นาที พอให้ชั่วโมงที่ 2 มีข้อมูลเข้าแอป
SOUND = True                          # ทุกบอร์ดในห้องดังพร้อมกันหนวกหู ตั้ง False = ดังเฉพาะตอนกด SW5

VOLUME = 25                            # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----

# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def read_climate():
    # คืน (อุณหภูมิที่ชดเชยแล้ว, ความชื้น, ความกด) ตัวที่อ่านไม่ได้เป็น None
    t = h = p = None
    try:
        t, h = sensors.sht40.temperature() + TEMP_OFFSET, sensors.sht40.humidity()
    except Exception:
        pass
    try:
        p = sensors.dps368.pressure()
    except Exception:
        pass
    return t, h, p


def read_az():
    # ค่าเร่งแกน z (m/s2) วางราบราว 9.8 ตะแคงแล้วลดลง = กระถางล้ม
    try:
        return sensors.bmi270.motion()[2]
    except Exception:
        return None


def knob_percent(i):
    # ลูกบิด VR1-VR4 (i = 0-3) เป็น 0-100 %  บอร์ดไม่มี pots.percent() จึงคิดเอง
    return pots.read(i) * 100 // 4095


class Button:
    # ปุ่มบนฐานบอร์ด (0 = SW5 ปุ่มล่าง, 1 = SW6 ปุ่มบน) ที่ไม่พลาดการกดสั้น ๆ
    # เฟิร์มแวร์กรองสัญญาณสั่น 50 ms ถ้าอ่านรอบละครั้งการกดแบบแตะจะหายไป จึงอ่านบ่อย ๆ ใน wait_ms

    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        # True ครั้งเดียวต่อการกดหนึ่งครั้ง (กดค้างไว้ก็ไม่นับซ้ำ)
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    # รอ ms มิลลิวินาที แต่ระหว่างรอก็อ่านปุ่มทุก 20 ms เพื่อไม่พลาดการกดสั้น ๆ
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def r1(x, digits=1):
    return None if x is None else round(x, digits)


def build_payload(n, t, h, p, az, knobs, manual):
    # รายงานหนึ่งใบ คีย์ครบตามสัญญา MQTT ข้อ 3.1: มี id กับ n เสมอ ตัวไหนอ่านไม่ได้เป็น None
    # (ลำดับคีย์ในใบที่ส่งจริงอาจสลับกัน แอปจึงต้องอ่านด้วยชื่อคีย์ ไม่ใช่ตำแหน่ง)
    # sim บอกตรง ๆ ว่าคีย์ไหนมาจากลูกบิดจำลอง · by บอกว่าส่งเพราะครบเวลาหรือเพราะคนกด SW5
    body = {"id": TEAM, "n": n, "temp_c": r1(t), "rh": r1(h), "hpa": r1(p), "az": r1(az, 2)}
    body["soil"], body["light"], body["tank"] = knobs          # ลูกบิดจำลอง VR1 VR3 VR4
    body["sim"], body["by"] = "soil light tank", "sw5" if manual else "timer"
    return body


# ---- 4) เครือข่าย ----
def connect_farm(w):
    # บันไดสามขั้น WiFi -> IP -> broker ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    try:
        linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        linked = False
    return "" if linked else "broker ไม่ตอบ พอร์ต 1883?"


def publish_json(topic, obj):
    # คืน "ok" / "refused" (broker ไม่รับใบนี้) / "lost" (สายหลุด: publish โยน OSError ไม่ใช่คืน False)
    try:
        return "ok" if mqtt.publish(topic, json.dumps(obj)) else "refused"
    except OSError:
        return "lost"


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # กราฟเส้นเรียบ ไม่มีจุดกลม: LVGL ไม่วาดจุดเมื่อจำนวนจุด >= ความกว้างกราฟ
    # เราจึงให้กว้างไม่เกิน 400 และตั้ง 400 จุด (เฟิร์มแวร์รับได้ 10-400)
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("โรงเรือนรายงานตัว", x=12, y=6, color=COL_TEXT, value=24)
    w = {"led": ui.Led(x=250, y=12, w=20, h=20, color=COL_OK),
         "sent": ui.Label("ส่งแล้ว 0 ใบ", x=600, y=12, color=COL_TEXT, value=16), "segs": [], "bars": []}
    for i, name in enumerate(("อุณหภูมิ C", "ความชื้น %", "ความกด hPa")):
        card(12 + i * 184, 44, 176, 100, name)
        w["segs"].append(ui.Seg7(text="--", x=22 + i * 184, y=80, w=150, h=52, color=(COL_OK, COL_INFO, COL_WARN)[i]))
    card(564, 44, 216, 100, "ดิน / แสง / ถัง (จำลอง)")
    for i in range(3):
        w["bars"].append(ui.Bar(x=576, y=72 + i * 22, w=190, h=14))
    card(12, 152, 420, 186, "กราฟ C: ฟ้า = บอร์ด  เขียว = ส่งแล้ว")
    # 15-40 C ไม่ใช่ 0-100 เพราะช่วงกว้างเกินทำให้เส้นแบนจนมองไม่เห็นว่าค่าขยับ
    w["chart"] = line_chart(22, 178, 400, 152, 15, 40, COL_INFO)
    w["s_sent"] = w["chart"].add_series(COL_OK)
    card(442, 152, 338, 186, "az เทียบตอนเริ่ม")
    w["tilt"] = ui.Label("--", x=456, y=184, color=COL_TEXT, value=28)
    ui.Label("ส่งใบถัดไปใน", x=456, y=266, color=COL_DIM, value=16)
    w["next"] = ui.Bar(x=456, y=294, w=300, h=18, max=SEND_MS)
    w["note"] = ui.Label("กำลังเริ่ม", x=12, y=352, color=COL_DIM)
    ui.poll()
    return w


def show_note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def show_now(w, t, h, p, az, az0, knobs):
    for i, v in enumerate((t, h, p)):
        w["segs"][i].text("--" if v is None else "%.1f" % v)     # Seg7 รับข้อความ ไม่ใช่ตัวเลข
    for i in range(3):
        w["bars"][i].value(knobs[i])
    # บอร์ดวางเอียงอยู่แล้ว (az ราว 7.7 ไม่ใช่ 9.8) จอจึงโชว์ az ที่เปลี่ยนไปจากตอนเริ่ม ส่วนที่ส่งออกเป็นค่าจริง
    w["tilt"].text("--" if az is None or az0 is None else "%+.2f" % (az - az0))


# ---- 6) โปรแกรมหลัก ----
def stop(w, msg):
    rgbmatrix.clear()
    w["led"].value(0)
    show_note(w, msg, COL_BAD)
    ui.poll()
    print("หยุดที่:", msg)
    raise SystemExit


def on_sent(w, body, manual):
    # ใบนี้ออกไปแล้ว: นับ จอไฟ เสียง (เรียกเฉพาะตอนส่งสำเร็จ ไม่ใช่ทุกรอบ)
    w["sent"].text("ส่งแล้ว %d ใบ" % body["n"])
    rgbmatrix.score(body["n"], rgbmatrix.GREEN)       # เขียนจอไฟเฉพาะตอนเลขเปลี่ยน
    if SOUND or manual:
        beep("good")
    print("ส่ง:", json.dumps(body))


def main():
    w = build_screen()
    if len(TEAM) != 6 or TEAM[:4] != "team" or not TEAM[4:].isdigit() or TEAM == "team00":
        stop(w, "แก้ TEAM เป็นเลขกลุ่มก่อน")
    az0 = read_az()                    # ท่าที่บอร์ดวางอยู่ตอนเริ่ม = "ศูนย์" ของความเอียง
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    w["led"].value(1)
    show_note(w, "ส่งเข้า " + TOPIC, COL_OK)
    send_btn, call_btn = Button(0), Button(1)
    sent, last_t, first = 0, None, True
    t_read = t_send = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        manual = send_btn.pressed_now()               # SW5 = ส่งเดี๋ยวนี้
        if call_btn.pressed_now():                    # SW6 = "เหตุการณ์" ไม่ใช่รายงาน จึงส่งเข้าอีกหัวข้อ
            if publish_json(TOPIC_EVENT, {"id": TEAM, "event": "sw6", "msg": "call"}) == "lost":
                stop(w, "สายหลุด ส่งไม่ออก")
            beep("tap")
        if first or time.ticks_diff(now, t_read) >= READ_MS:   # นาฬิกาสามเรือน: ปุ่ม 0.1 วัด 1 ส่ง 5 วิ
            t_read = now
            t, h, p = read_climate()
            az, knobs = read_az(), (knob_percent(0), knob_percent(2), knob_percent(3))
            show_now(w, t, h, p, az, az0, knobs)
            if t is not None:
                w["chart"].set_next(0, int(t))
            if last_t is not None:                    # เส้นเขียว = ค่าที่ส่งล่าสุด จึงเป็นขั้นบันได
                w["chart"].set_next(w["s_sent"], int(last_t))
        if first or manual or time.ticks_diff(now, t_send) >= SEND_MS:
            first, t_send = False, now
            body = build_payload(sent + 1, t, h, p, az, knobs, manual)
            result = publish_json(TOPIC, body)
            if result == "lost":
                stop(w, "สายหลุด ส่งไม่ออก")
            if result == "ok":
                sent, last_t = sent + 1, t
                on_sent(w, body, manual)
            else:
                show_note(w, "ใบที่ %d ถูกปฏิเสธ" % (sent + 1), COL_WARN)
        w["next"].value(time.ticks_diff(time.ticks_ms(), t_send))
        ui.poll()
        wait_ms(POLL_MS, (send_btn, call_btn))

    mqtt.disconnect()
    rgbmatrix.clear()
    w["led"].value(0)
    show_note(w, "จบ ส่งไป %d ใบ" % sent, COL_DIM)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เพิ่มคีย์ "crop" ใน build_payload เป็นชื่อพืชภาษาอังกฤษ เช่น "tomato" แล้วดูว่าหน้าเว็บมีการ์ดใหม่
# 2) ส่งเฉพาะเมื่ออุณหภูมิเปลี่ยนเกิน 0.3 C จากใบล่าสุด นับว่าจำนวนใบลดลงเท่าไร
#    ใบ้: ฟาร์ม 1,000 แห่งส่งทุก 5 วิ = 200 ใบต่อวินาที เข้าระบบเดียว
