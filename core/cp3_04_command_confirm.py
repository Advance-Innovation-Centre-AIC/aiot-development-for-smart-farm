# cp3_04_command_confirm.py - หลักการ 3.4: ทุกคำสั่งต้องมีคำยืนยัน และมีทางถอยที่ปลอดภัย
#
# หลักการ  : ส่งคำสั่งแล้วไม่ได้แปลว่าของจริงทำตาม (ข้อความหาย PLC ปฏิเสธ หรือ PLC ดับอยู่)
#            ส่ง -> รอคำยืนยันภายใน TIMEOUT_MS -> เงียบ = ส่งซ้ำ RETRIES ครั้ง -> ยังเงียบ = สั่งปิด ถือว่า "ไม่รู้"
#            สถานะปั๊มบนจอมาจาก plc/state เท่านั้น ไม่ใช่จากปุ่มที่เรากด
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS และ TEAM (ตรงกับเลขกลุ่มที่เปิด s2/app/farm_web.html)
# ลองเล่น  : เปิด PLC Simulator ใน s2/app/farm_web.html (กด "ต่อ" แล้ว "เริ่ม PLC Simulator") กด SW5 = ขอเปิดปั๊ม SW6 = ขอปิด ดูขั้นบนจอ
#            กด "หยุด Simulator" แล้วกดอีกที ดูทางถอย · เลื่อนถังจำลองต่ำกว่า 10 % แล้วขอเปิด: PLC ตอบ blocked_tank
# สัญญา    : ส่ง bento-aiot/<TEAM>/plc/cmd  {"pump": 1, "sec": 10} หรือ {"pump": 0}
#            ฟัง bento-aiot/<TEAM>/plc/state {"pump": 0/1, "left_s", "why", "n"}
#            PLC ตอบทุกคำสั่งด้วย why = on / off / blocked_tank / bad_cmd (tick / start / timeout / stop ไม่นับ)
#            plc/state ไม่มีเลขคำสั่ง จึงจับคู่ด้วย why + pump
# ปลอดภัย  : ทางถอยสั่งปิดได้เมื่อสายยังดีเท่านั้น ชั้นสุดท้ายคือ PLC (เปิดครั้งละไม่เกิน 30 วิ ไม่เปิดถ้าถังต่ำกว่า 10 %)
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator (Emulator ต่อ broker สาธารณะจริง จึงคุยกับ PLC Simulator ได้ · ถ้าไม่ได้เปิด PLC Simulator จะเห็นทางถอยทุกครั้ง)

import buttons
import json
import mqtt
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว - อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                       # เลขกลุ่ม ต้องตรงกับเลขกลุ่มที่เปิด farm_web.html
PUMP_SEC = 10        # SW5 ขอเปิดปั๊มกี่วินาที (PLC ตัดเองที่ 30)
TIMEOUT_MS = 3000    # รอคำยืนยันนานเท่านี้ต่อครั้ง
RETRIES = 1          # เงียบแล้วส่งซ้ำกี่ครั้ง ก่อนยอมแพ้
SAMPLE_MS = 20       # อ่านปุ่มและกล่องรับทุกกี่ ms
RUN_MS = 300000      # เล่นนาน 5 นาทีแล้วจบเอง
SPEAKER = 40         # ความดังลำโพงรวม 0-100% (firmware 2.4.2 ขึ้นไป)
VOLUME = 25          # ความดังเสียง 0-127
BTN_NAMES = ("SW5", "SW6")   # SW5 = ปุ่มล่าง = pressed(1), SW6 = ปุ่มบน = pressed(0)

