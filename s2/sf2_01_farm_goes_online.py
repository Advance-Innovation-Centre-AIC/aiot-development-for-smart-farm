# sf2_01_farm_goes_online.py - พาโรงเรือนขึ้นอินเทอร์เน็ตครั้งแรก
#
# ภารกิจ   : ต่อ WiFi โชว์ IP กับเวลาที่ใช้ต่อ แล้วเฝ้าดูลิงก์หนึ่งนาที
# ลองเล่น  : ระหว่างเฝ้าดู ปิด Hotspot สัก 10 วินาทีแล้วเปิดใหม่
# บนจอ     : เวลาต่อ (Seg7) ออนไลน์ % (Arc) ไฟ (Led) กราฟลิงก์ (Chart)
# บอร์ด     : TESAIoT Dev Kit (firmware เวอร์ชันล่าสุด) และ BENTO Emulator
# ต้องแก้ก่อนรัน: WIFI_SSID กับ WIFI_PASS
# กับดัก    : wifi.connect() บล็อกได้ราว 85 วินาที จอจะนิ่ง อย่ากดรีเซ็ต

import gpio
import math
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า (แก้ได้) ----
# ใช้ Hotspot มือถือ (WiFi ที่ต้อง login ผ่านเว็บ บอร์ดใช้ไม่ได้)
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร

FARM_NAME = "โรงเรือนกลุ่ม XX"  # ชื่อบนจอ
RUN_MS = 60000                # เฝ้าดูลิงก์กี่ ms
TICK_MS = 1000                # ถามลิงก์ทุกกี่ ms
IP_TRIES, IP_WAIT_MS = 15, 200
TEMP_OFFSET = 0.0    # ชดเชยความอุ่นจากชิป เช่น -7.0
HUM_FIX = True       # แปลงความชื้นเป็นของห้อง (สูงเกินจริงให้ตั้ง False)

SPEAKER = 40  # ลำโพงรวม 0-100% (firmware 2.4.2 ขึ้นไป)
VOLUME = 25   # ความดังเสียง 0-127
COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----

TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)

def read_climate():
    # ตัวที่อ่านไม่ได้เป็น None
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


def led_named(name):
    # หา LED ด้วยชื่อ ไม่ใช่เลข
    try:
        names = gpio.board_info()["led_names"]
        led = gpio.led(names.index(name) if name in names else 0)
        led.off()
        return led
    except Exception:
        return None


def set_led(led, on):
    if led:
        led.on() if on else led.off()


def marquee(text, color):
    # สั่งครั้งเดียว เฟิร์มแวร์วิ่งให้เอง ("" = หยุด)
    rgbmatrix.scroll(text, color, 80)


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ไม่แตะเน็ต ----
def sat_pressure(t):
    # สูตร Magnus (hPa)
    return 6.112 * math.exp(17.62 * t / (243.12 + t))


def room_humidity(h_raw, t_raw, t_room):
    return min(100.0, h_raw * sat_pressure(t_raw) / sat_pressure(t_room))


def ssid_heard(nets, ssid):
    # ในผล scan() [(ชื่อวง, rssi, ความปลอดภัย, ช่อง), ...] มีวงชื่อนี้ไหม
    for net in nets:
        if net[0] == ssid:
            return True
    return False


def signal_level(rssi):
    # rssi 0 = ไม่ได้วัด คืน None
    if not isinstance(rssi, int) or rssi >= 0:
        return None
    return max(0, min(100, rssi + 100))


def pct_color(pct):
    return COL_OK if pct >= 90 else (COL_WARN if pct >= 60 else COL_BAD)


# ---- 4) เครือข่าย ----
def connect_wifi(w):
    # ป้ายเตือนต้องขึ้นจอ "ก่อน" บรรทัดที่บล็อก คืน (สำเร็จไหม, ใช้ไปกี่ ms)
    show_note(w, "ครั้งแรกอาจรอนาน จอนิ่งได้ อย่ากดรีเซ็ต", COL_WARN)
    ui.poll()                          # ป้ายทั้งหมดต้องขึ้นจอก่อนเข้าบรรทัดที่บล็อก
    marquee("WIFI", rgbmatrix.YELLOW)
    print("กำลังต่อ", WIFI_SSID, "- บรรทัดถัดไปจะบล็อก")
    t0 = time.ticks_ms()
    ok = wifi.connect(WIFI_SSID, WIFI_PASS)
    took = time.ticks_diff(time.ticks_ms(), t0)
    w["seg"].text(str(took))           # Seg7 รับข้อความ ไม่ใช่ตัวเลข
    w["seg"].color(COL_OK if ok else COL_BAD)
    return ok, took


