# cp3_05_two_pipes.py - หลักการ 3.5: ค่าเดียวกัน สองท่อ (MQTT ธรรมดา 1883 เทียบกับ MQTTS/TLS 8883-8884)
#
# หลักการ  : ท่อ 1 = mqtt พอร์ต 1883 ไม่เข้ารหัส ข้อความวิ่งเป็นตัวอักษรตรง ๆ ใครอยู่บนทาง (WiFi เดียวกัน ผู้ให้บริการ
#            broker สาธารณะ) อ่านได้ทุกตัว และปลอมตัวเป็น broker ได้ ใช้ได้แค่งานทดลองที่ไม่มีของลับ
#            ท่อ 2 = MQTTS (MQTT บน TLS): เข้ารหัสทั้งทาง + บอร์ดตรวจบัตรของ broker ว่าเป็นตัวจริง
#            ถ้าเป็นโหมด mTLS บอร์ดก็โชว์บัตรของตัวเองด้วย (กุญแจลับเก็บในฮาร์ดแวร์ความปลอดภัยตามที่ชื่อโหมดบอก)
# ความจริง : บนบอร์ดนี้ โมดูล mqtt ต่อได้แบบไม่เข้ารหัสเท่านั้น (ไม่มี TLS) ท่อที่เข้ารหัสมีทางเดียวคือโมดูล tesaiot
#            ซึ่งต่อได้เฉพาะ broker ของแพลตฟอร์ม TESAIoT ตามค่าที่เก็บในบอร์ด (tesaiot.config())
#            พอร์ตที่ต่อจริงเลือกจาก tls_mode ไม่ใช่จากค่า port ในคลังค่าตั้ง: โหมด mTLS = 8883, serverTLS = 8884
#            ไฟล์นี้อ่านคลังค่าตั้งแบบไม่ต่อเน็ต และโชว์แค่ tls_mode, broker, พอร์ต ไม่โชว์เลขเครื่องหรือกุญแจใด ๆ
# ลองเล่น  : ไม่ตั้ง WiFi = เห็นใบที่ "จะ" วิ่งในท่อ 1 ทีละตัวอักษร + ค่าตั้งของท่อ 2 (ไม่ต้องมีเน็ต)
#            ตั้ง WiFi + TEAM = ท่อ 1 ส่งจริงทุก SEND_MS ดูด้วย MQTT Explorer (bento-aiot/<TEAM>/core/pipes) แล้วถามว่า
#            "ถ้าเป็นรหัสผ่าน คนข้างโต๊ะเห็นไหม" · หมุน VR1 = เปลี่ยนค่าที่ส่ง
# ท่อ 2    : ไฟล์นี้ "อ่าน" ค่าตั้งของท่อ 2 เท่านั้น ไม่ต่อ ไม่ส่ง ท่อ 2 ของจริงต้องใช้บอร์ดที่ลงทะเบียนกับแพลตฟอร์ม
#            TESAIoT แล้ว ซึ่งเป็นงานของผู้สอน ไม่ใช่ของไฟล์ตัวอย่าง
#            ลำดับอาร์กิวเมนต์ต่างกัน: mqtt.publish(หัวข้อ, ข้อความ) แต่ tesaiot.publish(ข้อความ) หัวข้อประกอบให้เอง
# ของบนบอร์ด: VR1 = ค่าที่ส่ง 0-100 % - ลำโพงดังตอนต่อติด ตอนพัง และตอนจบ
# ในฟาร์ม  : ค่าดินส่งแบบเปิดก็ได้ แต่คำสั่งเปิดปั๊มและรหัสอุปกรณ์ต้องไปท่อที่เข้ารหัสและตรวจตัวตนเสมอ
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (Emulator จำลอง mqtt และคลังค่าตั้งของ tesaiot ในเบราว์เซอร์)

import json
import mqtt
import pots
import tesaiot
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ไม่แก้ = ไม่ส่ง โชว์ใบที่จะส่งกับค่าตั้งของท่อ 2 อย่างเดียว
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว - อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                       # เลขกลุ่มที่ผู้สอนแจก เช่น team05
SEND_MS = 5000       # ส่งค่าทุกกี่ ms
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้

BROKER = "broker.hivemq.com"          # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
CLIENT_ID = "bento-pipe-" + TEAM      # ต้องไม่ซ้ำกับใครบน broker
TOPIC = "bento-aiot/" + TEAM + "/core/pipes"

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


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
# (และไม่แตะเน็ต: ทดสอบได้โดยไม่ต้องมี WiFi)
def pipe2_rows(cfg):
    # คลังค่าตั้ง -> 3 แถวที่โชว์ได้ (โหมด, broker, พอร์ตที่ต่อจริง) ไม่แตะคีย์ที่เป็นเลขเครื่องหรือกุญแจ
    # พอร์ตเลือกจากโหมดแบบเดียวกับเฟิร์มแวร์: mTLS ทั้งสองแบบ = 8883, นอกนั้น = 8884
    mode = cfg["tls_mode"]
    return mode, cfg["broker"], 8883 if mode[:4] == "mTLS" else 8884


