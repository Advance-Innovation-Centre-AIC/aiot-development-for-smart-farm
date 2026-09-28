# sf2_05_farm_cloud_one_call.py - ฟาร์มจำที่อยู่ของแพลตฟอร์มเอง แล้วส่งข้อมูลด้วยคำสั่งเดียว
#
# ภารกิจ   : อ่าน "คลังค่าตั้ง" ที่บอร์ดเก็บไว้บนแฟลช (ไม่ต้องมีเน็ต) ถ้าผู้สอนแจกชื่อ broker ของ
#            แพลตฟอร์ม TESAIoT ให้ บอร์ดจะต่อแบบเข้ารหัส (TLS) แล้วส่งอากาศในโรงเรือน 6 ใบด้วย tesaiot.publish()
# ลองเล่น  : รันแบบ PLATFORM_BROKER ว่างก่อน จด device_id broker tls_mode จากตารางบนจอลงใบงาน
#            วันที่ได้ชื่อ broker: ใส่แล้วรัน ถอดสาย USB เสียบใหม่ ดูว่าบอร์ดยังจำได้ไหม
# ของบนบอร์ดที่ใช้ : SHT40 = อุณหภูมิ + ความชื้น · แฟลช = คลังค่าตั้ง (ถอดไฟแล้วยังอยู่)
#            จอไฟ RGB 16x8 = คำว่า CLOUD ตอนต่อติด แล้วนับใบที่ส่งสำเร็จ
#            ลำโพงดังเฉพาะตอนต่อติด และตอนส่งสำเร็จ
# บนจอ     : ตารางคลังค่าตั้ง 6 คีย์ (Table), ไฟ "ต่อติดแล้ว" (Led), วงหมุนตอนรอเน็ต (Spinner),
#            หลอดเวลารอสาย TLS 0-15 วิ (Bar), ตัวเลขใหญ่ ms ที่รอ กับจำนวนใบที่ส่ง (Seg7)
# แนวคิด AIoT: sf2_02 ต้องพิมพ์ชื่อ broker กับหัวข้อในโค้ดทุกไฟล์ คลังค่าตั้งเก็บไว้ที่บอร์ดครั้งเดียว
#            โปรแกรมทุกตัวอ่านค่าเดียวกัน และ publish() ประกอบหัวข้อให้เองจาก device_id
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator (แพลตฟอร์มใน Emulator เป็นแบบจำลอง)
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS แล้ว DEVICE_ID กับ PLATFORM_BROKER (ผู้สอนแจก ยังไม่แจกให้ว่างไว้)
# กับดัก    : tesaiot.connect() ใช้ TLS พอร์ต 8883/8884 เสมอ จึงต่อ broker.hivemq.com:1883 ไม่ได้
#            และมันคืนค่าทันที ไม่ได้แปลว่าต่อติด ต้องวนถาม is_connected() เองพร้อมกำหนดเวลาเลิกรอ

import json
import math
import rgbmatrix
import sensors
import tesaiot
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "bento-teamXX"
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
DEVICE_ID = "<device_id ที่ผู้สอนแจก>"
# ห้ามใส่ broker.hivemq.com ตรงนี้ มันจะถูกเขียนลงแฟลชแล้วรอจนหมดเวลาเปล่า ๆ
PLATFORM_BROKER = ""
TEMP_OFFSET = 0.0    # บอร์ดอุ่นจากชิปของตัวเอง: เทียบกับเทอร์โมมิเตอร์ในห้อง แล้วใส่ค่าชดเชย เช่น -7.0
                     # (ใช้ค่าเดียวกับที่กลุ่มหาได้ในคาบ 1) ตั้งแล้วความชื้นจะถูกแปลงเป็นของห้องให้เองด้วย
HUM_FIX = True       # แปลงความชื้นเป็นของห้อง (ดู room_humidity) ถ้าเทียบไฮโกรมิเตอร์แล้วสูงเกินจริง ให้ตั้ง False

