# idea_15_hydroponic.py - โรงเรือนไฮโดรโปนิกส์: อากาศร้อน/ชื้นเกิน ผักใบเหี่ยว มือถือรู้ทันที
# แม่แบบ A (idea_3_silo.py) โครงเน็ตเดิม หน้าจอใหม่
# ไฟล์นี้เป็นจุดเริ่มต้นเท่านั้น ห้ามส่งตามนี้ ทีมต้องต่อยอดเองอย่างน้อย 3 อย่าง (ดู PROJECT_BRIEF_lite_th.md)
# หน้าจอ  : แผนที่อากาศ 3x3 (แถว = อุณหภูมิ, คอลัมน์ = ความชื้น) ช่องที่ผักอยู่ติดไฟ + Arc ของ VPD
# คิดบนบอร์ด: 1) VPD (kPa) แรงดึงน้ำจากใบ คิดจาก T กับ RH 2) เวลาที่ผักเครียดสะสม
# MQTT: หัวข้อ/คีย์เหมือน sf2_04 (สัญญา 3.4, 4) + คีย์เพิ่ม 2 ตัวใน event:
#       "vpd" = kPa ทศนิยม 2 ตำแหน่ง  "stress_min" = นาทีที่หลุดช่วงสะสม
#       crop: lettuce kangkong kale  คำสั่ง: ack, set t_hi (เหมือนเดิม)
# เกณฑ์ = ค่าสาธิตในห้อง 25-30 C ไม่ใช่คำแนะนำทางเกษตร
#
# ลองเล่น  : หมุน VR3 หรือเป่าลมหายใจใส่บอร์ด จนขึ้นเตือน แล้วส่ง {"cmd":"ack"} หรือกด SW5
# ต้องแก้ก่อนรัน: WIFI_SSID, WIFI_PASS, TEAM, CROP, TEMP_OFFSET

import buttons
import json
import mqtt
import pots
import rgbmatrix
import sensors
import time
import ui
import wifi

# ---- 1) ตั้งค่า ----
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # อังกฤษ/ตัวเลข ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว
TEAM = "teamXX"
CROP = "ผักสลัด"
TEMP_OFFSET = 0.0    # บอร์ดอ่านสูงกว่าห้อง: ใส่ค่าชดเชย เช่น -7.0

BROKER = "broker.hivemq.com"
ROOT = "bento-aiot"
CLIENT_ID = "bento-farm-" + TEAM
TOPIC_EVENT = ROOT + "/" + TEAM + "/event"
TOPIC_CMD = ROOT + "/" + TEAM + "/cmd"
BTN_NAMES = ("SW6", "SW5")

# (ชื่อไทย, รหัสอังกฤษ, T ต่ำ, T สูง, RH ต่ำ, RH สูง)
# ค่าสาธิต: ห้อง 27 C = ปกติ  VR3 ครึ่งรอบ (+5) = เริ่มเครียด  เป่าลมหายใจ = ชื้นเกิน
CROPS = (
    ("ผักสลัด", "lettuce", 18, 30, 40, 85),
    ("ผักบุ้ง", "kangkong", 20, 33, 40, 85),
    ("คะน้า", "kale", 18, 30, 40, 85),
)
T_HI_MAX = 45
READ_MS, BEEP_MS, POLL_MS, RUN_MS = 1000, 5000, 20, 600000
VPD_LO, VPD_HI = 0.4, 1.6    # ช่วง VPD ที่ผักใบชอบ (ค่าสาธิต) ใช้บอกบนจอเท่านั้น
MX = (("GROW", rgbmatrix.GREEN), ("CHECK", rgbmatrix.YELLOW), ("STRESS", rgbmatrix.RED))
BOX = (212, 90, 368, 150)

SPEAKER = 40
VOLUME = 25
COL_TEXT, COL_DIM = 0xE6FFF4, 0x86B8A8
COL_CARD, COL_EDGE, COL_CELL = 0x0B2724, 0x1F5C52, 0x14403A
COL_OK, COL_WARN, COL_BAD, COL_LEAF, COL_WATER = 0x7CE38B, 0xF2C14E, 0xFF6B6B, 0xB8F27C, 0x3FC1C9
LEVEL_COLORS = (COL_OK, COL_WARN, COL_BAD)


