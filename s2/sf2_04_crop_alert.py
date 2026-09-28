# sf2_04_crop_alert.py - พืชไม่สบายเมื่อไร มือถือเจ้าของฟาร์มรู้ทันที
#
# ภารกิจ   : ตัดสินทุกวินาทีว่าพืช "สบาย / เริ่มเครียด / แย่แล้ว" (กฎเดียวกับ sf1_02)
#            ส่ง "แจ้งเตือน" ออกไปทันทีเฉพาะตอนที่สถานะเปลี่ยน ไม่ใช่ทุกวินาที
#            ถ้าแย่แล้ว บอร์ดร้องซ้ำจนกว่าจะมีคนรับทราบ: {"cmd":"ack"} จากมือถือ หรือกด SW4 ที่ฟาร์ม
#            แอปของกลุ่มเปลี่ยนเกณฑ์ร้อนได้จากที่ไกลด้วย {"cmd":"set","t_hi":28}
# ลองเล่น  : หมุน VR3 (แดดจำลอง บวกอุณหภูมิ 0-10 C) จนแจ้งเตือนขึ้นบนมือถือและจอ LED 16x8
#            แล้วส่ง {"cmd":"ack"} จากช่อง JSON ในหน้ารวม mqtt_dashboard.html ให้บอร์ดเงียบ
# แนวคิด AIoT: Sense -> Decide -> Alert  ส่งเมื่อ "เปลี่ยน" คนรับจะได้ไม่ชินจนเมินแจ้งเตือน
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM และ CROP (broker ตั้งไว้แล้ว ไม่ต้องแก้)
# ภารกิจกลุ่ม: เติมคำสั่งเปิดปั๊มจาก sf2_03 ตรงจุด ">>> ภารกิจกลุ่ม" ในลูป (ดูใบงาน)

import buttons
import json
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ----- แก้สี่บรรทัดนี้ -----
WIFI_SSID = "bento-teamXX"
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"
TEAM = "teamXX"                        # team01 ถึง team20
CROP = "มะเขือเทศ"                     # พืชของกลุ่ม ต้องมีใน CROPS

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"   # หน้าเว็บโชว์หัวข้อนี้ในกล่อง event
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"

# (รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง) ตัวเลขเพื่อการเรียน ไม่ใช่คำแนะนำเกษตร
CROPS = {
    "มะเขือเทศ": ("tomato", 20, 30, 60, 80),
    "ผักสลัด": ("lettuce", 15, 25, 50, 70),
    "เห็ดนางฟ้า": ("mushroom", 22, 28, 80, 95),
    "กล้วยไม้": ("orchid", 22, 32, 60, 80),
}
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 100, 600000
# จอ LED 16x8 รับแต่อักษรอังกฤษกับตัวเลข สีมี OFF RED GREEN YELLOW BLUE PURPLE CYAN WHITE
MX = (("OK", rgbmatrix.GREEN), ("WARN", rgbmatrix.YELLOW), ("ALERT", rgbmatrix.RED))

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
code, t_lo, t_hi, h_lo, h_hi = CROPS[CROP]
SW4 = buttons.name(0)                  # ถามชื่อปุ่มจากเฟิร์มแวร์ ไม่พิมพ์เอง


def judge(t, h):
    """คืน (ระดับ 0-2, ปัญหาภาษาไทยไว้ขึ้นจอ, รหัสอังกฤษไว้ส่งให้เครื่องอ่าน)"""
    p = []
    if t < t_lo:
        p.append(("หนาวไป", "cold"))
    if t > t_hi:
        p.append(("ร้อนไป", "hot"))
    if h < h_lo:
        p.append(("แห้งไป", "dry"))
    if h > h_hi:
        p.append(("ชื้นไป", "wet"))
    if not p:
        return 0, "สบายดี", "ok"
    far = (t < t_lo - 3) or (t > t_hi + 3) or (h < h_lo - 10) or (h > h_hi + 10)
    return (2 if far else 1), " + ".join(x[0] for x in p), "+".join(x[1] for x in p)


def send(topic, obj):
    try:
        return mqtt.publish(topic, json.dumps(obj))
    except OSError:                    # publish ตอนสายหลุดโยน OSError ไม่ใช่คืน False
        stop("สายหลุดตอนส่ง")