WAIT_MS, POLL_MS = 15000, 250          # รอต่อติดนานสุด 15 วิ แล้วเลิกรอ ไม่รอตลอดไป
N, GAP_MS = 6, 5000                    # ส่ง 6 ใบ ห่างกันใบละ 5 วิ
# 6 คีย์ที่โชว์ในตาราง: 3 ตัวแรกค่ายาว อยู่คอลัมน์กว้าง 3 ตัวหลังเป็นตัวเลขสั้น ๆ
# port ในคลังคือค่าที่เก็บไว้ ไม่ใช่พอร์ตที่ connect() ใช้จริง พอร์ตจริงมาจาก tls_mode
SHOW = ("device_id", "broker", "tls_mode", "port", "qos", "keepalive")

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def read_climate():
    # คืน (อุณหภูมิที่ชดเชยแล้ว, อุณหภูมิดิบ, ความชื้น, ความกด) ตัวที่อ่านไม่ได้เป็น None
    # ชดเชยตรงนี้ที่เดียว ส่วนอื่นของโปรแกรมจึงได้ค่าที่แก้แล้วเสมอ
    t_raw = h = p = None
    try:
        t_raw = sensors.sht40.temperature()
        h = sensors.sht40.humidity()
    except Exception:
        pass
    try:
        p = sensors.dps368.pressure()
    except Exception:
        pass
    t = None if t_raw is None else t_raw + TEMP_OFFSET
    if h is not None and t is not None and TEMP_OFFSET != 0 and HUM_FIX:
        h = room_humidity(h, t_raw, t)
    return t, t_raw, h, p


def matrix_show(n):
    # จอไฟ RGB: หยุดตัววิ่ง (CLOUD) ก่อนเสมอ เพราะจอไฟใช้ร่วมกัน แล้วโชว์เลข n (0 = ล้างจอ)
    # เขียนเฉพาะตอนส่งสำเร็จหรือตอนจบ ไม่ใช่ทุกรอบ
    rgbmatrix.scroll("")
    if n:
        rgbmatrix.score(n, rgbmatrix.CYAN)
    else:
        rgbmatrix.clear()


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def sat_pressure(t):
    # ความดันไอน้ำอิ่มตัว (hPa) ที่อุณหภูมิ t C (สูตร Magnus)
    return 6.112 * math.exp(17.62 * t / (243.12 + t))


def room_humidity(h_raw, t_raw, t_room):
    # อากาศอุ่นขึ้นรอบเซนเซอร์ ความชื้นสัมพัทธ์จึงอ่านได้ต่ำกว่าห้อง
    # ไอน้ำในอากาศเท่าเดิม แต่ห้องเย็นกว่า จึงแปลงกลับด้วยอัตราส่วนความดันไออิ่มตัว
    return min(100.0, h_raw * sat_pressure(t_raw) / sat_pressure(t_room))


def rounded(v):
    # ปัดทศนิยม 1 ตำแหน่ง อ่านไม่ได้ (None) ก็คง None ไว้ ส่งออกไปเป็น null ใน JSON
    return None if v is None else round(v, 1)


def cfg_text(cfg, key):
    # ค่าของคีย์เป็นข้อความสั้นพอดีช่องตาราง ถ้าคลังไม่มีคีย์นี้ก็บอกตรง ๆ ไม่เดา
    return str(cfg[key])[:24] if key in cfg else "ไม่มีคีย์นี้"


def setup_problem(n_keys):
    # ยังเขียนลงแฟลชไม่ได้เพราะอะไร คืน (ข้อความ, สี) หรือ None ถ้าพร้อมแล้ว
    # PLATFORM_BROKER ว่าง = แค่มาอ่านคลัง ไม่ใช่ความผิด จึงเป็นสีส้มไม่ใช่สีแดง
    if PLATFORM_BROKER == "":
        return "อ่านได้ " + str(n_keys) + " คีย์ | ไม่ได้เขียนอะไรลงแฟลช", COL_WARN
    if DEVICE_ID[:1] == "<":
        return "ใส่ DEVICE_ID ที่ผู้สอนแจกก่อน", COL_BAD
    return None


# ---- 4) เครือข่าย ----
def save_platform():
    # เขียนทับเฉพาะสองคีย์ (ถึงตรงนี้ได้เมื่อใส่ PLATFORM_BROKER แล้วเท่านั้น)
    # แล้วอ่านคลังซ้ำ เพื่อพิสูจน์ว่าลงแฟลชจริง ไม่ใช่เชื่อว่าลง
    tesaiot.config_set("device_id", DEVICE_ID)        # รับข้อความทั้งสองช่อง
    tesaiot.config_set("broker", PLATFORM_BROKER)
    return tesaiot.config()


def join_wifi():
    # tesaiot.connect() ไม่ได้ต่อ WiFi ให้ ต้องต่อเองก่อน และต้องได้ IP จริง
    return wifi.connect(WIFI_SSID, WIFI_PASS) and wifi.ip() != "0.0.0.0"