def heard_on_air():
    # "ไม่ได้ยินวงเลย" กับ "ได้ยินแต่รหัสผิด" แก้คนละทาง scan() ช่วยแยกให้
    try:
        return ssid_heard(wifi.scan(), WIFI_SSID)
    except OSError:
        return False


def wait_for_ip():
    # ip() คืน "0.0.0.0" ตอนยังไม่มีที่อยู่ ซึ่งเป็นสตริงที่ if ถือว่าจริง ต้องเทียบตรง ๆ
    ip = wifi.ip()
    for _ in range(IP_TRIES):
        if ip != "0.0.0.0":
            break
        ui.poll()
        time.sleep_ms(IP_WAIT_MS)
        ip = wifi.ip()
    return ip


def read_signal():
    # firmware 2.4.1 ตอบ rssi = 0 เส้นฟ้าจึงขึ้นแค่ใน Emulator
    try:
        return signal_level(wifi.status()["rssi"])
    except Exception:
        return None


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def line_chart(x, y, w, h, lo, hi, color, parent=None):
    # LVGL ไม่วาดจุดกลมเมื่อจำนวนจุด >= ความกว้าง
    ch = ui.Chart(x=x, y=y, w=min(w, 400), h=h, color=color, min=lo, max=hi, parent=parent)
    ch.prop(ui.PROP_CHART_POINTS, 400)
    return ch


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ฟาร์มต่ออินเทอร์เน็ต", x=12, y=6, color=COL_TEXT, value=24)
    ui.Label(FARM_NAME, x=320, y=12, color=COL_INFO, value=16)
    w = {}
    card(12, 44, 380, 150, "ต่อ WiFi (ms) และ IP")
    w["seg"] = ui.Seg7(text="----", x=24, y=76, w=150, h=56, color=COL_WARN)
    w["ip"] = ui.Label("-", x=190, y=92, color=COL_TEXT, value=20)
    w["state"] = ui.Label("กำลังจะเริ่มต่อ", x=24, y=148, color=COL_WARN, value=20)
    card(402, 44, 378, 150, "ออนไลน์กี่ % ของเวลา")
    w["arc"] = ui.Arc(x=414, y=72, w=116, h=116)
    w["pct"] = ui.Label("--", x=546, y=78, color=COL_TEXT, value=28)
    w["led"] = ui.Led(x=546, y=124, w=24, h=24, color=COL_OK)
    w["drops"] = ui.Label("หลุด 0 ครั้ง", x=580, y=126, color=COL_DIM, value=16)
    card(12, 202, 400, 136, "เขียว=ลิงก์ ฟ้า=สัญญาณ (ถ้าวัดได้)")
    w["chart"] = line_chart(24, 230, 376, 100, 0, 100, COL_OK)
    w["s_sig"] = w["chart"].add_series(COL_INFO)
    card(422, 202, 358, 136, "อากาศในโรงเรือน")
    w["t"] = ui.Label("อุณหภูมิ -- C", x=434, y=236, color=COL_TEXT, value=24)
    w["h"] = ui.Label("ความชื้น -- %", x=434, y=282, color=COL_TEXT, value=24)
    w["note"] = ui.Label("กำลังเริ่ม", x=12, y=352, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_note(w, text, col):
    # ไม่เรียก ui.poll() ในนี้ คนเรียกเลือกเอง
    w["note"].color(col)
    w["note"].text(text)


def show_state(w, text, col):
    w["state"].color(col)
    w["state"].text(text)


def show_link(w, online, pct, drops):
    w["arc"].value(pct)
    w["arc"].color(pct_color(pct))
    w["pct"].text(str(pct) + " %")
    w["led"].value(1 if online else 0)  # 1 = สว่าง, 0 = หรี่
    w["drops"].text("หลุด " + str(drops) + " ครั้ง")
    show_state(w, "ออนไลน์อยู่" if online else "หลุด! ฟาร์มขาดการติดต่อ", COL_OK if online else COL_BAD)


def show_air(w, t, h):
    if t is not None:
        w["t"].text("อุณหภูมิ %.1f C" % t)
    if h is not None:
        w["h"].text("ความชื้น %.1f %%" % h)


def plot(w, online, signal):
    # วินาทีละหลายจุด หนึ่งนาทีจึงเต็มกราฟพอดี
    for _ in range(max(1, 400 * TICK_MS // RUN_MS)):
        w["chart"].set_next(0, 90 if online else 10)
        if signal is not None:
            w["chart"].set_next(w["s_sig"], signal)


# ---- 6) โปรแกรมหลัก ----
class Stop(Exception):
    # จบโปรแกรมแบบปกติ (SystemExit ทำให้บอร์ดเริ่มระบบใหม่ และอาจค้างจนต้องถอดสาย)
    pass


def stop(w, led, msg, col):
    # หยุดตัววิ่งด้วย ไม่งั้นมันวิ่งค้างต่อ
    set_led(led, False)
    marquee("", rgbmatrix.WHITE)
    rgbmatrix.clear()
    show_note(w, msg, col)
    ui.poll()


def failed(w, led, took):
    # ต่อไม่สำเร็จ: บอกว่าต้องแก้ตรงไหน แล้วค้างจอไฟ RGB เป็นสีแดงให้เห็นว่าไม่ผ่าน
    heard = heard_on_air()
    show_state(w, "ต่อไม่สำเร็จ", COL_BAD)
    stop(w, led, "ได้ยินวงแต่ต่อไม่ผ่าน ตรวจรหัสผ่าน" if heard
         else "ไม่ได้ยินวง " + WIFI_SSID + " ตรวจชื่อวง", COL_BAD)
    rgbmatrix.fill(rgbmatrix.RED)
    beep("bad")
    print("ต่อไม่สำเร็จใน", took, "ms | ได้ยินวงนี้:", heard)
    raise Stop


def link_changed(online, sec):
    # จอ LED กับเสียง: ทำเฉพาะตอนสถานะเปลี่ยน ไม่ใช่ทุกวินาที
    if not online:
        print("หลุดตอนวินาทีที่", sec)
    marquee("ONLINE" if online else "OFFLINE", rgbmatrix.GREEN if online else rgbmatrix.RED)
    beep("good" if online else "bad")


def watch_link(w, led):
    # connect() ตอบว่า "ตอนนั้นต่อสำเร็จ" ส่วน is_connected() ตอบว่า "ตอนนี้ยังต่ออยู่ไหม"
    up = total = drops = 0
    was = True
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        online = wifi.is_connected()
        total += 1
        up += 1 if online else 0
        if online != was:
            drops += 0 if online else 1
            link_changed(online, time.ticks_diff(time.ticks_ms(), t0) // 1000)
        was = online
        set_led(led, online)
        show_link(w, online, up * 100 // total, drops)
        t, _, h, _ = read_climate()
        show_air(w, t, h)
        plot(w, online, read_signal())
        ui.poll()
        time.sleep_ms(TICK_MS)
    return up, total, drops


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    led = led_named("RGB_GREEN")
    ok, took = connect_wifi(w)
    if not ok:
        failed(w, led, took)
    ip = wait_for_ip()
    got_ip = ip != "0.0.0.0"
    show_state(w, "ออนไลน์แล้ว" if got_ip else "ลิงก์ขึ้นแต่ยังไม่มี IP", COL_OK if got_ip else COL_WARN)
    w["ip"].text(ip)
    show_note(w, "จด IP กับเวลาลงใบงาน แล้วลองปิด Hotspot ดู", COL_DIM)
    ui.poll()
    marquee("ONLINE", rgbmatrix.GREEN)
    beep("start")
    print("ต่อสำเร็จใน", took, "ms | ip =", ip)
    up, total, drops = watch_link(w, led)
    stop(w, led, "จบรอบ - กด Program to Device อีกครั้งเพื่อเล่นใหม่", COL_WARN)
    print("ออนไลน์", up * 100 // max(1, total), "% จาก", total, "วินาที | หลุด", drops, "ครั้ง")


try:
    main()
except Stop:
    pass

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) ใส่รหัสผิดหนึ่งตัว จอบอกว่าผิดในกี่ ms แล้วลองสะกดชื่อวงผิด ข้อความเปลี่ยนไหม
# 2) แก้ pct_color ให้ต่ำกว่า 99 % = ส้ม แล้วปิด Hotspot 1 วินาทีดู
# 3) ฟาร์มจริงอยู่ไกลบ้าน 2 กม. จะหาเน็ตจากไหนให้บอร์ด
