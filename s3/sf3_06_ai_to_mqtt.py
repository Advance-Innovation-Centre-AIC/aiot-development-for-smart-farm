# sf3_06_ai_to_mqtt.py - ผล AI บนบอร์ดขึ้น MQTT: ให้แอปของกลุ่มรู้ทันทีว่า AI เห็นอะไร
#
# ภารกิจ   : รันโมเดล AI บนบอร์ด (หาด้วยชื่อ) แล้วส่ง "ผลที่ AI สรุปแล้ว" ขึ้น bento-aiot/<TEAM>/ai
#            ส่งเมื่อมีผลใหม่ที่ป้ายเปลี่ยน และส่งซ้ำทุก 2 วินาทีเป็นสัญญาณชีพ (heartbeat)
# ลองเล่น  : แก้ WIFI_SSID WIFI_PASS TEAM แล้วรัน · เปิดหน้า apps/web-dashboard/ (แสดง .../ai และเตือนเมื่อ anomaly)
#            หรือพิมพ์บนเครื่องตัวเอง
#            mosquitto_sub -h broker.hivemq.com -t 'bento-aiot/<TEAM>/ai' -v
#            วางบอร์ดนิ่ง แล้วเขย่า ดูว่าข้อความเปลี่ยนตอนไหน และ heartbeat มาทุกกี่วินาที
# ของบนบอร์ดที่ใช้ : แกน AI บนชิป (edge_ai) + IMU, WiFi + MQTT, ลำโพง (ดังตอนป้ายเพิ่งเป็นอันตราย)
# บนจอ     : ชื่อโมเดลที่ใช้, ป้ายผลตัวใหญ่, วงแหวนความมั่นใจ (Arc), ไฟอันตราย (Led), ไฟ MQTT (Led), ข้อความที่ส่ง
# แนวคิด AIoT: ส่ง "ผลสรุป" ไม่ใช่ข้อมูลดิบ = ประหยัดเน็ต และข้อมูลดิบไม่ออกนอกฟาร์ม
#            ส่งเฉพาะตอนมีความหมาย (ป้ายเปลี่ยน) + heartbeat ให้แอปรู้ว่าบอร์ดยังอยู่ · ห่างกันอย่างน้อย 200 ms
# โมเดล    : หาด้วยชื่อตามลำดับ MODEL_KEYS: AnomalousVibration (ส่งลงบอร์ดจาก Edge AI Store) ก่อน
#            ถ้าบอร์ดไม่มีหรือเลือกไม่สำเร็จจึงใช้ Motion ที่ติดมากับบอร์ด · ข้อความส่งแค่ "ชื่อ" โมเดล ไม่ส่งรหัสโมเดล
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.2 ขึ้นไป · 2.4.1 ก็รันได้) · ใน Emulator MQTT เป็นแบบจำลอง
# สัญญา MQTT: หัวข้อ .../ai = {"id": ทีม, "n": ลำดับข้อความ, "model": ชื่อโมเดล, "label": ป้าย, "conf": ความมั่นใจ %}
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM · ยังไม่แก้ TEAM = ทำงานออฟไลน์ (พิมพ์ผลลง Console แทน)

import edge_ai
import json
import mqtt
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                        # เลขกลุ่มที่ผู้สอนแจก เช่น team05 (ต้องตรงกับแอป)
BROKER = "broker.hivemq.com"
CLIENT_ID = "bento-ai-" + TEAM + "-%04x" % (time.ticks_ms() & 0xFFFF)   # ตัวท้ายสุ่มทุกครั้งที่รัน: รันใหม่ทันทีก็ไม่ชน id เก่า
TOPIC = "bento-aiot/" + TEAM + "/ai"
MODEL_KEYS = ("AnomalousVibration", "Motion")   # ลองตามลำดับ: โมเดลจาก Store ก่อน ไม่มีหรือเลือกไม่ได้ค่อยใช้โมเดลในตัว
DANGER = ("anomaly", "shaking")        # ป้ายที่ถือว่าอันตราย (ดังเสียง + ไฟแดง)
HEARTBEAT_MS = 2000                    # ป้ายไม่เปลี่ยน ก็ยังส่งซ้ำทุกเท่านี้
GAP_MS = 200                           # ห้ามส่งถี่กว่านี้ (broker สาธารณะ ใช้ร่วมกันหลายกลุ่ม)
TICK_MS = 100                          # ถามผล AI ทุก 0.1 วินาที (จอเขียนเฉพาะตอนมีผลใหม่)
RUN_MS = 300000
VOLUME = 25                            # ความดังเสียง 0-127 (≈20%)
SPEAKER = 40                           # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    # เสียงเบา ๆ แทน ui.sfx (ui.sfx ดังคงที่ ปรับเบาไม่ได้) · เล่นโน้ต MIDI ทีละตัว ห่างกัน 120 ms
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def find_models(keys):
    # โมเดลที่ชื่อตรงกับ keys เรียงตามลำดับที่อยากใช้ (ลำดับบนบอร์ดเปลี่ยนได้หลังรีบูต) เก็บแค่ ลำดับ กับ ชื่อ
    try:
        ms = edge_ai.models()
    except Exception:
        return []
    return [(m["index"], m["name"]) for key in keys for m in ms if key.lower() in m["name"].lower()]