def wait_platform(w):
    # สั่งต่อ แล้ววนถาม is_connected() ทุก POLL_MS จนติด (True) หรือรอครบ WAIT_MS (False)
    # connect() คืนทันที = "รับคำสั่งแล้ว" ไม่ใช่ "ต่อติดแล้ว" ส่งบรรทัดถัดไปเลยจะหาย
    tesaiot.connect()
    t0 = time.ticks_ms()
    while not tesaiot.is_connected():
        waited = time.ticks_diff(time.ticks_ms(), t0)
        show_wait(w, waited)
        if waited >= WAIT_MS:
            return False
        time.sleep_ms(POLL_MS)
    return True


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า (ทุกไฟล์ใช้แบบเดียวกัน)
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_config_card(w):
    # ช่องตารางบนบอร์ดมีขอบในหนา (แถวละราว 60 px) 6 แถวซ้อนกันจะล้นการ์ด
    # จึงวางเป็น 3 แถว x (คีย์, ค่า) 2 คู่ คอลัมน์ค่ากว้างไว้ให้ค่ายาว ๆ อย่าง broker
    card(12, 44, 768, 218, "tesaiot.config() บนแฟลช ถอดไฟแล้วยังอยู่")
    table = ui.Table(x=24, y=72, w=744, h=184, cols=4, rows=3)
    for col, width in enumerate((140, 330, 140, 134)):
        table.col_width(col, width)
    w["table"] = table


def build_link_card(w):
    card(12, 268, 390, 70, "สาย TLS: รอมาแล้วกี่ ms")
    w["led"] = ui.Led(x=24, y=298, w=34, h=34, color=COL_OK)     # 0 = หรี่ = ยังไม่ติด
    w["spin"] = ui.Spinner(x=70, y=296, w=38, h=38)
    w["bar"] = ui.Bar(x=120, y=309, w=140, h=14, min=0, max=WAIT_MS)
    w["bar"].color(COL_WARN)           # สีตอนสร้างใช้ไม่ได้กับ Bar ต้องตั้งหลังสร้าง
    w["seg_ms"] = ui.Seg7(text="0", x=272, y=296, w=120, h=38, color=COL_INFO)


def build_sent_card(w):
    card(412, 268, 368, 70, "tesaiot.publish() ส่งแล้ว (ใบ)")
    w["seg_sent"] = ui.Seg7(text="0", x=424, y=296, w=70, h=38, color=COL_OK)
    w["note"] = ui.Label("ยังไม่ได้ส่ง", x=506, y=306, color=COL_DIM, value=14)


def build_screen():
    # สร้างทุกอย่างบนจอครั้งเดียว แล้วคืน dict ของ widget ที่ต้องอัปเดตภายหลัง
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ฟาร์มจำที่อยู่คลาวด์เอง", x=12, y=6, color=COL_TEXT, value=24)
    w = {"status": ui.Label("กำลังอ่านคลังค่าตั้ง", x=12, y=352, color=COL_DIM)}
    build_config_card(w)
    build_link_card(w)
    build_sent_card(w)
    w["spin"].hide()                   # วงหมุนโผล่เฉพาะตอนรอเน็ต
    ui.poll()
    return w


def show_status(w, msg, col):
    # ไฟล์นี้ไม่มีอะไรให้แตะบนจอ จึงเรียก ui.poll() ในนี้ได้ ป้ายต้องขึ้นก่อนบรรทัดที่บล็อก
    w["status"].color(col)
    w["status"].text(msg)
    ui.poll()


def show_cfg(w, cfg):
    # คอลัมน์ 0-1 = 3 คีย์แรกของ SHOW, คอลัมน์ 2-3 = 3 คีย์หลัง
    for i in range(len(SHOW)):
        r, c = i % 3, i // 3 * 2
        w["table"].cell(r, c, SHOW[i])
        w["table"].cell(r, c + 1, cfg_text(cfg, SHOW[i]))
    ui.poll()


def show_wait(w, waited):
    # ตัวเลข ms กับหลอดเวลาโตขึ้นทุกครั้งที่ถาม หลอดเต็ม = ครบ WAIT_MS = เลิกรอ
    w["seg_ms"].text(str(waited))
    w["bar"].value(min(waited, WAIT_MS))
    ui.poll()


def mark_link(w, col):
    # ระบายสีตัวเลขกับหลอดเวลารอ: เขียว = ต่อติด, แดง = รอจนหมดเวลา
    w["seg_ms"].color(col)
    w["bar"].color(col)


def show_online(w):
    mark_link(w, COL_OK)
    w["spin"].hide()
    w["led"].value(1)
    show_status(w, "ต่อแพลตฟอร์มติดแล้ว", COL_OK)
    rgbmatrix.scroll("CLOUD", rgbmatrix.CYAN, 80)
    ui.sfx(ui.SFX_UI_START)