BROKER = "broker.hivemq.com"          # ต้องเป็น broker เดียวกับที่ farm_web.html ต่อ (HiveMQ)
CLIENT_ID = "bento-cmd-" + TEAM        # + เลขจากนาฬิกาบอร์ดทุกครั้งที่ต่อ: ไม่ชน id เก่า
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
def judge(want, st):
    # คืน None = ใบนี้ไม่ใช่คำตอบ รอต่อ, "ok" = PLC ยืนยัน, อื่น ๆ = เหตุที่ PLC ไม่ทำ
    why = st.get("why")
    if want is None or why not in ("on", "off", "blocked_tank", "bad_cmd"):
        return None
    return "ok" if why in ("on", "off") and st.get("pump") == want else why


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะบางเครื่องไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง ใช้ client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show(w, "res", "ลองต่อ broker ใหม่ %d/3" % n, COL_WARN)
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_farm(w):
    # บันไดสามขั้น WiFi -> IP -> broker + subscribe คืน "" ถ้าผ่าน ไม่งั้นบอกว่าพังตรงไหน
    if not TEAM[4:].isdigit():                    # TEAM ยังเป็น teamXX = ไม่ต่อ ไม่ส่งอะไรออกไปเลย
        return "แก้ TEAM ก่อน"
    show(w, "res", "ต่อ WiFi...", COL_WARN)      # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก (show เรียก ui.poll ให้)
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "ต่อ WiFi ไม่ได้"
    linked = connect_broker(w)         # ลองได้ 3 ครั้ง (ดู connect_broker)
    if not linked or not mqtt.subscribe(T_STATE):  # ฟังหัวข้อเดียว: กล่องรับมีช่องเดียว
        return "broker ไม่ตอบ"
    return ""