def read_result():
    # ผลล่าสุดของ AI หรือ None · สายไปแกน AI หลุดชั่วคราว (OSError) ก็แค่ข้ามรอบนี้
    try:
        return edge_ai.result()
    except OSError:
        return None


# ---- 3) สมอง (ตัดสินใจ) ----
def should_send(changed, since_ms):
    # ส่งเมื่อ: ห่างครั้งก่อนพอ และ (ป้ายต่างจากที่ส่งไปล่าสุด หรือ ถึงเวลา heartbeat)
    # ป้ายที่เปลี่ยนระหว่างรอช่วงห่าง 200 ms จะค้างไว้ แล้วส่งทันทีที่ครบช่วง ไม่หล่นหาย
    if since_ms < GAP_MS:
        return False
    return changed or since_ms >= HEARTBEAT_MS


# ---- 4) เครือข่าย ----
def go_online(w):
    # WiFi -> broker · ขั้นไหนพังคืน False แล้วทำงานต่อแบบออฟไลน์ (พิมพ์ผลลง Console)
    # client id สร้างจาก TEAM: ถ้าหลายกลุ่มลืมแก้ teamXX จะชนกันแล้ว broker เตะกันหลุด จึงไม่ต่อเลย
    if not (len(TEAM) == 6 and TEAM[:4] == "team" and TEAM[4:].isdigit() and TEAM != "team00"):
        note(w, "แก้ TEAM ก่อน (team01-team99): ออฟไลน์", COL_WARN)
        return False
    note(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    step = "WiFi"                      # บอกให้ชัดว่าพังขั้นไหน: WiFi หรือ broker
    try:
        if wifi.connect(WIFI_SSID, WIFI_PASS) and wifi.ip() != "0.0.0.0":
            step = "broker"
            if mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60):
                note(w, "ออนไลน์ " + TOPIC, COL_OK)
                return True
    except OSError:
        pass
    note(w, "ต่อ " + step + " ไม่ได้: ออฟไลน์ (ทำงานต่อ)", COL_WARN)
    return False


def send(online, obj):
    # ส่ง JSON (QoS 0) · คืน True ถ้าส่งขึ้น broker ได้ · ออฟไลน์หรือสายหลุดก็แค่พิมพ์ลง Console
    line = json.dumps(obj)
    print(line)
    if online:
        try:
            return bool(mqtt.publish(TOPIC, line))
        except OSError:
            pass
    return False


# ---- 5) หน้าจอ ----
def label(text, x, y, col=COL_DIM, size=16):
    return ui.Label(text, x=x, y=y, color=col, value=size)


