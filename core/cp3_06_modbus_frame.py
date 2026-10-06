# cp3_06_modbus_frame.py - หลักการ 3.6: กรอบ Modbus TCP ของจริง 12 ไบต์ สร้างบนบอร์ดด้วย struct
#
# หลักการ  : PLC ในตู้ควบคุมฟาร์มจำนวนมากพูด Modbus ค่าทุกอย่างอยู่ใน "holding register" (HR) ช่องละ 16 บิต
#            อยากรู้ค่า = ขออ่าน (FC03) อยากสั่ง = ขอเขียน (FC06) คำขอหนึ่งก้อนเรียกว่า ADU มีสองท่อน:
#            MBAP 7 ไบต์ (เลขรายการ tid, protocol = 0, len = จำนวนไบต์ที่ตามมา, unit = เลขเครื่อง)
#            + PDU 5 ไบต์ (fc, เลขช่อง addr, จำนวนช่อง qty หรือค่าที่จะเขียน value) ทุกช่องเป็น big-endian
#            ตัวอย่างที่ต้องได้เป๊ะ: fc06_write(1, 1, 0, 1) = 00 01 00 00 00 06 01 06 00 00 00 01
# ความจริง : เฟิร์มแวร์ของบอร์ดนี้ไม่มีโมดูล Modbus และไม่มี socket ให้ Python (ไม่มี socket/network/ssl)
#            บอร์ดจึงเปิด TCP ไปหา PLC เองไม่ได้ สิ่งที่บอร์ดทำได้จริงคือ "สร้างกรอบ" ด้วย struct โชว์ทีละไบต์
#            แล้วส่ง "สิ่งที่อยากทำ" พร้อมกรอบนั้น (hex) ขึ้น MQTT ให้ gateway บนโน้ตบุ๊ก
#            core/app/modbus_bridge.py พูด Modbus TCP กับ PLC จำลอง core/app/modbus_plc_sim.py (พอร์ต 5020) แทน
#            gateway แบบนี้คือวิธีที่ฟาร์มจริงใช้พา PLC รุ่นเก่าเข้าระบบ IoT
# ลองเล่น  : ยังไม่ตั้ง WiFi = "โหมดดูกรอบ" กด SW5 / SW6 ดูไบต์เปลี่ยน (ไม่ได้ส่งไปไหน)
#            ตั้ง WiFi + TEAM แล้วเปิด modbus_plc_sim.py กับ modbus_bridge.py (TEAM เดียวกัน) บนโน้ตบุ๊ก
#            กด SW5 อ่าน HR0-HR2 แล้วกด SW6 สลับปั๊ม ดูไบต์ขาไปบนจอบอร์ด และไบต์ขากลับที่ gateway พิมพ์
#            ลองปิด modbus_plc_sim.py แล้วกดอีกที หรือเปิดด้วย --tank 5 แล้วสั่งเปิดปั๊ม (PLC ปฏิเสธ รหัส 4)
# ของบนบอร์ด: SW5 (ปุ่มล่าง) = buttons.pressed(0) = FC03 อ่าน 3 ช่อง (HR0 ปั๊ม, HR1 ดิน %, HR2 ถัง %)
#            SW6 (ปุ่มบน) = buttons.pressed(1) = FC06 สลับปั๊ม HR0 - ลำโพงดังตอนกด และตอนได้/ไม่ได้คำตอบ
# บนจอ     : 12 ไบต์ของคำขอ สีฟ้า = MBAP สีเขียว = PDU ชื่อช่องอยู่ใต้ไบต์ - การ์ดล่าง = ผลและกรอบคำตอบ (hex)
#            ข้อความบนจอสั้นโดยตั้งใจ: บอร์ดคอมไพล์ไฟล์นี้บนตัวเอง ตัวอักษรไทยกินที่ 3 ไบต์ต่อตัว
# สัญญา    : (ตายตัวตาม core/app/modbus_bridge.py) ส่ง bento-aiot/<TEAM>/modbus/req
#            {"n", "fc", "addr", "qty" หรือ "value", "hex"} แล้วฟัง bento-aiot/<TEAM>/modbus/resp
#            {"n", "ok": 1, "hex", "v": [...]} / {"n", "ok": 0, "hex", "exc": 1-4} / {"n", "ok": 0, "err": ...}
#            จับคู่ด้วย n (n = tid ของกรอบ) ไม่มาใน RESP_MS = "ไม่มีคำตอบ" และไม่เดาว่าสำเร็จ
# ปลอดภัย  : สถานะปั๊มบนจอมาจากคำยืนยันของ PLC เท่านั้น ไม่มีคำตอบหรือ gateway แจ้ง err = ถือว่า "ไม่รู้" (--)
#            และถ้ายังไม่รู้สถานะ SW6 จะขอ "ปิด" ก่อนเสมอ (ไม่รู้ = ปิดไว้ก่อน) PLC ยังมีระบบป้องกันของตัวเองอีกชั้น
# ในฟาร์ม  : ปั๊มที่คุมด้วยอินเวอร์เตอร์ (VFD) มิเตอร์ไฟ หัววัดดิน/EC/pH แบบ RS-485 และ PLC ส่วนใหญ่พูด Modbus
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (Emulator ต่อ broker สาธารณะจริงผ่าน WebSocket: ถ้าไม่มี
#            gateway / modbus_bridge.py ของ TEAM เดียวกันทำงานอยู่ จะเห็น "ไม่มีคำตอบ")

import buttons
import json
import mqtt
import struct
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ไม่แก้ = โหมดดูกรอบ (สร้างกรอบโชว์บนจอ ไม่ส่งไปไหน)
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว - อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                       # เลขกลุ่ม ต้องตรงกับ TEAM ใน modbus_bridge.py
UNIT_ID = 1          # เลขเครื่องของ PLC (unit id) ใน MBAP
RESP_MS = 3000       # รอคำตอบจาก gateway นานเท่านี้ (gateway เองรอ PLC 2 วิ)
SAMPLE_MS = 20       # อ่านปุ่มและกล่องรับทุกกี่ ms (ปุ่มกรองสั่นทุกครั้งที่อ่าน กล่องรับมีช่องเดียว)
RUN_MS = 300000      # เล่นนาน 5 นาทีแล้วจบเอง
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

BROKER = "broker.hivemq.com"          # ต้องตรงกับ BROKER ใน modbus_bridge.py
CLIENT_ID = "bento-mb-" + TEAM + "-%04x" % (time.ticks_ms() & 0xFFFF)   # ตัวท้ายสุ่มทุกครั้งที่รัน: รันใหม่ทันทีก็ไม่ชน id เก่า
T_REQ = "bento-aiot/" + TEAM + "/modbus/req"
T_RESP = "bento-aiot/" + TEAM + "/modbus/resp"

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
# struct.pack(">HHHBBHH", ...) = big-endian (">") ช่อง H = 2 ไบต์ ช่อง B = 1 ไบต์ รวม 2+2+2+1+1+2+2 = 12
# len = 6 เพราะนับเฉพาะไบต์ที่ตามหลังช่อง len: unit 1 + PDU 5 (fc 1 + addr 2 + qty/value 2)
def fc03_read(tid, unit, addr, qty):
    # FC03 อ่าน holding register qty ช่อง เริ่มที่ช่อง addr
    return struct.pack(">HHHBBHH", tid & 0xFFFF, 0, 6, unit, 3, addr, qty)


def fc06_write(tid, unit, addr, value):
    # FC06 เขียนค่า value ลง holding register ช่องเดียว (ช่อง addr)
    return struct.pack(">HHHBBHH", tid & 0xFFFF, 0, 6, unit, 6, addr, value)


# ---- 4) เครือข่าย ----
def connect_farm(w):
    # บันไดสามขั้น WiFi -> IP -> broker + subscribe คืน "" ถ้าผ่าน ไม่งั้นบอกว่าพังตรงไหน
    # TEAM ยังเป็น teamXX = ไม่ต่อ ไม่ส่งอะไรออกไปเลย
    if not TEAM[4:].isdigit():
        return "แก้ TEAM ก่อน"
    show(w, "stat", "ต่อ WiFi...", COL_WARN)    # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก (show เรียก ui.poll ให้)
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    try:
        linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        linked = False
    if not linked or not mqtt.subscribe(T_RESP):   # subscribe ต้องมาหลัง connect เสมอ
        return "broker ไม่ตอบ"
    return ""


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("Modbus TCP 12 ไบต์", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("MQTT", x=380, y=12, color=COL_DIM, value=16)
    ui.Label("บอร์ดไม่มี Modbus/socket: MQTT -> gateway -> PLC", x=12, y=38, color=COL_WARN, value=16)
    card(12, 62, 768, 112, "ฟ้า = MBAP  เขียว = PDU")
    w = {"b": [], "led": ui.Led(x=430, y=12, w=20, h=20, color=COL_OK, value=0),
         "link": ui.Label("ออฟไลน์", x=458, y=12, color=COL_DIM, value=16)}
    for i in range(12):                                   # ไบต์ที่ i อยู่ที่ x = 28 + i * 62
        w["b"].append(ui.Label("--", x=28 + i * 62, y=96, color=COL_INFO if i < 7 else COL_OK, value=28))
    # ชื่อช่องใต้ไบต์แรกของแต่ละช่อง: tid 2 ไบต์ proto 2 len 2 unit 1 | fc 1 addr 2 qty/value 2
    for i, name in zip((0, 2, 4, 6, 7, 8, 10), "tid proto len unit fc addr qty/value".split()):
        ui.Label(name, x=28 + i * 62, y=136, color=COL_DIM, value=14)
    card(12, 186, 768, 152, "HR0 ปั๊ม  HR1 ดิน  HR2 ถัง")
    w["stat"] = ui.Label(" ", x=28, y=224, color=COL_DIM)          # ไม่ใส่ value = ตัวอักษร 20
    w["rhex"] = ui.Label(" ", x=28, y=270, color=COL_DIM, value=16)
    w["note"] = ui.Label("SW5 อ่าน  SW6 สลับปั๊ม", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, key, text, col):
    # เปลี่ยนป้ายหนึ่งป้าย (สี + ข้อความ) แล้วส่งขึ้นจอ เรียกเฉพาะตอนมีเหตุการณ์
    w[key].color(col)
    w[key].text(text)
    ui.poll()


def show_link(w, online):
    # ไฟ MQTT: ติด = ต่อ broker อยู่ ส่งกรอบได้ · หรี่ = ออฟไลน์
    w["led"].value(1 if online else 0)
    show(w, "link", "เชื่อมต่อแล้ว" if online else "ออฟไลน์", COL_OK if online else COL_DIM)


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    problem = connect_farm(w) if WIFI_SSID[0] != "<" else "โหมดดูกรอบ"   # ยังไม่ตั้ง WiFi = โหมดดูกรอบ
    online = not problem                                  # ต่อไม่ติด = เหลือโหมดดูกรอบ (บอกบนจอ ไม่เงียบ)
    show(w, "stat", problem or T_REQ, COL_WARN if problem else COL_OK)   # ออนไลน์ = โชว์หัวข้อที่จะส่ง
    show_link(w, online)
    beep("start")
    regs = ["--"] * 3                                     # HR0-HR2 ที่ PLC ยืนยันล่าสุด ("--" = ไม่รู้)
    want = 0 if online else 1                             # ค่าที่ SW6 จะเขียนครั้งหน้า: ออนไลน์และยังไม่รู้ = ปิดก่อน
    n = wait = 0                                          # n = tid ล่าสุด, wait = n ที่รอคำตอบอยู่ (0 = ไม่รอ)
    down = [False, False]
    t0 = t_req = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            if online and not mqtt.is_connected():        # สายหลุด: บอกบนจอ คำขอที่ค้างจะหมดเวลา = ไม่รู้ผล
                online = False
                show_link(w, False)
            now = time.ticks_ms()
            for i in (0, 1):                                  # i = 0 คือ SW5, i = 1 คือ SW6
                d = buttons.pressed(i)
                if d and not down[i] and not wait:           # ขอบกด และไม่ได้รอคำตอบของคำขอก่อนหน้าอยู่
                    n += 1
                    arg = want if i else 3                    # SW6 เขียนค่า want ลง HR0 - SW5 อ่าน 3 ช่องเริ่ม HR0
                    frame = (fc06_write if i else fc03_read)(n, UNIT_ID, 0, arg)
                    for k in range(12):
                        w["b"][k].text("%02X" % frame[k])    # ฐานสิบหก 2 หลักต่อไบต์ เหมือนที่ gateway พิมพ์
                    # บรรทัดผล (สีส้ม = ยังไม่มีผล): บอกว่ากรอบนี้ขออะไร ออนไลน์แล้วจะถูกแทนด้วยผลเมื่อคำตอบมา
                    show(w, "stat", ("#%d FC03 อ่าน HR0 %d ช่อง", "#%d FC06 เขียน HR0 = %d")[i] % (n, arg), COL_WARN)
                    beep("tap")
                    if i and not online:
                        want = 1 - want                       # โหมดดูกรอบ: สลับเองให้เห็นกรอบทั้งสองแบบ (ไม่ใช่สถานะจริง)
                    if online:
                        req = {"n": n, "fc": frame[7], "addr": 0, "hex": "%02x" * 12 % tuple(frame)}   # 12 ไบต์ = 24 ตัวอักษร hex
                        req["value" if i else "qty"] = arg
                        try:
                            mqtt.publish(T_REQ, json.dumps(req))
                        except OSError:                       # สายหลุด: ส่งไม่ออก ปล่อยให้หมดเวลา = ไม่รู้ผล
                            pass
                        wait, t_req = n, now
                down[i] = d
            if wait:                                          # ถามกล่องรับทุกรอบระหว่างรอ (กล่องมีช่องเดียว)
                msg = mqtt.get_message()                      # None = ยังไม่มีอะไรมา / (topic, bytes)
                try:
                    r = json.loads(msg[1].decode())
                    r = r if r.get("n") == wait else None     # คำตอบของคำขออื่น (เก่า/ของคนอื่น) = ไม่นับ
                except Exception:                             # ไม่มีข้อความ / อ่านไม่ได้ / ไม่ใช่ dict (broker สาธารณะ ใครส่งขยะมาก็ได้)
                    r = None
                if not r and time.ticks_diff(now, t_req) >= RESP_MS:
                    r = {"err": "ไม่มีคำตอบ"}                  # เงียบเกิน RESP_MS = ไม่รู้ ไม่ใช่ "สำเร็จ"
                if r:
                    wait = 0
                    ok = r.get("ok") == 1
                    if ok:                                    # PLC ยืนยันแล้วเท่านั้นจึงเชื่อ
                        v = r.get("v", [])
                        regs = v + regs[len(v):]              # FC03 ได้ 3 ช่อง - FC06 ได้ค่าที่เขียนสำเร็จ (HR0)
                        text, want = "ยืนยันแล้ว", 1 - regs[0]
                    elif "exc" in r:                          # PLC ตอบว่า "ไม่ทำ" (4 = ระบบป้องกัน เช่น ถังต่ำ)
                        text = "PLC ปฏิเสธ %d" % r["exc"]
                    else:                                     # gateway แจ้ง err หรือหมดเวลา: ไม่รู้ว่าปั๊มเป็นอย่างไร
                        text, regs[0], want = r.get("err"), "--", 0
                    show(w, "stat", "%s:  %s  %s  %s" % (text, regs[0], regs[1], regs[2]), COL_OK if ok else COL_BAD)
                    w["rhex"].text(r.get("hex", " "))         # กรอบคำตอบที่ PLC ส่งกลับ (gateway ส่งต่อเป็น hex)
                    beep("good" if ok else "bad")
            time.sleep_ms(SAMPLE_MS)
    finally:
        mqtt.disconnect()                                 # หยุดกลางทางก็ตัดสาย broker ให้เรียบร้อย
        show_link(w, False)
    show(w, "note", "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่", COL_WARN)
    beep("good")


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนกด: กด SW5 ครั้งแรก (n = 1) ไบต์ที่ 0-1 (tid) และไบต์ที่ 11 (qty) จะเป็นเลขอะไร แล้วกดดูเฉลยบนจอ
# 2) ตั้ง UNIT_ID = 2 แล้วกด SW6 ไบต์ไหนเปลี่ยน PLC จำลองตอบไหม (ดูที่หน้าต่าง modbus_plc_sim.py)
# 3) ปิด modbus_plc_sim.py แล้วกด SW6 จอขึ้นอะไร ทำไมโปรแกรมไม่ยอมเชื่อว่าปั๊มเปิดแล้ว