def send_cmd(on):
    # สายหลุด: publish โยน OSError = ส่งไม่ออก ปล่อยให้หมดเวลาไปทางถอยเอง
    try:
        mqtt.publish(T_CMD, json.dumps({"pump": 1, "sec": PUMP_SEC} if on else {"pump": 0}))
    except OSError:
        pass


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("สั่ง -> ยืนยัน -> ปลอดภัย", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label("MQTT", x=380, y=12, color=COL_DIM, value=16)
    w = {"led": ui.Led(x=430, y=12, w=20, h=20, color=COL_OK, value=0)}
    w["link"] = ui.Label("ออฟไลน์", x=458, y=12, color=COL_DIM, value=16)
    card(12, 64, 440, 274, "ขั้น")
    w["steps"] = [ui.Label(STEPS[i], x=28, y=98 + i * 44, color=COL_DIM) for i in range(4)]
    w["res"] = ui.Label(" ", x=28, y=282, color=COL_TEXT)
    card(462, 64, 318, 274, "PLC บอกว่า")
    w["pump"] = ui.Label("--", x=478, y=104, color=COL_DIM, value=24)
    w["note"] = ui.Label("SW5 ขอเปิด %d วิ  SW6 ขอปิด" % PUMP_SEC, x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show(w, key, text, col):
    w[key].color(col)
    w[key].text(text)
    ui.poll()


def show_link(w, online):
    w["led"].value(1 if online else 0)
    show(w, "link", "เชื่อมต่อแล้ว" if online else "ออฟไลน์", COL_OK if online else COL_DIM)


def show_steps(w, cols, text, col):
    # สีขั้น 1-4: เทา = ยังไม่ถึง ส้ม = กำลังทำ เขียว = ผ่าน แดง = พัง
    for i in range(4):
        w["steps"][i].color(cols[i])
    show(w, "res", text, col)


# ---- 6) โปรแกรมหลัก ----
def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    w = build_screen()
    problem = connect_farm(w)
    if problem:
        show(w, "res", problem, COL_BAD)
        beep("bad")
        return   # จบแบบปกติ (SystemExit ทำให้บอร์ดเริ่มระบบใหม่ และอาจค้างจนต้องถอดสาย)
    show_link(w, True)
    show(w, "res", "พร้อม", COL_OK)
    beep("start")
    want = None                                # คำสั่งที่รอคำยืนยัน: 1 เปิด / 0 ปิด / None ไม่มี
    tries = 0
    down = [False, False]
    D, W, G, B = COL_DIM, COL_WARN, COL_OK, COL_BAD
    online = True
    t0 = t_sent = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
            if online and not mqtt.is_connected():   # สายหลุด: ไฟ MQTT หรี่ แต่ยังวนต่อ
                online = False
                show_link(w, False)
            now = time.ticks_ms()
            for i in (0, 1):                   # i = 0 คือ SW5 (เปิด), i = 1 คือ SW6 (ปิด)
                d = buttons.pressed(1 - i)     # SW5 = pressed(1), SW6 = pressed(0)
                if d and not down[i] and want is None:   # เพิ่งกด และไม่มีคำสั่งค้างอยู่
                    want, tries, t_sent = 1 - i, 1, now
                    send_cmd(want)             # ขั้น 1: ส่ง -> ขั้น 2: รอ
                    show_steps(w, (G, W, D, D), "...", COL_WARN)
                    beep("tap")
                down[i] = d
            msg = mqtt.get_message()           # None = ยังไม่มีอะไรมา / (topic, bytes)
            try:
                st = json.loads(msg[1].decode())
                why = st.get("why")            # ไม่ใช่ dict = โยน AttributeError
            except Exception:                  # ไม่มีข้อความ / อ่านไม่ได้ (broker สาธารณะ ใครส่งขยะมาก็ได้)
                st = None
            if st:                             # ทุกใบจาก PLC = ความจริงล่าสุด
                on = st.get("pump") == 1
                show(w, "pump", "%s  why=%s" % ("เปิด อีก %s วิ" % st.get("left_s") if on else "ปิด", why),
                     COL_OK if on else COL_TEXT)
                verdict = judge(want, st)
                if verdict == "ok":            # ขั้น 2 ผ่าน: PLC ยืนยันเอง
                    show_steps(w, (G, G, G if tries > 1 else D, D), "ยืนยันแล้ว", COL_OK)
                    beep("good")
                    want = None
                elif verdict:                  # PLC ตอบว่าไม่ทำ = รู้ผลแล้ว ไม่ต้องส่งซ้ำ
                    show_steps(w, (G, B, D, D), "PLC ไม่ทำ: " + verdict, COL_BAD)
                    beep("bad")
                    want = None
            if want is not None and time.ticks_diff(now, t_sent) >= TIMEOUT_MS:
                if tries <= RETRIES:           # ขั้น 3: เงียบ = ส่งซ้ำ (ไม่ใช่ "คงสำเร็จแล้วมั้ง")
                    tries, t_sent = tries + 1, now
                    send_cmd(want)
                    show_steps(w, (G, B, W, D), "ส่งซ้ำ", COL_WARN)
                    beep("tap")
                else:                          # ขั้น 4: ยอมแพ้ ถอยไปปลอดภัย
                    send_cmd(0)                # สั่งปิด (ถ้าสายยังดี) แล้วเลิกเดา
                    show(w, "pump", "ไม่รู้ = ถือว่าปิด", COL_BAD)
                    show_steps(w, (G, B, B, B), "ยอมแพ้: สั่งปิด", COL_BAD)
                    beep("empty")
                    want = None
            time.sleep_ms(SAMPLE_MS)
    finally:
        mqtt.disconnect()                      # หยุดกลางทางก็ตัดสาย broker ให้เรียบร้อย
        show_link(w, False)
    show(w, "note", "จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่", COL_WARN)
    beep("good")


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: กด "หยุด Simulator" ใน farm_web.html แล้วกด SW5 นานเท่าไรจอจึงขึ้น "ยอมแพ้: สั่งปิด" (ดู TIMEOUT_MS กับ RETRIES)
# 2) ตั้ง RETRIES = 0 แล้วลองอีกที การส่งซ้ำมีข้อดีข้อเสียอะไร ถ้าคำสั่งแรกถึงแล้วแต่คำตอบหาย จะเกิดอะไร
# 3) ตั้ง PUMP_SEC = 999 แล้วกด SW5 ปั๊มเปิดนานกี่วินาที ใครเป็นคนตัด (ดู left_s ที่ PLC ตอบ)
