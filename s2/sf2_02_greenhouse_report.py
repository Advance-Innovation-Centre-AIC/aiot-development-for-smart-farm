# sf2_02_greenhouse_report.py - โรงเรือนส่งรายงานทุก 5 วินาที
#
# ภารกิจ   : วัดอากาศทุก 1 วินาที ส่งรายงานผ่าน MQTT ทุก 5 วินาที
# ลองเล่น  : เปิด farm_web.html?team=<เลขกลุ่ม> บนมือถือ แล้วเป่าลมใส่บอร์ด หมุนลูกบิด
# บนจอ     : ตัวเลขใหญ่ (Seg7) หลอดลูกบิด (Bar) กราฟ (Chart) ไฟออนไลน์ (Led)
# แนวคิด AIoT: วัดถี่ได้ แต่ส่งห่าง ๆ โดยถามว่าถึงเวลาหรือยัง ไม่ใช่ time.sleep(5)
# บอร์ด     : TESAIoT Dev Kit และ BENTO Emulator (Emulator ต่อ broker สาธารณะจริงผ่าน WebSocket)
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM
# ระวัง     : พอร์ต 1883 ไม่เข้ารหัส ใครก็อ่านหัวข้อเราได้ ห้ามส่งของลับ

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
TEMP_OFFSET = 0.0    # ชดเชยความอุ่นจากชิป เช่น -7.0

BROKER = "broker.hivemq.com"          # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
CLIENT_ID = "bento-farm-" + TEAM       # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
TOPIC = "bento-aiot/" + TEAM + "/telemetry"
TOPIC_EVENT = "bento-aiot/" + TEAM + "/event"
BTN_NAMES = ("SW5", "SW6")
READ_MS, SEND_MS, POLL_MS = 1000, 5000, 100
RUN_MS = 1800000                      # ส่งนาน 30 นาที
SOUND = True                          # False = ดังเฉพาะตอนกด SW5

SPEAKER = 40  # ลำโพงรวม 0-100% (firmware 2.4.2 ขึ้นไป)
VOLUME = 25   # ความดังเสียง 0-127
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----

TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def read_climate():
    # ตัวที่อ่านไม่ได้เป็น None
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
    # แกน z (m/s2) วางราบราว 9.8 ตะแคงแล้วลดลง
    try:
        return sensors.bmi270.motion()[2]
    except Exception:
        return None


def knob_percent(i):
    return pots.read(i) * 100 // 4095


class Button:

    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        # True ครั้งเดียวต่อการกด
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    # รอ ms แต่อ่านปุ่มทุก 20 ms ไม่ให้พลาดการแตะสั้น ๆ
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
def connect_broker(w):
    # broker สาธารณะบางเครื่องไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง ใช้ client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WARN)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_farm(w):
    # บันไดสามขั้น WiFi -> IP -> broker ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน
    show_note(w, "ต่อ WiFi... จอนิ่งได้", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)         # ลองได้ 3 ครั้ง (ดู connect_broker)
    return "" if linked else "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"


def publish_json(topic, obj):
    # คืน "ok" / "refused" (broker ไม่รับใบนี้) / "lost" (สายหลุด: publish โยน OSError ไม่ใช่คืน False)
    try:
        return "ok" if mqtt.publish(topic, json.dumps(obj)) else "refused"
    except OSError:
        return "lost"


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # LVGL ไม่วาดจุดกลมเมื่อจำนวนจุด >= ความกว้าง
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: เชื่อมต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
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
    # 15-40 C เพราะช่วง 0-100 ทำให้เส้นแบน
    w["chart"] = line_chart(22, 178, 400, 152, 15, 40, COL_INFO)
    w["s_sent"] = w["chart"].add_series(COL_OK)
    card(442, 152, 338, 186, "az เทียบตอนเริ่ม")
    w["tilt"] = ui.Label("--", x=456, y=184, color=COL_TEXT, value=28)
    ui.Label("ส่งใบถัดไปใน", x=456, y=266, color=COL_DIM, value=16)
    w["next"] = ui.Bar(x=456, y=294, w=300, h=18, max=SEND_MS)
    w["note"] = ui.Label("กำลังเริ่ม", x=12, y=352, color=COL_DIM)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=632, y=10, color=COL_DIM, value=16)
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
    # จอโชว์ az เทียบตอนเริ่ม ส่วนที่ส่งออกเป็นค่าจริง
    w["tilt"].text("--" if az is None or az0 is None else "%+.2f" % (az - az0))


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    # จบโปรแกรมแบบปกติ (SystemExit ทำให้บอร์ดเริ่มระบบใหม่ และอาจค้างจนต้องถอดสาย)
    pass


def stop(w, msg):
    show_link(w, False)
    rgbmatrix.clear()
    w["led"].value(0)
    show_note(w, msg, COL_BAD)
    ui.poll()
    print("หยุดที่:", msg)
    raise Stop


def on_sent(w, body, manual):
    w["sent"].text("ส่งแล้ว %d ใบ" % body["n"])
    rgbmatrix.score(body["n"], rgbmatrix.GREEN)
    if SOUND or manual:
        beep("good")
    print("ส่ง:", json.dumps(body))


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    if len(TEAM) != 6 or TEAM[:4] != "team" or not TEAM[4:].isdigit() or TEAM == "team00":
        stop(w, "แก้ TEAM เป็นเลขกลุ่มก่อน")
    az0 = read_az()  # ท่าตอนเริ่ม = ศูนย์ของความเอียง
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    show_link(w, True)
    w["led"].value(1)
    show_note(w, "ส่งเข้า " + TOPIC, COL_OK)
    send_btn, call_btn = Button(1), Button(0)
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
            if last_t is not None:
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


try:
    main()
except Stop:
    pass
finally:
    try:
        mqtt.disconnect()              # ปิดทุกครั้ง แม้ถูกหยุดกลางทาง
    except Exception:
        pass

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เพิ่มคีย์ "crop" ใน build_payload เช่น "tomato" หน้าเว็บมีการ์ดใหม่ไหม
# 2) ส่งเฉพาะเมื่ออุณหภูมิเปลี่ยนเกิน 0.3 C นับว่าใบลดลงเท่าไร
#    ใบ้: ฟาร์ม 1,000 แห่งส่งทุก 5 วิ = 200 ใบต่อวินาที
