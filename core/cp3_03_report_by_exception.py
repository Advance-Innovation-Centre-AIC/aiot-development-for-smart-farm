# cp3_03_report_by_exception.py - หลักการ 3.3: ส่งเมื่อเปลี่ยน + ส่งว่ายังอยู่ (report by exception + heartbeat)
#
# หลักการ  : วัดทุก SAMPLE_MS แต่ส่งเฉพาะตอนค่าต่างจาก "ใบที่ส่งไปล่าสุด" (ไม่ใช่ค่ารอบก่อน) มากกว่า DEADBAND
#            ค่าแรกส่งเสมอ และส่งใบ "ยังอยู่" (heartbeat) ทุก HEARTBEAT_S วินาที ปลายทางจึงแยก "ค่านิ่ง" กับ "บอร์ดตาย" ได้
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM ในข้อ 1)
# ลองเล่น  : ปล่อย VR1 นิ่ง ๆ ดูว่ามีแต่ใบ "ยังอยู่" แล้วหมุน VR1 ช้า ๆ และเร็ว ๆ ดูตัวเลข % ประหยัด
# ของบนบอร์ด: VR1 = ค่าที่วัด 0-100 % - ลำโพงดังตอนเริ่ม ตอนพัง และตอนจบเท่านั้น
# ใบที่ส่ง  : หัวข้อ bento-aiot/<TEAM>/core/report  JSON {"id", "n", "up_s", "value", "unit", "why"}
#            n = เลขใบ - up_s = บอร์ดเปิดมากี่วินาที - why = "change" / "heartbeat"
#            ดูด้วย MQTT Explorer (subscribe bento-aiot/<TEAM>/core/#) หน้าเว็บและแอปของคาบ 2 ยังไม่แสดงหัวข้อนี้
# บนจอ     : กราฟ ฟ้า = ค่าที่วัด เขียว = ค่าในใบที่ส่งล่าสุด (จึงเป็นขั้นบันได) - ส่งกี่ใบ จากที่วัดกี่ครั้ง - % ประหยัด
# ต่อยอด   : ให้ read_value() คืนอุณหภูมิ SHT40 (อ่าน 3 ครั้ง + TEMP_OFFSET แบบ cp1_04)
#            แล้วตั้ง DEADBAND = 0.3, UNIT = "C" และช่วงกราฟ LO, HI = 15, 40
# ในฟาร์ม  : เซนเซอร์ใช้แบตเตอรี่ ส่งน้อยลง = วิทยุทำงานน้อยลง แบตอยู่ได้นานขึ้น
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (Emulator ต่อ broker สาธารณะจริงผ่าน WebSocket)
# ระวัง     : พอร์ต 1883 ไม่เข้ารหัส (mqtt ของเฟิร์มแวร์นี้ไม่มี TLS) ใครก็อ่านได้ ห้ามส่งของลับ
#            ไม่มี retain และไม่มี Last Will คนรับจึงต้องจับเวลา heartbeat เอง

import json
import mqtt
import pots
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว - อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                       # เลขกลุ่มที่ผู้สอนแจก เช่น team05 ห้ามซ้ำกลุ่มอื่น
DEADBAND = 3         # ส่งเมื่อค่าต่างจากใบที่ส่งล่าสุดเกินเท่านี้ (%)
HEARTBEAT_S = 30     # ค่าไม่เปลี่ยนเลย ก็ส่งใบ "ยังอยู่" ทุกกี่วินาที
SAMPLE_MS = 1000     # วัดทุกกี่ ms (ถ้าส่งทุกครั้งที่วัด = ส่งถี่เท่านี้)
RUN_MS = 300000      # ส่งนาน 5 นาทีแล้วจบเอง
SPEAKER = 40         # ความดังลำโพงรวม 0-100% (firmware 2.4.2 ขึ้นไป)
VOLUME = 25          # ความดังเสียง 0-127 ใช้กับทุกเสียงในไฟล์นี้

