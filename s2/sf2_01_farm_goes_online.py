# sf2_01_farm_goes_online.py - พาโรงเรือนของเราขึ้นอินเทอร์เน็ตครั้งแรก
#
# ภารกิจ   : ต่อ WiFi ให้บอร์ดโรงเรือน แล้วขึ้น "ป้ายประจำฟาร์ม" บอกเลข IP เวลาที่ใช้ต่อ
#            จากนั้นเฝ้าดูหนึ่งนาทีว่าฟาร์มออนไลน์อยู่กี่ % ของเวลา
# ลองเล่น  : จับเวลาด้วยมือถือตอนจอนิ่ง เทียบกับเลข ms บนจอ แล้วระหว่างเฝ้าดู
#            ปิด Hotspot สัก 10 วินาทีแล้วเปิดใหม่ ดูว่าจอเห็นอะไร
# แนวคิด AIoT: ก่อนจะส่งข้อมูลฟาร์มออกไปได้ บอร์ดต้องมี "ที่อยู่" (IP) บนเครือข่ายก่อน
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.1 ขึ้นไป) และ BENTO Emulator
# ต้องแก้ก่อนรัน: WIFI_SSID กับ WIFI_PASS (ไฟล์นี้ยังไม่ใช้ broker)
# กับดัก    : wifi.connect() บล็อกได้นานถึงราว 85 วินาที จอจะนิ่ง อย่ากดรีเซ็ต
#            ป้าย "กำลังต่อ" กับ ui.poll() จึงต้องมาก่อนบรรทัดนั้น ไม่ใช่หลัง
# ของเล่นบน Dev Kit: จอ LED 16x8 วิ่งคำว่า WIFI ONLINE OFFLINE ลำโพงดังเฉพาะตอนสถานะเปลี่ยน

import rgbmatrix
import sensors
import time
import ui
import wifi

# ----- แก้สองบรรทัดนี้ให้ตรงกับ Hotspot มือถือของกลุ่ม -----
WIFI_SSID = "bento-teamXX"            # WiFi คณะที่ต้อง login ผ่านเว็บ บอร์ดใช้ไม่ได้
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว

FARM_NAME = "โรงเรือนกลุ่ม XX"          # ชื่อฟาร์มบนจอ แก้เป็นของกลุ่มคุณ
RUN_MS = 60000                        # เฝ้าดูลิงก์นานเท่าไรหลังต่อติด
TICK_MS = 1000

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


def read_th():
    try:
        return sensors.sht40.temperature(), sensors.sht40.humidity()
    except Exception:
        return None, None


ui.screen()
time.sleep_ms(200)
ui.Label("ฟาร์มต่ออินเทอร์เน็ต", x=20, y=12, color=COL_TEXT, value=24)
ui.Label(FARM_NAME, x=420, y=18, color=COL_INFO, value=18)