def note(w, text, col):
    w["note"].color(col)
    w["note"].text(text)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    label("ผล AI ขึ้น MQTT", 12, 6, COL_TEXT, 24)
    w = {"model": label("หาโมเดล...", 12, 44, COL_INFO, 20)}
    w["arc"] = ui.Arc(x=12, y=84, w=180, h=180, min=0, max=100, value=0)
    w["conf"] = label("0 %", 70, 162, COL_TEXT, 24)
    label("AI เห็นว่า", 220, 90)
    w["label"] = label("-", 220, 116, COL_TEXT, 28)
    w["led"] = ui.Led(x=220, y=170, w=36, h=36, color=COL_BAD, value=0)
    label("ไฟแดง = ป้ายอันตราย", 268, 178)
    label("MQTT", 560, 90)
    w["mqtt"] = ui.Led(x=560, y=116, w=36, h=36, color=COL_OK, value=0)
    w["mqtt_txt"] = label("ยังไม่ได้ต่อ", 606, 124, COL_DIM)
    w["sent"] = label("ยังไม่ได้ส่ง", 220, 230)
    w["json"] = label(" ", 12, 300, COL_TEXT, 14)
    w["note"] = label(" ", 12, 352, COL_WARN, 16)
    ui.poll()
    return w


def show_link(w, online):
    # ไฟ MQTT: ติดเขียว = ต่อ broker อยู่ ข้อความขึ้นแอปได้ · ดับ = ออฟไลน์ ผลพิมพ์ลง Console แทน
    w["mqtt"].value(1 if online else 0)
    w["mqtt_txt"].color(COL_OK if online else COL_BAD)
    w["mqtt_txt"].text("เชื่อมต่อแล้ว" if online else "ออฟไลน์")


def show_result(w, lab, conf):
    w["label"].text(lab)
    w["conf"].text("%d %%" % conf)
    w["arc"].value(conf)
    bad = lab in DANGER
    w["arc"].color(COL_BAD if bad else COL_OK)
    w["led"].value(1 if bad else 0)


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    name = None
    for idx, nm in find_models(MODEL_KEYS):   # ตัวแรกเลือกไม่สำเร็จ ก็ลองตัวถัดไป (เช่น โมเดลในตัว)
        try:
            edge_ai.select(idx)
            name = nm
            break
        except OSError:
            pass
    if name is None:
        note(w, "ไม่พบหรือโหลดโมเดลไม่ได้: " + " / ".join(MODEL_KEYS), COL_BAD)
        return
    w["model"].text("โมเดล: " + name)
    online = go_online(w)
    show_link(w, online)
    seq, lab, conf, sent_lab, n = -1, None, 0, None, 0
    t0 = t_sent = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            now = time.ticks_ms()
            r = read_result()
            new = bool(r) and r["seq"] != seq
            if new:                                        # 1) ผลใหม่จาก AI
                seq, old = r["seq"], lab
                lab, conf = r["label"] or "-", int(r["conf"] * 100)
                show_result(w, lab, conf)
                if lab in DANGER and lab != old:
                    beep(84, 76)                           # เพิ่งเป็นอันตราย: ดังครั้งเดียว
            if lab is not None and should_send(lab != sent_lab, time.ticks_diff(now, t_sent)):
                n += 1                                     # 2) ส่งเมื่อมีความหมาย หรือถึง heartbeat
                ok = send(online, {"id": TEAM, "n": n, "model": name, "label": lab, "conf": conf})
                t_sent, sent_lab = now, lab
                w["sent"].text(("ส่งแล้ว %d ข้อความ" if ok else "ออฟไลน์: พิมพ์ลง Console %d") % n)
                w["json"].text("%s %d%% n=%d" % (lab, conf, n))
            if online and not mqtt.is_connected():
                online = False
                show_link(w, False)
                note(w, "เน็ตหลุด ทำงานต่อแบบออฟไลน์", COL_WARN)
            ui.poll()
            time.sleep_ms(TICK_MS)
    finally:
        try:
            edge_ai.stop()                                 # หยุดโมเดลเสมอ แม้โปรแกรมถูกหยุดกลางทาง
        except OSError:
            pass
        if online:
            mqtt.disconnect()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ตั้ง HEARTBEAT_MS = 10000 แล้วดูใน mosquitto_sub ว่าข้อความลดลงเท่าไรตอนวางบอร์ดนิ่ง
# 2) ในแอปของกลุ่ม (s2/app/) เพิ่มกฎ: ไม่ได้ข้อความจาก .../ai เกิน 5 วินาที = ขึ้นเตือน "AI เงียบ"
# 3) ส่ง "conf" เฉพาะตอนเกิน CONF 60 % หรือเพิ่มคีย์ "danger": 1/0 ให้แอปใช้ง่ายขึ้น
