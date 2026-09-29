# cp3_04_command_confirm.py - หลักการ 3.4: ทุกคำสั่งต้องมีคำยืนยัน และมีทางถอยที่ปลอดภัย
#
# หลักการ  : ส่งคำสั่งออกไปแล้วไม่ได้แปลว่าของจริงทำตาม (ข้อความหายได้ PLC ปฏิเสธได้ PLC ดับอยู่ก็ได้)
#            วงปิดที่ถูก: ส่ง -> รอคำยืนยันจากตัวที่ทำจริง ภายใน TIMEOUT_MS -> เงียบ = ส่งซ้ำ RETRIES ครั้ง
#            -> ยังเงียบ = เลิกเดา ถอยไปสภาพปลอดภัย (สั่งปิด และถือว่า "ไม่รู้" จนกว่า PLC จะบอกเอง)
#            ความจริงอยู่ที่ PLC ไม่ใช่ที่คนสั่ง: สถานะปั๊มบนจอ (การ์ดขวา) มาจาก plc/state เท่านั้น ไม่ใช่จากปุ่มที่เรากด
# ลองเล่น  : เปิด field_sim.py (TEAM เดียวกัน) บนโน้ตบุ๊ก แล้วกด SW5 = ขอเปิดปั๊ม PUMP_SEC วินาที
#            SW6 = ขอปิด ดูขั้นบนจอติดไฟทีละขั้น · ปิด field_sim.py แล้วกดอีกที ดูทางถอย
#            รอจนน้ำในถังต่ำกว่า 10 % (หรือแก้ TANK_START ใน field_sim.py) แล้วขอเปิด: PLC ตอบ blocked_tank
# ของบนบอร์ด: SW5 (ปุ่มล่าง) = buttons.pressed(0) = ขอเปิดปั๊ม - SW6 (ปุ่มบน) = buttons.pressed(1) = ขอปิด
#            ลำโพงดังตอนกด ตอนได้คำยืนยัน ตอนถูกปฏิเสธ และตอนยอมแพ้
# สัญญา    : (ตาม s2/app/field_sim.py และ MQTT_CONTRACT_th.md ข้อ 3.6 และ 4.2)
#            ส่ง bento-aiot/<TEAM>/plc/cmd  {"pump": 1, "sec": 10} หรือ {"pump": 0}
#            ฟัง bento-aiot/<TEAM>/plc/state {"pump": 0/1, "left_s", "why", "n"} PLC ตอบทันทีทุกครั้งที่ได้คำสั่ง
#            why = on / off / blocked_tank / bad_cmd คือคำตอบของคำสั่ง - tick คือรายงานตามรอบทุก 5 วิ
#            (start / timeout / stop ก็มาได้ ไม่นับเป็นคำตอบ) plc/state ไม่มีเลขคำสั่ง จึงจับคู่ด้วย why + pump
#            ไม่ได้ยิน plc/state เกิน 15 วิ = ถือว่า PLC หลุด (สัญญาข้อ 3.6)
# ปลอดภัย  : ทางถอยสั่งปิดได้ก็ต่อเมื่อสายยังดี ถ้าเน็ตล่มทั้งเส้น คำสั่งปิดก็ไปไม่ถึง ชั้นสุดท้ายคือ PLC เอง
#            (field_sim เปิดได้ครั้งละไม่เกิน 30 วิ และไม่เปิดถ้าถังต่ำกว่า 10 %) จึงต้องมีกันหลายชั้นเสมอ
# ในฟาร์ม  : สั่งปั๊มจากโทรศัพท์แล้วเน็ตหลุดกลางทาง ถ้าแอปขึ้นว่า "เปิดแล้ว" ทั้งที่ไม่รู้ = น้ำท่วมแปลงหรือปั๊มแห้งพัง
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (MQTT ใน Emulator เป็นแบบจำลองในเบราว์เซอร์ field_sim ตอบไม่ได้
#            จึงเห็นทางถอยทุกครั้ง เว้นแต่ผู้สอนจำลองคำตอบให้)

import buttons
import json
import mqtt
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว - อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                       # เลขกลุ่ม ต้องตรงกับ TEAM ใน field_sim.py
PUMP_SEC = 10        # SW5 ขอเปิดปั๊มกี่วินาที (PLC ตัดเองที่ 30)
TIMEOUT_MS = 3000    # รอคำยืนยันนานเท่านี้ต่อครั้ง
RETRIES = 1          # เงียบแล้วส่งซ้ำกี่ครั้ง ก่อนยอมแพ้
SAMPLE_MS = 20       # อ่านปุ่มและกล่องรับทุกกี่ ms (ปุ่มกรองสั่นทุกครั้งที่อ่าน กล่องรับมีช่องเดียว)
RUN_MS = 300000      # เล่นนาน 5 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง = pressed(0), SW6 = ปุ่มบน = pressed(1)

BROKER = "broker.hivemq.com"          # ต้องตรงกับ BROKER ใน field_sim.py
CLIENT_ID = "bento-cmd-" + TEAM       # ต้องไม่ซ้ำกับใครบน broker
T_CMD = "bento-aiot/" + TEAM + "/plc/cmd"
T_STATE = "bento-aiot/" + TEAM + "/plc/state"
STEPS = ("1 ส่ง plc/cmd", "2 รอ plc/state", "3 เงียบ: ส่งซ้ำ", "4 ยอมแพ้: สั่งปิด")

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
def judge(want, st):
    # plc/state หนึ่งใบ เทียบกับคำสั่งที่รออยู่ (want = 1 เปิด / 0 ปิด / None ไม่มีคำสั่งค้าง)
    # คืน None = ใบนี้ไม่ใช่คำตอบ (tick/start/timeout) รอต่อ, "ok" = ยืนยัน, อื่น ๆ = เหตุที่ PLC ไม่ทำ
    why = st.get("why")
    if want is None or why not in ("on", "off", "blocked_tank", "bad_cmd"):
        return None                               # ไม่มีคำสั่งค้าง หรือเป็นรายงานตามรอบ
    return "ok" if why in ("on", "off") and st.get("pump") == want else why


# ---- 4) เครือข่าย ----
def connect_farm(w):
    # บันไดสามขั้น WiFi -> IP -> broker + subscribe คืน "" ถ้าผ่าน ไม่งั้นบอกว่าพังตรงไหน
    if not TEAM[4:].isdigit():                    # TEAM ยังเป็น teamXX = ไม่ต่อ ไม่ส่งอะไรออกไปเลย
        return "แก้ TEAM ก่อน"
    show(w, "res", "ต่อ WiFi...", COL_WARN)      # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก (show เรียก ui.poll ให้)
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    try:
        linked = mqtt.connect(BROKER, port=1883, client_id=CLIENT_ID, keepalive=60)
    except OSError:
        linked = False
    if not linked or not mqtt.subscribe(T_STATE):  # ฟังหัวข้อเดียว: กล่องรับมีช่องเดียว
        return "broker ไม่ตอบ"
    return ""


def send_cmd(on):
    # ส่งคำสั่งหนึ่งใบ (สายหลุด: publish โยน OSError = ส่งไม่ออก ไม่มีคำตอบ ปล่อยให้หมดเวลาไปทางถอยเอง)
    try:
        mqtt.publish(T_CMD, json.dumps({"pump": 1, "sec": PUMP_SEC} if on else {"pump": 0}))
    except OSError:
        pass


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    # การ์ด = กล่องพื้นเข้มขอบเทา + หัวเรื่องสีฟ้า
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("สั่ง -> ยืนยัน -> ปลอดภัย", x=12, y=6, color=COL_TEXT, value=24)
    card(12, 64, 440, 274, "ขั้น")
    w = {"steps": [ui.Label(STEPS[i], x=28, y=98 + i * 44, color=COL_DIM) for i in range(4)]}
    w["res"] = ui.Label(" ", x=28, y=282, color=COL_TEXT)            # ไม่ใส่ value = ตัวอักษร 20
    card(462, 64, 318, 274, "PLC บอกว่า")
    w["pump"] = ui.Label("--", x=478, y=104, color=COL_DIM, value=24)   # สถานะปั๊มตาม PLC เท่านั้น
    w["note"] = ui.Label("SW5 ขอเปิด %d วิ  SW6 ขอปิด" % PUMP_SEC, x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, key, text, col):
    # เปลี่ยนป้ายหนึ่งป้าย (สี + ข้อความ) แล้วส่งขึ้นจอ เรียกเฉพาะตอนมีเหตุการณ์
    w[key].color(col)
    w[key].text(text)
    ui.poll()


def show_steps(w, cols, text, col):
    # สีของขั้น 1-4 (เทา = ยังไม่ถึง ส้ม = กำลังทำ เขียว = ผ่าน แดง = พัง) + บรรทัดผล
    for i in range(4):
        w["steps"][i].color(cols[i])
    show(w, "res", text, col)


# ---- 6) โปรแกรมหลัก ----
def main():
    w = build_screen()
    problem = connect_farm(w)
    if problem:
        show(w, "res", problem, COL_BAD)
        beep("bad")
        raise SystemExit
    show(w, "res", "พร้อม", COL_OK)
    beep("start")
    want = None                                           # คำสั่งที่รอคำยืนยัน: 1 เปิด / 0 ปิด / None ไม่มี
    tries = 0
    down = [False, False]
    D, W, G, B = COL_DIM, COL_WARN, COL_OK, COL_BAD       # สีของขั้น: เทา ส้ม เขียว แดง
    t0 = t_sent = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        for i in (0, 1):                                  # i = 0 คือ SW5 (เปิด), i = 1 คือ SW6 (ปิด)
            d = buttons.pressed(i)
            if d and not down[i] and want is None:        # ขอบกด และไม่มีคำสั่งค้างรอคำยืนยันอยู่
                want, tries, t_sent = 1 - i, 1, now
                send_cmd(want)                            # ขั้น 1: ส่ง -> ขั้น 2: รอ
                show_steps(w, (G, W, D, D), "...", COL_WARN)
                beep("tap")
            down[i] = d
        msg = mqtt.get_message()                          # None = ยังไม่มีอะไรมา / (topic, bytes)
        try:
            st = json.loads(msg[1].decode())
            why = st.get("why")                           # ไม่ใช่ dict = โยน AttributeError
        except Exception:                                 # ไม่มีข้อความ / อ่านไม่ได้ (broker สาธารณะ ใครส่งขยะมาก็ได้)
            st = None
        if st:                                            # ทุกใบจาก PLC = ความจริงล่าสุด (สถานะปั๊มบนจอตามนี้เท่านั้น)
            on = st.get("pump") == 1
            show(w, "pump", "%s  why=%s" % ("เปิด อีก %s วิ" % st.get("left_s") if on else "ปิด", why),
                 COL_OK if on else COL_TEXT)
            verdict = judge(want, st)
            if verdict == "ok":                           # ขั้น 2 ผ่าน: PLC ยืนยันเอง
                show_steps(w, (G, G, G if tries > 1 else D, D), "ยืนยันแล้ว", COL_OK)
                beep("good")
                want = None
            elif verdict:                                 # PLC ตอบว่าไม่ทำ (เช่น blocked_tank) = รู้ผลแล้ว ไม่ต้องส่งซ้ำ
                show_steps(w, (G, B, D, D), "PLC ไม่ทำ: " + verdict, COL_BAD)
                beep("bad")
                want = None
        if want is not None and time.ticks_diff(now, t_sent) >= TIMEOUT_MS:
            if tries <= RETRIES:                          # ขั้น 3: เงียบ = ส่งซ้ำ (ไม่ใช่ "คงสำเร็จแล้วมั้ง")
                tries, t_sent = tries + 1, now
                send_cmd(want)
                show_steps(w, (G, B, W, D), "ส่งซ้ำ", COL_WARN)
                beep("tap")
            else:                                         # ขั้น 4: ยอมแพ้ ถอยไปปลอดภัย
                send_cmd(0)                               # สั่งปิด (ถ้าสายยังดี) แล้วเลิกเดา
                show(w, "pump", "ไม่รู้ = ถือว่าปิด", COL_BAD)
                show_steps(w, (G, B, B, B), "ยอมแพ้: สั่งปิด", COL_BAD)
                beep("empty")
                want = None
        time.sleep_ms(SAMPLE_MS)
    show(w, "note", "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่", COL_WARN)
    beep("good")


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: ปิด field_sim.py แล้วกด SW5 นานเท่าไรจอจึงขึ้น "ไม่มีคำยืนยัน" (ดู TIMEOUT_MS กับ RETRIES)
# 2) ตั้ง RETRIES = 0 แล้วลองอีกที ข้อดีข้อเสียของการส่งซ้ำคืออะไร ถ้าคำสั่งแรกไปถึงแล้วแต่คำตอบหาย จะเกิดอะไร
# 3) ตั้ง PUMP_SEC = 999 แล้วกด SW5 ปั๊มเปิดนานกี่วินาที ใครเป็นคนตัด (ดู left_s ที่ PLC ตอบ)
