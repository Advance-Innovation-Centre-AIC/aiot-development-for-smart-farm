# sf3_06_ai_to_mqtt.py - ผล AI บนบอร์ดขึ้น MQTT: ให้แอปของกลุ่มรู้ทันทีว่า AI เห็นอะไร
#
# ภารกิจ   : รันโมเดล AI บนบอร์ด (หาด้วยชื่อ) แล้วส่ง "ผลที่ AI สรุปแล้ว" ขึ้น bento-aiot/<TEAM>/ai
#            ส่งเมื่อมีผลใหม่ที่ป้ายเปลี่ยน และส่งซ้ำทุก 2 วินาทีเป็นสัญญาณชีพ (heartbeat)
# ลองเล่น  : แก้ WIFI_SSID WIFI_PASS TEAM แล้วรัน · เปิดหน้า apps/web-dashboard/ (แสดง .../ai และเตือนเมื่อป้ายอันตราย)
#            และหน้าแจ้งเตือนเข้ามือถือ s3/app/notify/index.html?team=<TEAM> (เปิดในเบราว์เซอร์ ไม่ต้องติดตั้งอะไร)
#            วางบอร์ดนิ่ง แล้วเขย่า ดูว่าข้อความเปลี่ยนตอนไหน และ heartbeat มาทุกกี่วินาที
# บนจอ     : ชื่อโมเดลที่ใช้, ป้ายผลตัวใหญ่, วงแหวนความมั่นใจ (Arc), ไฟอันตราย (Led), ไฟ MQTT (Led), ข้อความที่ส่ง
# แนวคิด AIoT: ส่ง "ผลสรุป" ไม่ใช่ข้อมูลดิบ = ประหยัดเน็ต และข้อมูลดิบไม่ออกนอกฟาร์ม
#            ส่งเฉพาะตอนมีความหมาย (ป้ายเปลี่ยน) + heartbeat ให้แอปรู้ว่าบอร์ดยังอยู่ · ห่างกันอย่างน้อย 200 ms
#            broker สาธารณะหลุดได้เสมอ: หลุดหรือส่งไม่ขึ้น = ต่อใหม่เอง รอ 5 -> 10 -> 20 -> 30 วินาที ไม่ทิ้งงานกลางทาง
#            ระหว่างหลุด n ยังนับต่อ แอปจึงเห็นว่าขาดไปกี่ข้อความ (n กระโดด)
# โมเดล    : หาด้วยชื่อตามลำดับ MODEL_KEYS: AnomalousVibration (ส่งลงบอร์ดจาก Edge AI Store) ก่อน
#            ถ้าบอร์ดไม่มีหรือเลือกไม่สำเร็จจึงใช้ Motion ที่ติดมากับบอร์ด · ข้อความส่งแค่ "ชื่อ" โมเดล ไม่ส่งรหัสโมเดล
#            ป้ายอันตราย (ไฟแดง + เสียง) ชุดเดียวกับ sf3_04 ไม่รวม Push: anomaly shaking sirens cough baby_cry chainsaw crackling_fire alarm
#            เปลี่ยน MODEL_KEYS เป็นโมเดลเสียง เช่น ("SirenDetection", "Siren Detection") ได้โดยไม่ต้องแก้ DANGER
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.2 ขึ้นไป · 2.4.1 ก็รันได้) · BENTO Emulator ต่อ broker สาธารณะจริง ใช้โมเดลในตัว (Shake = shaking · POTEN เกินครึ่ง = เสียงดัง)
# สัญญา MQTT: หัวข้อ .../ai = {"id": ทีม, "n": ลำดับข้อความ, "model": ชื่อโมเดล, "label": ป้าย, "conf": ความมั่นใจ %}
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM · ยังไม่แก้ = ทำงานออฟไลน์ (พิมพ์ผลลง Console แทน)

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
CLIENT_ID = "bento-ai-" + TEAM         # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
TOPIC = "bento-aiot/" + TEAM + "/ai"
MODEL_KEYS = ("AnomalousVibration", "Motion")   # ลองตามลำดับ: โมเดลจาก Store ก่อน ไม่มีหรือเลือกไม่ได้ค่อยใช้โมเดลในตัว
DANGER = ("anomaly", "shaking", "sirens", "cough", "baby_cry",   # ป้ายที่ถือว่าอันตราย (ดังเสียง + ไฟแดง)
          "chainsaw", "crackling_fire", "alarm")                # ชุดเดียวกับ sf3_04 (ไม่รวม Push ของเรดาร์)