def show_sent(w, i, sent, t, h, ok):
    # ส่งสำเร็จ: ตัวนับ + จอไฟ RGB + เสียง (เฉพาะตอนสำเร็จ) ถูกปฏิเสธ: บอกเป็นสีส้ม
    if ok:
        w["seg_sent"].text(str(sent))
        matrix_show(sent)
        ui.sfx(ui.SFX_FLAPPY_SCORE)
        w["note"].color(COL_TEXT)
        w["note"].text("ใบที่ " + str(i) + " temp_c=" + str(t) + " rh=" + str(h))
    else:
        w["note"].color(COL_WARN)
        w["note"].text("ใบที่ " + str(i) + " ถูกปฏิเสธ")
    ui.poll()


# ---- 6) โปรแกรมหลัก ----
def stop(w, msg, col=COL_BAD):
    # จบเพราะอะไรก็ตาม: ล้างจอไฟ RGB ดับไฟ "ต่อติด" แล้วบอกเหตุผลทั้งบนจอและคอนโซล
    matrix_show(0)
    w["spin"].hide()
    w["led"].value(0)
    show_status(w, msg, col)
    print("หยุดที่:", msg)
    raise SystemExit


def send_all(w):
    # ส่ง N ใบด้วยคำสั่งเดียว ไม่ต้องบอกหัวข้อ เฟิร์มแวร์ประกอบให้จาก device_id
    # คืนจำนวนใบที่ส่งสำเร็จ (ใบที่ถูกปฏิเสธไม่นับ)
    sent = 0
    for i in range(1, N + 1):
        t, _, h, _ = read_climate()
        t, h = rounded(t), rounded(h)
        if not tesaiot.is_connected():
            stop(w, "สายหลุดที่ใบที่ " + str(i))
        ok = tesaiot.publish(json.dumps({"n": i, "temp_c": t, "rh": h}))
        if ok:
            sent += 1
        show_sent(w, i, sent, t, h, ok)
        time.sleep_ms(GAP_MS)
    return sent


def main():
    w = build_screen()
    cfg = tesaiot.config()             # 1) อ่านคลังค่าตั้ง: ไม่ต้องต่อเน็ต อ่านได้ทันที
    show_cfg(w, cfg)
    print("คลังค่าตั้งมี", len(cfg), "คีย์")
    problem = setup_problem(len(cfg))
    if problem:
        stop(w, problem[0], problem[1])
    show_cfg(w, save_platform())       # 2) เขียนทับสองคีย์ แล้วโชว์ค่าที่อ่านกลับมาจากแฟลช
    show_status(w, "กำลังต่อ WiFi จอจะนิ่งสักครู่", COL_WARN)
    w["spin"].show()
    if not join_wifi():                # 3) WiFi ก่อน แพลตฟอร์มทีหลัง
        stop(w, "ต่อ WiFi ไม่ได้ ค่าที่ตั้งไว้ยังอยู่บนแฟลช")
    show_status(w, "สั่งต่อแล้ว กำลังรอสาย TLS", COL_WARN)
    if not wait_platform(w):           # 4) วนถามเองพร้อมกำหนดเวลาเลิกรอ
        mark_link(w, COL_BAD)
        stop(w, "รอ " + str(WAIT_MS // 1000) + " วิ ยังไม่ติด ตรวจ broker/ใบรับรอง")
    show_online(w)
    sent = send_all(w)                 # 5) ส่งด้วยคำสั่งเดียว
    show_status(w, "จบ ส่งได้ " + str(sent) + " ใบ จาก " + str(N), COL_OK)


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ถอดสาย USB เสียบใหม่ แล้วพิมพ์ใน Playground: import tesaiot; print(tesaiot.config()["broker"])
#    ได้ชื่อที่ตั้งไว้ หรือกลับเป็นค่าโรงงาน (ถ้าอยากล้างจริง ๆ มี tesaiot.config_reset())
# 2) เทียบจำนวนบรรทัดที่ใช้ส่งหนึ่งใบในไฟล์นี้ (ดู send_all) กับใน sf2_02 แล้วเขียนว่าอะไรหายไปอยู่ในคลังค่าตั้ง
# 3) ลด WAIT_MS เหลือ 1000 แล้วรันใหม่ หลอดเวลาเต็มก่อนสายติดไหม ตัวเลข ms ตอนติดจริงคือเท่าไร
#    ทำไมงานจริงต้องมีเวลาเลิกรอ แต่ต้องไม่สั้นกว่าเวลาจับมือ TLS