# ---- 4) เครือข่าย ----
def read_config():
    # อ่านคลังค่าตั้งของ tesaiot จากแฟลช ไม่ต่อเน็ต อ่านไม่ได้ (เช่น ไม่มีคลังนี้) = ขีด
    try:
        return pipe2_rows(tesaiot.config())
    except Exception:
        return "--", "--", "--"


def connect_plain(w):
    # ท่อ 1: WiFi -> IP -> broker พอร์ต 1883 คืน "" ถ้าผ่าน ไม่งั้นบอกว่าพังตรงไหน
    if not TEAM[4:].isdigit():                    # TEAM ยังเป็น teamXX = ไม่ต่อ ไม่ส่งอะไรออกไปเลย
        return "แก้ TEAM ก่อน"
    show(w, "s1", "ต่อ WiFi...", COL_WARN)       # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก (show เรียก ui.poll ให้)
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    try:
        linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        linked = False
    return "" if linked else "broker ไม่ตอบ"


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen(rows):
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ค่าเดียวกัน สองท่อ", x=12, y=6, color=COL_TEXT, value=24)
    card(12, 44, 380, 294, "ท่อ 1: mqtt พอร์ต 1883")
    ui.Label("ไม่เข้ารหัส: ใครก็อ่านได้", x=28, y=76, color=COL_BAD)
    ui.Label("ใบที่วิ่งในสาย (ตัวอักษรตรง ๆ):", x=28, y=116, color=COL_DIM, value=16)
    w = {"p1": ui.Label(" ", x=28, y=144, color=COL_TEXT, value=16)}
    w["s1"] = ui.Label(" ", x=28, y=290, color=COL_DIM, value=16)
    card(402, 44, 378, 294, "ท่อ 2: tesaiot (TLS)")
    ui.Label("เข้ารหัส + ตรวจบัตร broker", x=418, y=76, color=COL_OK)
    for i in range(3):                            # ค่าจาก tesaiot.config() อ่านจากแฟลช ไม่ต่อเน็ต
        ui.Label(("tls_mode", "broker", "port")[i], x=418, y=116 + i * 50, color=COL_DIM, value=14)
        ui.Label(str(rows[i]), x=418, y=134 + i * 50, color=COL_TEXT, value=14)   # ชื่อโหมด mTLS ยาว 47 ตัว
    ui.Label("ไฟล์นี้อ่านค่าตั้งอย่างเดียว ไม่ต่อ", x=418, y=290, color=COL_DIM, value=16)
    w["note"] = ui.Label("หมุน VR1 = ค่าที่ส่ง", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, key, text, col):
    # เปลี่ยนป้ายหนึ่งป้าย (สี + ข้อความ) แล้วส่งขึ้นจอ เรียกเฉพาะตอนมีเหตุการณ์
    w[key].color(col)
    w[key].text(text)
    ui.poll()


# ---- 6) โปรแกรมหลัก ----
def main():
    w = build_screen(read_config())
    problem = connect_plain(w) if WIFI_SSID[0] != "<" else "ยังไม่ตั้ง WiFi: ไม่ส่ง"
    show(w, "s1", problem or TOPIC, COL_WARN if problem else COL_OK)
    beep("bad" if problem else "start")
    n = n1 = 0
    t0 = t_send = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        if n == 0 or time.ticks_diff(now, t_send) >= SEND_MS:     # ใบแรกออกทันที
            t_send, n = now, n + 1
            body = json.dumps({"id": TEAM, "n": n, "soil": pots.read(0) * 100 // 4095})
            w["p1"].text(body)                    # ใบเดียวกันนี้ ถ้าไปท่อ 2 จะถูกเข้ารหัสก่อนออกจากบอร์ด
            try:
                if not problem and mqtt.publish(TOPIC, body):
                    n1 += 1
                    w["s1"].text("ส่งแล้ว %d ใบ (อ่านได้ทุกตัว)" % n1)
            except OSError:                       # สายหลุด: publish โยน OSError
                problem = "สายหลุด"
                show(w, "s1", problem, COL_BAD)
                beep("bad")
            ui.poll()
        time.sleep_ms(100)
    show(w, "note", "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่", COL_WARN)
    beep("good")


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: ใบในท่อ 1 ยาวกี่ตัวอักษร ถ้าใส่ "pin": "1234" เพิ่มเข้าไป ใครบ้างที่อ่านเห็น
# 2) ดูแถว port ของท่อ 2 แล้วตอบ: ทำไมไม่ใช่ 1883 และทำไมโหมด mTLS กับ serverTLS ใช้คนละพอร์ต
# 3) ท่อ 2 ตรวจบัตรของ broker ถ้าไม่ตรวจ (เข้ารหัสอย่างเดียว) จะโดนหลอกแบบไหนได้บ้าง