BROKER = "broker.hivemq.com"          # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
CLIENT_ID = "bento-core-" + TEAM       # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
TOPIC = "bento-aiot/" + TEAM + "/core/report"
UNIT = "%"
LO, HI = 0, 100      # ช่วงของกราฟ

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_value():
    # VR1 เป็น 0-100 % (บอร์ดไม่มี pots.percent() จึงคิดเอง)
    return pots.read(0) * 100 // 4095


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
# (และไม่แตะเน็ต: ทดสอบได้โดยไม่ต้องมี WiFi)
def why_send(value, last, quiet_ms):
    # ส่งใบนี้ไหม: "change" / "heartbeat" / None
    # last = ค่าในใบที่ส่งไปล่าสุด (None = ยังไม่เคยส่ง) - quiet_ms = เงียบมานานเท่าไรแล้ว
    if last is None or abs(value - last) > DEADBAND:
        return "change"
    if quiet_ms >= HEARTBEAT_S * 1000:
        return "heartbeat"
    return None


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะบางเครื่องไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง ใช้ client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WARN)
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_farm(w):
    # บันไดสามขั้น WiFi -> IP -> broker ขั้นไหนพังคืนข้อความบอกว่าพังตรงไหน
    note(w, "ต่อ WiFi...", COL_WARN)      # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก (note เรียก ui.poll ให้)
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)         # ลองได้ 3 ครั้ง (ดู connect_broker)
    return "" if linked else "broker ไม่ตอบ"


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # ตั้ง 400 จุด กว้างไม่เกิน 400: LVGL ไม่วาดจุดกลมเมื่อจำนวนจุด >= ความกว้าง จึงได้เส้นเรียบ
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ส่งเมื่อเปลี่ยน + ส่งว่ายังอยู่", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("MQTT", x=380, y=12, color=COL_DIM, value=16)
    w = {"led": ui.Led(x=430, y=12, w=20, h=20, color=COL_OK, value=0)}
    w["link"] = ui.Label("ออฟไลน์", x=458, y=12, color=COL_DIM, value=16)
    card(12, 44, 420, 294, "ฟ้า = วัด  เขียว = ใบที่ส่ง")
    w["chart"] = line_chart(22, 72, 400, 256, LO, HI, COL_INFO)
    w["s_sent"] = w["chart"].add_series(COL_OK)
    card(442, 44, 338, 294, "นับใบ")
    w["sent"] = ui.Label(" ", x=456, y=80, color=COL_TEXT, value=20)
    w["pct"] = ui.Label(" ", x=456, y=150, color=COL_OK, value=28)
    w["last"] = ui.Label(" ", x=456, y=250, color=COL_TEXT, value=16)
    ui.Label(TOPIC, x=456, y=300, color=COL_DIM, value=14)
    w["note"] = ui.Label(" ", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_link(w, online):
    # ไฟ MQTT: ติด = ต่อ broker อยู่ ใบขึ้นได้ · หรี่ = ออฟไลน์
    w["led"].value(1 if online else 0)
    w["link"].color(COL_OK if online else COL_DIM)
    w["link"].text("เชื่อมต่อแล้ว" if online else "ออฟไลน์")


def note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)
    ui.poll()


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):   # จบโปรแกรมแบบปกติ (SystemExit ทำให้บอร์ดเริ่มระบบใหม่ และอาจค้างจนต้องถอดสาย)
    pass


def stop(w, msg):
    show_link(w, False)
    note(w, msg, COL_BAD)
    beep("bad")
    raise Stop


def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    if len(TEAM) != 6 or TEAM[:4] != "team" or not TEAM[4:].isdigit() or TEAM == "team00":
        stop(w, "แก้ TEAM เป็นเลขกลุ่มก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    show_link(w, True)
    note(w, "ต่างเกิน %d %s หรือครบ %d วิ = ส่ง" % (DEADBAND, UNIT, HEARTBEAT_S), COL_OK)
    beep("start")
    n = would = 0
    last = None
    t0 = t_sent = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            if not mqtt.is_connected():       # สายหลุดระหว่างรอบ: บอกบนจอก่อนจบ
                stop(w, "สายหลุด")
            now = time.ticks_ms()
            v = read_value()                  # 1) วัด
            would += 1                        # ถ้าส่งทุกครั้งที่วัด ใบนี้ก็ต้องส่ง
            why = why_send(v, last, time.ticks_diff(now, t_sent))   # 2) ตัดสินว่าต้องส่งไหม
            if why:                           # 3) ส่ง
                body = json.dumps({"id": TEAM, "n": n + 1, "up_s": time.ticks_diff(now, t0) // 1000,
                                   "value": v, "unit": UNIT, "why": why})
                try:                          # สายหลุด: publish โยน OSError ไม่ใช่คืน False
                    ok = mqtt.publish(TOPIC, body)
                except OSError:
                    stop(w, "สายหลุด")
                if ok:
                    n, last, t_sent = n + 1, v, now
                    w["last"].text("#%d %s = %d" % (n, why, v))
                    print("ส่ง:", body)
                else:                         # broker ไม่รับใบนี้ รอบหน้าลองใหม่เอง
                    note(w, "broker ไม่รับใบ %d" % (n + 1), COL_WARN)
            # 4) โชว์ - กราฟรับจำนวนเต็มเท่านั้น
            w["chart"].set_next(0, int(v))
            if last is not None:
                w["chart"].set_next(w["s_sent"], int(last))   # เส้นเขียว = ค่าในใบล่าสุด
            w["sent"].text("ส่ง %d ใบ / วัด %d ครั้ง" % (n, would))
            w["pct"].text("ประหยัด %d %%" % (100 - n * 100 // would))
            ui.poll()
            time.sleep_ms(SAMPLE_MS)
        show_link(w, False)
    finally:
        mqtt.disconnect()                     # หยุดกลางทางก็ตัดสาย broker ให้เรียบร้อย
    note(w, "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่", COL_WARN)
    beep("good")


try:
    main()
except Stop:
    pass                 # stop() บอกเหตุบนจอแล้ว จบเงียบ ๆ

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: ค่าที่วัดได้ 40, 40.5, 41, 43, 43.2, 46 ตามลำดับ DEADBAND = 2 จะส่งกี่ใบ (ไม่นับ heartbeat)
#    ใบ้: ค่าแรกส่งเสมอ แล้วเทียบกับ "ใบที่ส่งไปล่าสุด" ต้องต่างมากกว่า 2
# 2) ตั้ง HEARTBEAT_S = 10 แล้วปล่อย VR1 นิ่ง ๆ ตัวเลข % ประหยัดลดลงเท่าไร คุ้มไหมกับการรู้เร็วขึ้นว่าบอร์ดตาย
# 3) ตั้ง DEADBAND = 0 แล้วหมุน VR1 ดูว่าเหลือประหยัดกี่ % (DEADBAND เล็กเกิน = สัญญาณรบกวนก็ถูกส่งหมด)
