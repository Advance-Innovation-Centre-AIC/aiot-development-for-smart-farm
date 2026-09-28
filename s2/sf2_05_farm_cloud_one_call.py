# sf2_05_farm_cloud_one_call.py - ฟาร์มจำที่อยู่ของแพลตฟอร์มเอง แล้วส่งข้อมูลด้วยคำสั่งเดียว
#
# ภารกิจ   : อ่าน "คลังค่าตั้ง" ที่บอร์ดเก็บไว้บนแฟลช (ไม่ต้องมีเน็ต) ถ้าผู้สอนแจกชื่อ broker ของ
#            แพลตฟอร์ม TESAIoT ให้ บอร์ดจะต่อแบบเข้ารหัส แล้วส่งอากาศในโรงเรือนด้วย tesaiot.publish()
# ลองเล่น  : รันแบบ PLATFORM_BROKER ว่างก่อน จด device_id broker tls_mode ลงใบงาน
#            วันที่ได้ชื่อ broker: ใส่แล้วรัน ถอดสาย USB เสียบใหม่ ดูว่าบอร์ดยังจำได้ไหม
# แนวคิด AIoT: sf2_02 ต้องพิมพ์ชื่อ broker กับหัวข้อในโค้ดทุกไฟล์ คลังค่าตั้งเก็บไว้ที่บอร์ดครั้งเดียว
#            โปรแกรมทุกตัวอ่านค่าเดียวกัน และ publish() ประกอบหัวข้อให้เองจาก device_id
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS แล้ว DEVICE_ID กับ PLATFORM_BROKER (ผู้สอนแจก ยังไม่แจกให้ว่างไว้)
# กับดัก    : tesaiot.connect() ใช้ TLS พอร์ต 8883/8884 เสมอ จึงต่อ broker.hivemq.com:1883 ไม่ได้
#            และมันคืนค่าทันที ไม่ได้แปลว่าต่อติด ต้องวนถาม is_connected() เองพร้อมกำหนดเวลาเลิกรอ
# ที่มา     : ย่อจากคอร์สแม่ examples/s02/07_platform_in_one_call.py

import json
import rgbmatrix
import sensors
import tesaiot
import time
import ui
import wifi

WIFI_SSID = "bento-teamXX"
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
DEVICE_ID = "<device_id ที่ผู้สอนแจก>"
# ห้ามใส่ broker.hivemq.com ตรงนี้ มันจะถูกเขียนลงแฟลชแล้วรอจนหมดเวลาเปล่า ๆ
PLATFORM_BROKER = ""

WAIT_MS, POLL_MS = 15000, 250          # รอต่อติดนานสุด 15 วิ แล้วเลิกรอ ไม่รอตลอดไป
N, GAP_MS = 6, 5000                    # ส่ง 6 ใบ ห่างกันใบละ 5 วิ
SHOW = ("device_id", "broker", "port", "tls_mode", "qos", "keepalive")

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF

ui.screen()
time.sleep_ms(200)
ui.Label("ฟาร์มจำที่อยู่คลาวด์เอง", x=20, y=12, color=COL_TEXT, value=24)
status = ui.Label("กำลังอ่านคลังค่าตั้ง", x=20, y=48, color=COL_DIM, value=20)
ui.Panel(x=20, y=80, w=760, h=120, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("tesaiot.config()", x=36, y=88, color=COL_INFO, value=16)
cells = []                             # ห้ามสร้าง Label ด้วยข้อความว่าง จึงใส่ "รออ่าน" ไว้ก่อน
for i in range(6):
    cells.append(ui.Label("รออ่าน", x=36 + (i // 3) * 372, y=116 + (i % 3) * 28,
                          color=COL_TEXT, value=20))
ui.Label("รอต่อมาแล้ว (ms)", x=20, y=216, color=COL_DIM, value=16)
seg = ui.Seg7(text="0", x=20, y=244, w=180, h=52, color=COL_INFO)
ui.Label("ส่งแล้ว (ใบ)", x=240, y=216, color=COL_DIM, value=16)
seg_sent = ui.Seg7(text="0", x=240, y=244, w=120, h=52, color=COL_OK)
note = ui.Label("ตั้งครั้งเดียว อยู่บนแฟลช ถอดไฟแล้วยังอยู่", x=20, y=316, color=COL_DIM, value=16)
ui.poll()


def show(msg, col):
    status.color(col)
    status.text(msg)
    ui.poll()


def stop(msg, col=COL_BAD):
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    show(msg, col)
    print("หยุดที่:", msg)
    raise SystemExit


def show_cfg(cfg):
    for i in range(len(SHOW)):
        key = SHOW[i]
        cells[i].color(COL_TEXT if key in cfg else COL_DIM)
        cells[i].text(key + " = " + (str(cfg[key])[:24] if key in cfg else "ไม่มีคีย์นี้"))
    ui.poll()


# --- 1) อ่านคลังค่าตั้ง: ไม่ต้องต่อเน็ต อ่านได้ทันที ---
cfg = tesaiot.config()
show_cfg(cfg)
print("คลังค่าตั้งมี", len(cfg), "คีย์")
if PLATFORM_BROKER == "":
    # port ในคลังคือค่าที่เก็บไว้ ไม่ใช่พอร์ตที่ connect() ใช้จริง พอร์ตจริงมาจาก tls_mode
    stop("อ่านได้ " + str(len(cfg)) + " คีย์ | ไม่ได้เขียนอะไรลงแฟลช", COL_WARN)
if DEVICE_ID[:1] == "<":
    stop("ใส่ DEVICE_ID ที่ผู้สอนแจกก่อน")

# --- 2) เขียนทับเฉพาะสองคีย์ (ถึงตรงนี้ได้เมื่อใส่ PLATFORM_BROKER แล้วเท่านั้น) ---
tesaiot.config_set("device_id", DEVICE_ID)        # รับข้อความทั้งสองช่อง
tesaiot.config_set("broker", PLATFORM_BROKER)
cfg = tesaiot.config()                 # อ่านซ้ำเพื่อพิสูจน์ว่าลงจริง ไม่ใช่เชื่อว่าลง
show_cfg(cfg)

# --- 3) ต่อ WiFi ก่อน tesaiot.connect() ไม่ได้ต่อ WiFi ให้ ---
show("กำลังต่อ WiFi จอจะนิ่งสักครู่", COL_WARN)
if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
    stop("ต่อ WiFi ไม่ได้ ค่าที่ตั้งไว้ยังอยู่บนแฟลช")

# --- 4) สั่งต่อ แล้ววนถามเองพร้อมกำหนดเวลาเลิกรอ ---
show("สั่งต่อแล้ว กำลังรอสาย TLS", COL_WARN)
tesaiot.connect()                      # คืนทันที = "รับคำสั่งแล้ว" ไม่ใช่ "ต่อติดแล้ว"
t0 = time.ticks_ms()
while not tesaiot.is_connected():
    waited = time.ticks_diff(time.ticks_ms(), t0)
    seg.text(str(waited))
    if waited >= WAIT_MS:
        seg.color(COL_BAD)
        stop("รอ " + str(WAIT_MS // 1000) + " วิ ยังไม่ติด ตรวจ broker/ใบรับรอง")
    ui.poll()
    time.sleep_ms(POLL_MS)
seg.color(COL_OK)
show("ต่อแพลตฟอร์มติดแล้ว", COL_OK)
rgbmatrix.scroll("CLOUD", rgbmatrix.CYAN, 80)
ui.sfx(ui.SFX_UI_START)

# --- 5) ส่งด้วยคำสั่งเดียว ไม่ต้องบอกหัวข้อ เฟิร์มแวร์ประกอบให้จาก device_id ---
sent = 0
for i in range(1, N + 1):
    try:
        t, h = round(sensors.sht40.temperature(), 1), round(sensors.sht40.humidity(), 1)
    except Exception:
        t = h = None
    if not tesaiot.is_connected():
        stop("สายหลุดที่ใบที่ " + str(i))
    if tesaiot.publish(json.dumps({"n": i, "temp_c": t, "rh": h})):
        sent = i
        seg_sent.text(str(sent))
        rgbmatrix.scroll("")
        rgbmatrix.score(sent, rgbmatrix.CYAN)        # เขียนจอ LED เฉพาะตอนส่งสำเร็จ
        ui.sfx(ui.SFX_FLAPPY_SCORE)
        note.text("ใบที่ " + str(i) + " temp_c=" + str(t) + " rh=" + str(h))
    else:
        note.color(COL_WARN)
        note.text("ใบที่ " + str(i) + " ถูกปฏิเสธ")
    ui.poll()
    time.sleep_ms(GAP_MS)

show("จบ ส่งได้ " + str(sent) + " ใบ จาก " + str(N), COL_OK)

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ถอดสาย USB เสียบใหม่ แล้วพิมพ์ใน Playground: import tesaiot; print(tesaiot.config()["broker"])
#    ได้ชื่อที่ตั้งไว้ หรือกลับเป็นค่าโรงงาน (ถ้าอยากล้างจริง ๆ มี tesaiot.config_reset())
# 2) เทียบจำนวนบรรทัดที่ใช้ส่งหนึ่งใบในไฟล์นี้ กับใน sf2_02 แล้วเขียนว่าอะไรหายไปอยู่ในคลังค่าตั้ง