ui.Panel(x=20, y=56, w=760, h=120, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("สถานะ", x=36, y=64, color=COL_DIM, value=16)
state = ui.Label("กำลังจะเริ่มต่อ", x=36, y=96, color=COL_WARN, value=20)
ui.Label("เวลาที่ใช้ต่อ (ms)", x=380, y=64, color=COL_DIM, value=16)
seg = ui.Seg7(text="----", x=380, y=96, w=144, h=64, color=COL_WARN)
ui.Label("IP ของฟาร์ม", x=560, y=64, color=COL_DIM, value=16)
ip_lbl = ui.Label("-", x=560, y=100, color=COL_TEXT, value=20)

ui.Panel(x=20, y=192, w=360, h=150, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("ออนไลน์กี่ % ของเวลา", x=36, y=200, color=COL_DIM, value=16)
arc = ui.Arc(x=60, y=228, w=104, h=104, min=0, max=100, value=0)
arc.color(COL_OK)
pct_lbl = ui.Label("--", x=196, y=250, color=COL_TEXT, value=28)
drop_lbl = ui.Label("หลุด 0 ครั้ง", x=196, y=296, color=COL_DIM, value=16)

ui.Panel(x=400, y=192, w=380, h=150, color=COL_CARD, min=COL_DIM, max=12, value=1)
ui.Label("อากาศในโรงเรือนตอนนี้", x=416, y=200, color=COL_DIM, value=16)
lbl_t = ui.Label("อุณหภูมิ -- C", x=416, y=236, color=COL_TEXT, value=24)
lbl_h = ui.Label("ความชื้น -- %", x=416, y=284, color=COL_TEXT, value=24)
note = ui.Label("ครั้งแรกอาจรอนาน อย่ากดรีเซ็ต", x=20, y=352, color=COL_DIM, value=16)
ui.poll()   # ป้ายทั้งหมดต้องขึ้นจอก่อนเข้าบรรทัดที่บล็อก

# ตัววิ่งบนจอ LED สั่งครั้งเดียว แล้วเฟิร์มแวร์วิ่งให้เอง ลองดูว่ามันยังวิ่งไหมตอนจอหลักนิ่ง
rgbmatrix.scroll("WIFI", rgbmatrix.YELLOW, 80)
print("กำลังต่อ", WIFI_SSID, "- บรรทัดถัดไปจะบล็อก")
t0 = time.ticks_ms()
ok = wifi.connect(WIFI_SSID, WIFI_PASS)
took = time.ticks_diff(time.ticks_ms(), t0)
seg.text(str(took))                   # Seg7 รับข้อความ ไม่ใช่ตัวเลข

if not ok:
    # "ไม่ได้ยินวงเลย" กับ "ได้ยินแต่รหัสผิด" แก้คนละทาง scan() ช่วยแยกให้
    heard = False
    try:
        for net in wifi.scan():
            if net[0] == WIFI_SSID:
                heard = True
    except OSError:
        pass
    seg.color(COL_BAD)
    state.color(COL_BAD)
    state.text("ต่อไม่สำเร็จ")
    note.color(COL_BAD)
    note.text("ได้ยินวงแต่ต่อไม่ผ่าน ตรวจรหัสผ่าน" if heard
              else "ไม่ได้ยินวง " + WIFI_SSID + " ตรวจชื่อวง")
    ui.poll()
    rgbmatrix.scroll("")               # หยุดตัววิ่ง แล้วค้างเป็นสีแดงไว้ให้เห็นว่าไม่ผ่าน
    rgbmatrix.fill(rgbmatrix.RED)
    ui.sfx(ui.SFX_UI_DENY)
    print("ต่อไม่สำเร็จใน", took, "ms | ได้ยินวงนี้:", heard)
    raise SystemExit

# ip() คืน "0.0.0.0" ตอนยังไม่มีที่อยู่ ซึ่งเป็นสตริงที่ if ถือว่าจริง ต้องเทียบตรง ๆ
ip = wifi.ip()
for _ in range(15):
    if ip != "0.0.0.0":
        break
    ui.poll()
    time.sleep_ms(200)
    ip = wifi.ip()

seg.color(COL_OK)
state.color(COL_OK if ip != "0.0.0.0" else COL_WARN)
state.text("ออนไลน์แล้ว" if ip != "0.0.0.0" else "ลิงก์ขึ้นแต่ยังไม่มี IP")
ip_lbl.text(ip)
note.text("จด IP กับเวลาลงใบงาน แล้วลองปิด Hotspot ดู")
ui.poll()
rgbmatrix.scroll("ONLINE", rgbmatrix.GREEN, 80)
ui.sfx(ui.SFX_UI_START)
print("ต่อสำเร็จใน", took, "ms | ip =", ip)

# ----- เฝ้าดูลิงก์ของฟาร์ม -----
# connect() ตอบว่า "ตอนนั้นต่อสำเร็จ" ส่วน is_connected() ตอบว่า "ตอนนี้ยังต่ออยู่ไหม"
up = total = drops = 0
was_online = True
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
    online = wifi.is_connected()
    total += 1
    if online:
        up += 1
    if online != was_online:           # จอ LED กับเสียง: ทำเฉพาะตอนสถานะเปลี่ยน ไม่ใช่ทุกวินาที
        if not online:
            drops += 1
            print("หลุดตอนวินาทีที่", time.ticks_diff(time.ticks_ms(), t0) // 1000)
        rgbmatrix.scroll("ONLINE" if online else "OFFLINE",
                         rgbmatrix.GREEN if online else rgbmatrix.RED, 80)
        ui.sfx(ui.SFX_UI_SELECT if online else ui.SFX_UI_DENY)
    was_online = online

    pct = up * 100 // total
    arc.value(pct)
    arc.color(COL_OK if pct >= 90 else (COL_WARN if pct >= 60 else COL_BAD))
    pct_lbl.text(str(pct) + " %")
    drop_lbl.text("หลุด " + str(drops) + " ครั้ง")
    state.color(COL_OK if online else COL_BAD)
    state.text("ออนไลน์อยู่" if online else "หลุด! ฟาร์มขาดการติดต่อ")

    t, h = read_th()
    if t is not None:
        lbl_t.text("อุณหภูมิ %.1f C" % t)
        lbl_h.text("ความชื้น %.1f %%" % h)
    ui.poll()
    time.sleep_ms(TICK_MS)

rgbmatrix.scroll("")                   # หยุดตัววิ่งก่อนจบ ไม่งั้นมันวิ่งค้างต่อ
rgbmatrix.clear()
print("ออนไลน์", up * 100 // max(1, total), "% จาก", total, "วินาที | หลุด", drops, "ครั้ง")

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ใส่รหัสผ่านผิดไปหนึ่งตัว แล้วจับเวลาว่ากว่าจอจะบอกว่าผิดใช้กี่ ms เทียบกับตอนรหัสถูก
# 2) ถ้าฟาร์มจริงอยู่ไกลบ้าน 2 กม. กลุ่มคุณจะเอาเน็ตจากไหนมาให้บอร์ด ลองเขียนคำตอบลงใบงาน