ui.screen()
time.sleep_ms(200)
ui.Label("พืชไม่สบาย มือถือรู้ทันที", x=20, y=12, color=COL_TEXT, value=24)
status = ui.Label("กำลังเริ่ม", x=380, y=18, color=COL_DIM, value=18)
ui.Panel(x=20, y=52, w=420, h=180, color=COL_CARD, min=COL_DIM, max=12, value=1)
crop_lbl = ui.Label("-", x=36, y=60, color=COL_INFO, value=16)
lbl_th = ui.Label("-- C  -- %", x=36, y=92, color=COL_TEXT, value=24)
mood = ui.Label("...", x=36, y=140, color=COL_WARN, value=24)
ui.Panel(x=456, y=52, w=324, h=180, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("แจ้งสถานะไปแล้ว (ครั้ง)", x=472, y=60, color=COL_DIM, value=16)
seg_alert = ui.Seg7(text="0", x=472, y=88, w=144, h=52, color=COL_WARN)
ack_lbl = ui.Label("ยังไม่มีแจ้งเตือน", x=472, y=168, color=COL_DIM, value=20)
ui.Label("VR3 = แดดจำลอง   " + SW4 + " = รับทราบที่ฟาร์ม", x=20, y=252,
         color=COL_DIM, value=20)
ui.Label("ส่ง " + TOPIC_EVENT + "  ฟัง /cmd", x=20, y=300, color=COL_DIM, value=16)
ui.poll()


def show_crop():
    crop_lbl.text(CROP + " ชอบ " + str(t_lo) + "-" + str(t_hi) + " C, " + str(h_lo) + "-" +
                  str(h_hi) + " %")


def show(msg, col):
    status.color(col)
    status.text(msg)
    ui.poll()


def stop(msg, col=COL_BAD):
    rgbmatrix.scroll("")               # หยุดตัววิ่งก่อน ไม่งั้นมันวิ่งต่อหลังโปรแกรมจบ
    rgbmatrix.clear()
    show(msg, col)
    raise SystemExit


show_crop()
if len(TEAM) != 6 or TEAM[:4] != "team" or not TEAM[4:].isdigit() or TEAM == "team00":
    stop("แก้ TEAM เป็นเลขกลุ่มก่อน")
show("กำลังต่อ WiFi รอสักครู่", COL_WARN)
if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
    stop("ต่อ WiFi ไม่ได้ ตรวจชื่อวง/รหัส")
try:
    linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
except OSError:
    linked = False
if not linked or not mqtt.subscribe(TOPIC_CMD):
    stop("broker ไม่ตอบ เน็ตกันพอร์ต 1883?")
show("เฝ้าพืชอยู่", COL_OK)

level, acked, alerts = -1, True, 0   # -1 = ยังไม่รู้ รอบแรกจึงแจ้งเสมอ (บอกว่าฟาร์มออนไลน์)
t, h, sun, prev = None, None, 0, False
t_read = t_beep = t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
    now = time.ticks_ms()
    ack_now = False
    msg = mqtt.get_message()           # ฟังทุก 100 ms แม้จะอ่านเซนเซอร์แค่ทุกวินาที
    if msg is not None:
        try:
            cmd = json.loads(msg[1].decode())
        except ValueError:
            cmd = None
        act = cmd.get("cmd", "") if isinstance(cmd, dict) else "?"
        if act == "ack":
            ack_now = True
        elif act == "set":             # เกณฑ์ใหม่จากที่ไกล ต้องเป็นเลขจำนวนเต็มในช่วงที่สมเหตุผล
            v = cmd.get("t_hi")
            if isinstance(v, int) and t_lo < v <= 45:
                t_hi, level, t = v, -1, None     # level -1 + t None = ตัดสินใหม่และแจ้งทันที
                show_crop()
                ui.sfx(ui.SFX_UI_SELECT)
        # >>> ภารกิจกลุ่ม: วาง elif สำหรับ "led"/"pump" จาก sf2_03 ตรงนี้ <<<
    b = buttons.pressed(0)
    if b and not prev:                 # SW4 = คนที่ยืนอยู่หน้าฟาร์มกดรับทราบเอง ไม่ต้องพึ่งเน็ต
        ack_now = True
    prev = b
    if ack_now and not acked:
        acked = True
        ack_lbl.color(COL_OK)
        ack_lbl.text("มีคนรับทราบแล้ว")
        rgbmatrix.scroll("ACK", rgbmatrix.CYAN, 80)
        ui.sfx(ui.SFX_UI_SELECT)

    # t เป็น None = ยังไม่เคยอ่านได้ อ่านทันทีไม่ต้องรอครบวินาที
    if t is None or time.ticks_diff(now, t_read) >= READ_MS:
        t_read = now
        try:
            sun = pots.read(2) * 10 // 4095      # VR3 = แดดจำลอง 0-10 C
        except Exception:
            sun = 0
        try:
            t = sensors.sht40.temperature() + sun
            h = sensors.sht40.humidity()
        except Exception:
            t = None
        if t is not None:
            new, why_th, why_en = judge(t, h)
            lbl_th.text("%.1f C  %.1f %%  (แดด +%d)" % (t, h, sun))
            mood.color((COL_OK, COL_WARN, COL_BAD)[new])
            mood.text(("สบายดี :)", "เริ่มเครียด: ", "แย่แล้ว!: ")[new] + ("" if new == 0 else why_th))
            if new != level:           # ส่ง เล่นเสียง และเขียนจอ LED เฉพาะตอนเปลี่ยน
                level, acked = new, new < 2
                alerts += 1
                seg_alert.text(str(alerts))
                send(TOPIC_EVENT, {"id": TEAM, "crop": code, "level": level, "alert": why_en,
                                   "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun})
                rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
                ui.sfx((ui.SFX_FLAPPY_SCORE, ui.SFX_UI_MOVE, ui.SFX_GAME_OVER)[level])
                ack_lbl.color(COL_BAD if level == 2 else COL_DIM)
                ack_lbl.text("รอคนรับทราบ" if level == 2 else "ส่งแจ้งสถานะแล้ว")
                t_beep = now
    if level == 2 and not acked and time.ticks_diff(now, t_beep) >= BEEP_MS:
        t_beep = now                   # ร้องซ้ำทุก 5 วิ ไม่ใช่ทุกรอบลูป ห้องมี 20 บอร์ด
        ui.tone(69, ui.WAVE_SQUARE, 90, 150)
    ui.poll()
    time.sleep_ms(POLL_MS)

stop("จบ แจ้งเตือน " + str(alerts) + " ครั้ง", COL_DIM)

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) หมุนแดดขึ้นลงเร็ว ๆ ข้ามเส้นไปมา นับว่าแจ้งเตือนรัวแค่ไหน แล้วแก้ให้ต้อง "แย่ติดกัน 5 วินาที" ก่อนแจ้ง
# 2) แจ้งเตือนมี sun_c บอกว่าบวกแดดจำลองไปเท่าไร ถ้าไม่บอก ข้อมูลฟาร์มที่เก็บไว้จะเสียอย่างไร