# ---- 2) ฮาร์ดแวร์ ----
TUNES = {"tap": (76,), "good": (72, 79, 84), "bad": (84, 76), "empty": (84, 76, 69)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


def read_climate():
    try:
        t_raw = sensors.sht40.temperature()
        h = sensors.sht40.humidity()
    except Exception:
        return None, None
    return t_raw + TEMP_OFFSET, h


# ---- 3) สมอง ----
def valid_team(team):
    return len(team) == 6 and team[:4] == "team" and team[4:].isdigit() and team != "team00"


def judge(t, h, lim):
    # คืน (ระดับ 0-2, ไทย, อังกฤษ)  2 = หลุดไกล (เกิน 3 C หรือ 10 %RH)
    t_lo, t_hi, h_lo, h_hi = lim
    p = []
    if t < t_lo:
        p.append(("หนาวไป", "cold"))
    if t > t_hi:
        p.append(("โรงร้อน", "hot"))
    if h < h_lo:
        p.append(("แห้งไป", "dry"))
    if h > h_hi:
        p.append(("ชื้นเกิน", "wet"))
    if not p:
        return 0, "สบายดี", "ok"
    far = (t < t_lo - 3) or (t > t_hi + 3) or (h < h_lo - 10) or (h > h_hi + 10)
    return (2 if far else 1), " + ".join(x[0] for x in p), "+".join(x[1] for x in p)


def vpd_kpa(t, h):
    # ความดันไอน้ำอิ่มตัว (สูตร Tetens) x ส่วนที่อากาศยังขาดน้ำ
    es = 0.6108 * 2.718281828 ** (17.27 * t / (t + 237.3))
    return es * (1 - h / 100)


def cell_of(t, h, lim):
    # แถว 0 = ร้อน 1 = พอดี 2 = หนาว  คอลัมน์ 0 = แห้ง 1 = พอดี 2 = ชื้น
    row = 0 if t > lim[1] else (2 if t < lim[0] else 1)
    col = 0 if h < lim[2] else (2 if h > lim[3] else 1)
    return row * 3 + col


def mmss(sec):
    return "%02d:%02d" % (sec // 60, sec % 60)


# ---- 4) เครือข่าย ----
def connect_broker(w):
    # broker สาธารณะไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง client_id ใหม่ทุกครั้ง
    for n in (1, 2, 3):
        if n > 1:
            show_status(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WARN)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def connect_farm(w):
    # WiFi -> IP -> broker + subscribe  พังขั้นไหนบอกขั้นนั้น
    show_status(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()
    if not wifi.connect(WIFI_SSID, WIFI_PASS) or wifi.ip() == "0.0.0.0":
        return "WiFi ไม่ติด: ตรวจชื่อ/รหัส"
    linked = connect_broker(w)
    if not linked or not mqtt.subscribe(TOPIC_CMD):     # subscribe หลัง connect เสมอ
        return "broker ไม่ตอบ: รอ 1 นาทีแล้วรันใหม่"
    return ""


def send(obj):
    try:
        mqtt.publish(TOPIC_EVENT, json.dumps(obj))
        return True
    except OSError:
        return False


# ---- 5) หน้าจอ ----
def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_EDGE, max=14, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_LEAF, value=16)


def show_link(w, ok):
    w["mq"].value(1 if ok else 0)
    w["mq_t"].text("MQTT: ต่อแล้ว" if ok else "MQTT: ออฟไลน์")


def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("ไฮโดรโปนิกส์  แผนที่อากาศของผัก", x=12, y=4, color=COL_LEAF, value=24)
    w = {"box": None, "cells": [], "cell": -1}
    card(12, 40, 330, 300, "ผักอยู่ช่องไหน")
    for r in range(3):
        ui.Label(("ร้อน", "พอดี", "หนาว")[r], x=22, y=96 + r * 72, color=COL_DIM, value=14)
        for c in range(3):
            w["cells"].append(ui.Panel(x=72 + c * 88, y=72 + r * 72, w=82, h=66,
                                       color=COL_CELL, min=COL_EDGE, max=10, value=1))
    for c in range(3):
        ui.Label(("แห้ง", "พอดี", "ชื้น")[c], x=96 + c * 88, y=292, color=COL_DIM, value=14)
    ui.Label("แถว = อุณหภูมิ  คอลัมน์ = ความชื้น", x=24, y=314, color=COL_DIM, value=14)
    card(352, 40, 428, 150, "อากาศในโรงเรือน")
    w["th"] = ui.Label("-- C  -- %", x=366, y=70, color=COL_TEXT, value=24)
    w["vpd"] = ui.Label("VPD -.-- kPa", x=366, y=104, color=COL_WATER, value=16)
    w["vpd_t"] = ui.Label("-", x=366, y=128, color=COL_DIM, value=14)
    w["crop"] = ui.Label("-", x=366, y=156, color=COL_DIM, value=14)
    w["arc"] = ui.Arc(x=650, y=62, w=116, h=116, min=0, max=300, value=0)
    w["arc"].color(COL_WATER)
    card(352, 200, 428, 110, "ผักเครียดไหม")
    w["mood"] = ui.Label("...", x=366, y=226, color=COL_WARN, value=16)
    w["stress"] = ui.Label("เครียดสะสม 00:00", x=366, y=252, color=COL_TEXT, value=16)
    w["ack"] = ui.Label("-", x=366, y=278, color=COL_DIM, value=14)
    ui.Label("VR3 = แดดบนหลังคา  เป่าลม = ชื้นขึ้น  " + BTN_NAMES[1] + " = รับทราบ", x=352, y=318, color=COL_DIM, value=14)
    w["status"] = ui.Label("กำลังเริ่ม", x=12, y=356, color=COL_DIM)
    w["mq"] = ui.Led(x=600, y=10, w=16, h=16, color=COL_OK, value=0)
    w["mq_t"] = ui.Label("MQTT: ออฟไลน์", x=624, y=8, color=COL_DIM, value=16)
    ui.poll()
    return w


def show_status(w, msg, col):
    w["status"].color(col)
    w["status"].text(msg)


def show_ack(w, s, msg, col):
    w["ack"].color(col)
    w["ack"].text("แจ้งแล้ว %d ครั้ง  %s" % (s.alerts, msg))


def show_crop(w, s):
    t_lo, t_hi, h_lo, h_hi = s.lim
    w["crop"].text("%s ชอบ %d-%d C, %d-%d %%" % (s.crop[0], t_lo, t_hi, h_lo, h_hi))


def show_reading(w, s, t, h, sun, level, why_th, vpd):
    w["th"].text("%.1f C  %d %%" % (t, h))
    w["vpd"].text("VPD %.2f kPa  (แดด +%d)" % (vpd, sun))
    w["vpd_t"].text("อับชื้น เสี่ยงเน่าโคน" if vpd < VPD_LO else
                    ("อากาศดึงน้ำจากใบแรง ใบเหี่ยว" if vpd > VPD_HI else "ใบคายน้ำพอดี"))
    w["arc"].value(int(min(300, vpd * 100)))
    cell = cell_of(t, h, s.lim)
    if cell != w["cell"]:              # ทาสีใหม่เฉพาะช่องที่เปลี่ยน
        if w["cell"] >= 0:
            w["cells"][w["cell"]].color(COL_CELL)
        w["cell"] = cell
    w["cells"][cell].color(LEVEL_COLORS[level])
    w["mood"].color(LEVEL_COLORS[level])
    w["mood"].text(("ผักสบาย โตดี", "เริ่มเครียด: ", "ผักเครียดหนัก: ")[level] + ("" if level == 0 else why_th))
    w["stress"].text("เครียดสะสม " + mmss(s.stress_s))


def close_box(w):
    if w["box"] is not None:
        w["box"].delete()
        w["box"] = None


# ---- 6) โปรแกรมหลัก ----
class Watch:
    def __init__(self, idx):
        self.level, self.acked, self.alerts, self.t = -1, True, 0, None
        self.crop = CROPS[idx]
        self.lim = list(self.crop[2:])
        self.stress_s = 0              # วินาทีที่ระดับ 1 หรือ 2
        self.vpd = 0.0


class Stop(Exception):
    pass


def stop(w, msg, col=COL_BAD):
    show_link(w, False)
    rgbmatrix.scroll("")
    rgbmatrix.clear()
    close_box(w)
    show_status(w, msg, col)
    ui.poll()
    raise Stop


def on_command(w, s, raw):
    # คำสั่งจากแอป (สัญญาข้อ 4) คืน True ถ้ารับทราบ  คำสั่งที่ไม่รู้จัก = เงียบ
    try:
        cmd = json.loads(raw.decode())
        act = cmd.get("cmd")
    except Exception:
        return False
    if act == "ack":
        return True
    elif act == "set":                 # จำนวนเต็มในช่วงที่สมเหตุผลเท่านั้น
        v = cmd.get("t_hi")
        if isinstance(v, int) and s.lim[0] < v <= T_HI_MAX:
            s.lim[1], s.level, s.t = v, -1, None     # ตัดสินใหม่และแจ้งทันที
            show_crop(w, s)
            beep("tap")
    # >>> ภารกิจกลุ่ม: วาง elif "pump" จาก sf2_03 ตรงนี้ (พัดลม/พ่นหมอก) <<<
    return False


def on_level(w, s, level, why_th, why_en, t, h, sun):
    # ระดับเปลี่ยน: ส่งก่อน (สัญญาข้อ 3.4)
    s.level, s.acked = level, level < 2
    s.alerts += 1
    if not send({"id": TEAM, "crop": s.crop[1], "level": level, "alert": why_en,
                 "temp_c": round(t, 1), "rh": round(h, 1), "sun_c": sun,
                 "vpd": round(s.vpd, 2), "stress_min": s.stress_s // 60}):
        stop(w, "สายหลุดตอนส่ง")
    rgbmatrix.scroll(MX[level][0], MX[level][1], 80)
    beep(("good", "tap", "empty")[level])
    show_ack(w, s, "รอคนดูแลโรงเรือน" if level == 2 else "ส่งแล้ว", COL_BAD if level == 2 else COL_DIM)
    close_box(w)
    if level == 2:  # MsgBox รับไม่เกิน 126 ไบต์
        w["box"] = ui.MsgBox("ผักเครียดหนัก!\n" + why_th + "\n" + BTN_NAMES[1] + " / ack = รับทราบ",
                             x=BOX[0], y=BOX[1], w=BOX[2], h=BOX[3], value=0)


def read_and_judge(w, s):
    # อ่าน -> ตัดสิน -> โชว์ -> แจ้งถ้าเปลี่ยน  คืน True ถ้าเพิ่งแจ้ง
    sun = pots.read(2) * 10 // 4095    # VR3 = แดดบนหลังคา +0..10 C
    t, h = read_climate()
    if t is None or h is None:
        s.t = None                     # อ่านไม่ได้ = ลองใหม่รอบหน้าเลย
        return False
    s.t = t = t + sun
    s.vpd = vpd_kpa(t, h)
    level, why_th, why_en = judge(t, h, s.lim)
    if level > 0:
        s.stress_s += READ_MS // 1000
    show_reading(w, s, t, h, sun, level, why_th, s.vpd)
    if level == s.level:               # ส่ง/เสียง/จอไฟ RGB เฉพาะตอนเปลี่ยน
        return False
    on_level(w, s, level, why_th, why_en, t, h, sun)
    return True


def main():
    if hasattr(ui, "volume"):
        ui.volume(SPEAKER)
    w = build_screen()
    names = [c[0] for c in CROPS]
    if CROP not in names:
        stop(w, "แก้ CROP ให้ตรงกับ CROPS")
    s = Watch(names.index(CROP))
    show_crop(w, s)
    if not valid_team(TEAM):
        stop(w, "แก้ TEAM ก่อน")
    problem = connect_farm(w)
    if problem:
        stop(w, problem)
    show_link(w, True)
    show_status(w, "เฝ้าโรงเรือนอยู่", COL_OK)
    was_down = False
    t_read = t_beep = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        msg = mqtt.get_message()       # ฟังทุก 20 ms อ่านเซนเซอร์ทุกวินาที
        ack = on_command(w, s, msg[1]) if msg else False
        down = buttons.pressed(1)      # SW5 = รับทราบในโรงเรือน ไม่ต้องพึ่งเน็ต
        if down and not was_down:      # กดค้างไม่นับซ้ำ
            ack = True
        was_down = down
        if ack and not s.acked:        # รับทราบ: หยุดร้อง ปิดกล่อง จอไฟ ACK
            s.acked = True
            close_box(w)
            show_ack(w, s, "รับทราบแล้ว", COL_OK)
            rgbmatrix.scroll("ACK", rgbmatrix.CYAN, 80)
            beep("tap")
        ui.poll()
        if s.t is None or time.ticks_diff(now, t_read) >= READ_MS:
            t_read = now
            if read_and_judge(w, s):
                t_beep = now
        if s.level == 2 and not s.acked and time.ticks_diff(now, t_beep) >= BEEP_MS:
            t_beep = now
            beep("bad")
        time.sleep_ms(POLL_MS)

    stop(w, "จบรอบ เครียดสะสม " + mmss(s.stress_s), COL_DIM)


try:
    main()
except Stop:
    pass
finally:
    try:
        mqtt.disconnect()
    except Exception:
        pass

# ---- ตาคุณ ----
# 1) แจ้งเฉพาะเมื่อหลุดช่วงติดกัน 5 วินาที (นับใน Watch) - เปิดประตูโรงเรือนแป๊บเดียวไม่ควรเตือน
# 2) ระดับ 2: สั่งพัดลมหรือพ่นหมอกจากแอป: วาง elif "pump" จาก sf2_03 ใน on_command
# 3) เตือนเมื่อ VPD หลุดช่วงนานเกิน 10 นาที แม้ T กับ RH ยังผ่านเกณฑ์