HEARTBEAT_MS = 2000                    # ป้ายไม่เปลี่ยน ก็ยังส่งซ้ำทุกเท่านี้
GAP_MS = 200                           # ห้ามส่งถี่กว่านี้ (broker สาธารณะ ใช้ร่วมกันหลายกลุ่ม)
TICK_MS = 100                          # ถามผล AI ทุก 0.1 วินาที (จอเขียนเฉพาะตอนมีผลใหม่)
RUN_MS = 300000
KEEPALIVE_S = 30                       # บอร์ดกับ broker ทักกันทุกเท่านี้ ยิ่งสั้น ยิ่งรู้เร็วว่าสายหลุด
RETRY_MS = 5000                        # สายหลุด: รอเท่านี้แล้วลองต่อใหม่ ไม่สำเร็จก็รอนานขึ้นทีละเท่าตัว
RETRY_MAX_MS = 30000                   # แต่รอไม่เกินเท่านี้ (ไม่รุม broker ที่ใช้ร่วมกันหลายกลุ่ม)
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
def connect_broker(w, tries=3):
    # broker สาธารณะบางเครื่องไม่ตอบเป็นพัก ๆ: ลองได้ tries ครั้ง ใช้ client_id ใหม่ทุกครั้ง
    for n in range(1, tries + 1):
        if n > 1:
            note(w, "ลองต่อ broker ใหม่ %d/%d" % (n, tries), COL_WARN)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=KEEPALIVE_S,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def wifi_up():
    # WiFi ต่ออยู่และได้ IP แล้วไหม · ยังไม่ต่อ ก็ต่อเอง (เฉพาะเมื่อแก้ชื่อ Hotspot แล้ว)
    if not wifi.is_connected():
        if WIFI_SSID[:1] in ("<", ""):  # ห้ามส่งชื่อตัวอย่างให้ wifi.connect: รอบอร์ดต่อ WiFi ที่จำไว้เอง
            return False
        if not wifi.connect(WIFI_SSID, WIFI_PASS):
            return False
    return wifi.ip() != "0.0.0.0"


def go_online(w):
    # WiFi -> broker · คืน True = ออนไลน์ · False = ยังต่อไม่ได้ (เดี๋ยวลองใหม่)
    # None = ตั้งค่ายังไม่ครบ ลองใหม่ก็ไม่ได้ผล: TEAM ยังเป็น teamXX (ส่งทับกลุ่มอื่น) หรือยังไม่ตั้ง Hotspot
    if not (len(TEAM) == 6 and TEAM[:4] == "team" and TEAM[4:].isdigit() and TEAM != "team00"):
        note(w, "แก้ TEAM ก่อน (team01-team99): ออฟไลน์", COL_WARN)
        return None
    if WIFI_SSID[:1] in ("<", "") and not wifi.is_connected():
        note(w, "ตั้งชื่อ Hotspot ก่อน: ออฟไลน์", COL_WARN)
        return None
    note(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    step = "WiFi"                      # บอกให้ชัดว่าพังขั้นไหน: WiFi หรือ broker
    try:
        if wifi_up():
            step = "broker"
            if connect_broker(w):
                note(w, "ออนไลน์ " + TOPIC, COL_OK)
                return True
    except OSError:
        pass
    note(w, "ต่อ " + step + " ไม่ได้: จะลองใหม่เอง (ทำงานต่อ)", COL_WARN)
    return False


def reconnect(w):
    # ต่อใหม่หลังหลุด: ลองครั้งเดียวต่อรอบ จอจะได้ไม่ค้างนาน (ไม่ได้ก็รอรอบหน้า)
    note(w, "กำลังต่อใหม่...", COL_WARN)
    ui.poll()
    try:
        return wifi_up() and connect_broker(w, 1)
    except OSError:
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
    name, online = None, False
    try:
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
        can_retry = online is not None                 # ตั้งค่าครบแล้วเท่านั้น จึงลองต่อใหม่เมื่อหลุด
        online = bool(online)
        show_link(w, online)
        seq, lab, conf, sent_lab, n, n_ok, drops = -1, None, 0, None, 0, 0, 0
        t0 = t_sent = t_retry = time.ticks_ms()
        wait = RETRY_MS
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            r = read_result()
            new = bool(r) and r["seq"] != seq
            if new:                                        # 1) ผลใหม่จาก AI
                seq, old = r["seq"], lab
                lab, conf = r["label"] or "-", int(r["conf"] * 100)
                show_result(w, lab, conf)
                if lab in DANGER and lab != old:
                    beep(84, 76)                           # เพิ่งเป็นอันตราย: ดังครั้งเดียว
            if lab is not None and should_send(lab != sent_lab, time.ticks_diff(time.ticks_ms(), t_sent)):
                n += 1                                     # 2) ส่งเมื่อมีความหมาย หรือถึง heartbeat
                ok = send(online, {"id": TEAM, "n": n, "model": name, "label": lab, "conf": conf})
                t_sent, sent_lab = time.ticks_ms(), lab           # เวลาที่ส่งจริง (เสียงบี๊บก่อนหน้ากินเวลา ~240 ms)
                n_ok += 1 if ok else 0
                w["sent"].text("ส่งออกได้ %d จาก %d ข้อความ" % (n_ok, n))
                w["json"].text("%s %d%% n=%d" % (lab, conf, n))
                if online and not ok:                      # ส่งไม่ออก = สายมีปัญหา แม้บอร์ดยังคิดว่าต่ออยู่
                    try:
                        mqtt.disconnect()                  # ปิดสายเก่าทิ้ง แล้วข้อ 3) จะนับเป็นหลุด
                    except OSError:
                        pass
            if online and not mqtt.is_connected():         # 3) หลุด: ทำงานต่อแบบออฟไลน์ แล้วรอต่อใหม่
                online = False
                drops += 1
                show_link(w, False)
                t_retry, wait = time.ticks_ms(), RETRY_MS
                note(w, "เน็ตหลุด (ครั้งที่ %d) จะต่อใหม่ใน %d วินาที" % (drops, wait // 1000), COL_WARN)
            if not online and can_retry and time.ticks_diff(time.ticks_ms(), t_retry) >= wait:
                online = reconnect(w)                      # 4) ถึงเวลาลองต่อใหม่
                show_link(w, online)
                if online:
                    wait = RETRY_MS
                    note(w, "ต่อใหม่สำเร็จ ส่งต่อที่ n=%d (หลุดไป %d ครั้ง)" % (n + 1, drops), COL_OK)
                else:
                    t_retry, wait = time.ticks_ms(), min(wait * 2, RETRY_MAX_MS)
                    note(w, "ยังต่อไม่ได้ จะลองใหม่ใน %d วินาที" % (wait // 1000), COL_WARN)
            ui.poll()
            time.sleep_ms(TICK_MS)
    finally:
        try:
            edge_ai.stop()                                 # หยุดโมเดลเสมอ แม้โปรแกรมถูกหยุดกลางทาง
        except OSError:
            pass
        try:
            mqtt.disconnect()
        except OSError:
            pass


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ตั้ง HEARTBEAT_MS = 10000 แล้วดูในแดชบอร์ดว่าข้อความลดลงเท่าไรตอนวางบอร์ดนิ่ง
# 2) ในแอปของกลุ่ม (s2/app/) เพิ่มกฎ: ไม่ได้ข้อความจาก .../ai เกิน 5 วินาที = ขึ้นเตือน "AI เงียบ"
# 3) ส่ง "conf" เฉพาะตอนเกิน CONF 60 % หรือเพิ่มคีย์ "danger": 1/0 ให้แอปใช้ง่ายขึ้น
