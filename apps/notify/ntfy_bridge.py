# ntfy_bridge.py - ส่งแจ้งเตือนฟาร์มเข้ามือถือ แม้ปิดหน้าเว็บแล้ว: ฟัง MQTT ของกลุ่ม -> ส่งต่อให้ ntfy (และ Telegram ถ้าตั้งไว้)
#
# ทำอะไร   : subscribe bento-aiot/<กลุ่ม>/event (และ telemetry ถ้าใส่ --telemetry) ตามสัญญา s2/app/MQTT_CONTRACT_th.md
#            แปลงเป็นข้อความภาษาไทย แล้วส่งเข้าหัวข้อ ntfy ของกลุ่ม มือถือที่ติดตั้งแอป ntfy และ subscribe หัวข้อนั้นจะเด้งทันที
# ติดตั้ง    : pip install paho-mqtt        (ใช้ได้ทั้ง paho 1.x และ 2.x · ส่วน HTTP ใช้ของที่มากับ Python)
# รัน        : export NTFY_TOPIC=<ชื่อหัวข้อที่เดายาก>      (อย่าเขียนลงไฟล์ อย่าส่งให้คนนอกกลุ่ม)
#              python ntfy_bridge.py --team team05
#              python ntfy_bridge.py --team team05 --telemetry --soil-min 30 --tank-min 15
#              python ntfy_bridge.py --team team05 --test         ส่งข้อความทดสอบหนึ่งใบแล้วจบ
#              python ntfy_bridge.py --team team05 --dry-run      พิมพ์ว่าจะส่งอะไร ไม่ส่งจริง
# Telegram  : (ไม่บังคับ) export TELEGRAM_BOT_TOKEN=...  TELEGRAM_CHAT_ID=...  แล้วรันเหมือนเดิม ส่งทั้งสองทาง
#
# อ้างอิง API ของ ntfy (ตรวจจากเอกสารจริงเมื่อ 2026-09-28): https://docs.ntfy.sh/publish/
#   - หัวข้อ "Publish as JSON": POST ข้อความ JSON ไปที่ราก https://ntfy.sh/ (ไม่ใช่ https://ntfy.sh/<topic>)
#     ช่องที่ใช้: topic (จำเป็น), message, title, tags (array), priority (1-5), click
#     ส่งเป็น JSON ใน body ภาษาไทยจึงไม่ต้องเข้ารหัสใน header (เอกสารบอกว่าบางไลบรารีส่ง UTF-8 ใน header ไม่ได้)
#   - หัวข้อ "Message priority": 1 min · 2 low · 3 default · 4 high · 5 max/urgent
#   - หัวข้อ "Tags & emojis": tag ที่ตรงชื่อ emoji จะขึ้นเป็นรูป เช่น warning, rotating_light, bell, droplet, seedling
#   - หัวข้อ "Limitations": ข้อความละไม่เกิน 4,096 ไบต์ · ขอได้ 60 ครั้งทันที แล้วเติมให้ 1 ครั้งทุก 5 วินาที
#     · ntfy.sh ส่งได้วันละ 250 ข้อความต่อผู้ส่ง -> ไฟล์นี้จึงมีช่วงเว้น (cooldown) และเพดานต่อชั่วโมงของตัวเอง
#   - "the topic is essentially a password": ไม่มีการสมัคร ใครรู้ชื่อหัวข้อก็อ่านได้ ตั้งชื่อยาวและเดายาก
# อ้างอิง Telegram Bot API (ตรวจเมื่อ 2026-09-28): https://core.telegram.org/bots/api#sendmessage
#   https://api.telegram.org/bot<token>/sendMessage รับ JSON {chat_id, text} ตอบ {"ok": true, ...}
#   token อ่านจากตัวแปรแวดล้อมเท่านั้น ไม่พิมพ์ออกจอ ไม่เขียนลงไฟล์ (URL มี token อยู่ จึงไม่พิมพ์ URL ด้วย)
#
# ความเป็นส่วนตัว: broker สาธารณะและ ntfy.sh เป็นบริการสาธารณะ ห้ามส่งชื่อคน เบอร์โทร ตำแหน่งบ้าน หรือรหัสผ่านผ่านไฟล์นี้

import argparse
import json
import os
import queue
import re
import secrets
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

import paho.mqtt.client as mqtt

ROOT = "bento-aiot"
TEAM_RE = re.compile(r"^team(0[1-9]|[1-9][0-9])$")
TOPIC_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")        # ชื่อหัวข้อ ntfy ที่ใช้ได้ (ตามเอกสาร ntfy)
STALE_S = 15                                             # สัญญาข้อ 3.6 / 3.7
CROP_TH = {"tomato": "มะเขือเทศ", "lettuce": "ผักสลัด", "mushroom": "เห็ดนางฟ้า", "orchid": "กล้วยไม้"}
ALERT_TH = {"ok": "ปกติ", "cold": "หนาวไป", "hot": "ร้อนไป", "dry": "แห้งไป", "wet": "ชื้นไป"}
LEVEL_TH = ("สบายดี", "เริ่มเครียด", "แย่แล้ว")
WHY_TH = {"blocked_tank": "ถังต่ำกว่า 10 % ไม่ยอมเปิด", "timeout": "ครบเวลา ดับเอง", "on": "เปิดตามคำสั่ง",
          "off": "ปิดตามคำสั่ง", "stop": "หยุดฉุกเฉินที่ PLC", "bad_cmd": "คำสั่งอ่านไม่ได้"}


def now_s():
    return time.strftime("%H:%M:%S")


def num(v, lo=-1e9, hi=1e9):
    """รับเฉพาะตัวเลขจริงในช่วง (ข้อความจาก broker สาธารณะเป็นของใครก็ได้)"""
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) and lo <= v <= hi else None


# ---- 1) แปลงข้อความจากบอร์ด -> แจ้งเตือน (key, title, message, priority, tags) หรือ None ----
class Translator:
    def __init__(self, team, args):
        self.team, self.args = team, args
        self.crop_level = {}          # พืช -> ระดับล่าสุด (เตือน "กลับมาปกติ" เฉพาะตอนเคยแย่)
        self.active = {}              # กฎ telemetry ที่กำลังเข้าเงื่อนไข (เตือนตอนเพิ่งเข้าเท่านั้น)

    def event(self, d):
        e, t = d.get("event"), self.team
        if e == "sw6":
            return "sw6", "มีคนกดเรียกที่ฟาร์ม", "ปุ่ม SW6 บนบอร์ด " + t, 4, ["bell"]
        if e == "plc_lost":
            return "plc_lost", "PLC หลุด!", "Gateway " + t + " ไม่ได้ยิน PLC เกิน 15 วินาที ไม่รู้ว่าปั๊มเดินอยู่ไหม", 5, ["rotating_light"]
        if e == "auto_water":
            return "auto_water", "Gateway รดน้ำเอง", "ดินแห้ง " + str(num(d.get("soil"), 0, 100)) + " % · " + t, 2, ["droplet"]
        if e == "pump":
            why = str(d.get("why", ""))[:16]
            if why == "blocked_tank" or self.args.all_events:
                return "pump_" + why, "ปั๊ม: " + WHY_TH.get(why, why), "กลุ่ม " + t, 4 if why == "blocked_tank" else 2, ["warning"]
            return None                                   # ปั๊มเปิด/ปิดปกติ ไม่เด้ง (เยอะเกิน)
        if "crop" in d and "level" in d:
            crop, lv = str(d.get("crop"))[:16], num(d.get("level"), 0, 2)
            if lv is None:
                return None
            lv = int(lv)
            before = self.crop_level.get(crop)
            self.crop_level[crop] = lv
            if lv == 0 and not before:                    # สบายดีตั้งแต่แรก ไม่ต้องเด้ง
                return None
            why = " + ".join(ALERT_TH.get(x, x[:8]) for x in str(d.get("alert", "")).split("+"))
            msg = "%s: %s (%s) · %s °C %s %%" % (CROP_TH.get(crop, crop), LEVEL_TH[lv], why,
                                                 num(d.get("temp_c"), -50, 100), num(d.get("rh"), 0, 100))
            title = "พืชกลับมาปกติ" if lv == 0 else "พืช" + LEVEL_TH[lv]
            return "crop%d" % lv, title, msg + " · " + t, (2, 3, 5)[lv], ["seedling"] if lv == 0 else ["warning", "seedling"]
        if isinstance(e, str) and e:                      # event ของกลุ่มเอง เช่น too_hot, delivered
            return "ev_" + e[:24], "เหตุการณ์: " + e[:24], json.dumps(d, ensure_ascii=False)[:300], 3, ["farmer"]
        return None

    def telemetry(self, d):
        """กฎเกณฑ์ฝั่ง bridge: เตือนตอนเพิ่งข้ามเกณฑ์ แล้วรอจนกลับมาปกติก่อนเตือนใหม่"""
        a, out = self.args, []
        rules = (("soil_low", d.get("soil"), a.soil_min, "<", "ดินแห้ง", "ความชื้นดิน %s %%", 3, ["droplet"]),
                 ("tank_low", d.get("tank"), a.tank_min, "<", "น้ำในถังใกล้หมด", "น้ำในถัง %s %%", 4, ["warning"]),
                 ("temp_hi", d.get("temp_c"), a.temp_max, ">", "ร้อนเกิน", "อุณหภูมิ %s °C", 4, ["thermometer"]))
        for key, v, limit, op, title, fmt, prio, tags in rules:
            v = num(v)
            if v is None or limit is None:
                continue
            hit = v < limit if op == "<" else v > limit
            if hit and not self.active.get(key):
                out.append((key, title, (fmt % v) + " (เกณฑ์ " + op + " " + str(limit) + ") · " + self.team, prio, tags))
            self.active[key] = hit
        return out


# ---- 2) กันสแปม: เว้นต่อชนิด + เพดานต่อชั่วโมง ----
class RateLimit:
    def __init__(self, cooldown_s, per_hour):
        self.cooldown_s, self.per_hour = cooldown_s, per_hour
        self.last = {}                # key -> เวลาที่ส่งล่าสุด
        self.held = {}                # key -> จำนวนครั้งที่พักไว้ในช่วงเว้น
        self.sent = []                # เวลาที่ส่งในชั่วโมงที่ผ่านมา

    def allow(self, key, now=None):
        now = time.monotonic() if now is None else now
        self.sent = [t for t in self.sent if now - t < 3600]
        if now - self.last.get(key, -1e9) < self.cooldown_s or len(self.sent) >= self.per_hour:
            self.held[key] = self.held.get(key, 0) + 1
            return False, 0
        self.last[key] = now
        self.sent.append(now)
        return True, self.held.pop(key, 0)


# ---- 3) ส่งออก: ntfy (หลัก) และ Telegram (ถ้าตั้งไว้) ทำในเธรดแยก MQTT จะได้ไม่ค้างรอเน็ต ----
def http_json(url, body, headers=None, timeout=10):
    """POST JSON คืน (รหัส HTTP, ข้อความตอบกลับ) · ไม่โยน error ออกไป"""
    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    h = {"Content-Type": "application/json", "User-Agent": "bento-farm-ntfy-bridge/1.0"}
    h.update(headers or {})
    req = urllib.request.Request(url, data=data, headers=h, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read(2048).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, ""
    except (urllib.error.URLError, OSError) as e:
        return 0, type(e).__name__


class Sender:
    def __init__(self, server, topic, token, tg_token, tg_chat, click, dry_run):
        self.server = server.rstrip("/")
        self.topic, self.token, self.click = topic, token, click
        self.tg_token, self.tg_chat = tg_token, tg_chat
        self.dry_run = dry_run
        self.q = queue.Queue(maxsize=50)
        self.results = []             # (ช่องทาง, รหัส HTTP) ไว้ให้ --test ตรวจ
        threading.Thread(target=self.work, daemon=True).start()

    def push(self, title, message, priority, tags):
        try:
            self.q.put_nowait((title, message, priority, tags))
        except queue.Full:
            print(now_s(), "คิวเต็ม ทิ้งหนึ่งใบ:", title)

    def work(self):
        while True:
            title, message, priority, tags = self.q.get()
            self.send_ntfy(title, message, priority, tags)
            if self.tg_token and self.tg_chat:
                self.send_telegram(title, message)
            self.q.task_done()

    def send_ntfy(self, title, message, priority, tags):
        body = {"topic": self.topic, "title": title[:120], "message": message[:1000],
                "priority": int(priority), "tags": tags}
        if self.click:
            body["click"] = self.click
        if self.dry_run:
            shown = dict(body, topic="<NTFY_TOPIC>")
            print(now_s(), "[dry-run] ntfy <-", json.dumps(shown, ensure_ascii=False))
            return
        headers = {"Authorization": "Bearer " + self.token} if self.token else None
        code, text = http_json(self.server + "/", body, headers)
        self.results.append(("ntfy", code))
        msg_id = ""
        if code == 200:
            try:
                msg_id = " id=" + str(json.loads(text).get("id", ""))[:16]
            except ValueError:
                pass
        hint = {429: " (ntfy จำกัดความถี่ รอสักครู่)", 0: " (ต่อ ntfy ไม่ได้: " + text + ")"}.get(code, "")
        print(now_s(), "ntfy", code, "ส่ง:", title + msg_id + hint)

    def send_telegram(self, title, message):
        url = "https://api.telegram.org/bot" + self.tg_token + "/sendMessage"   # ห้ามพิมพ์ url นี้ (มี token)
        code, text = http_json(url, {"chat_id": self.tg_chat, "text": (title + "\n" + message)[:3500]})
        self.results.append(("telegram", code))
        print(now_s(), "telegram", code, "ส่ง:", title)


# ---- 4) MQTT ----
def make_client(team):
    cid = "farm-ntfy-" + team + "-" + uuid.uuid4().hex[:6]     # ไม่ชนกับบอร์ดหรือแอปอื่น (สัญญาข้อ 5.2)
    if hasattr(mqtt, "CallbackAPIVersion"):                      # paho 2.x
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
    return mqtt.Client(client_id=cid)                            # paho 1.x


def parse_args():
    p = argparse.ArgumentParser(description="ส่งแจ้งเตือนฟาร์มจาก MQTT เข้ามือถือผ่าน ntfy (และ Telegram)")
    p.add_argument("--team", default=os.environ.get("FARM_TEAM", ""), help="เลขกลุ่ม เช่น team05")
    p.add_argument("--broker", default="broker.hivemq.com", help='สำรอง: "test.mosquitto.org"')
    p.add_argument("--port", type=int, default=1883)
    p.add_argument("--telemetry", action="store_true", help="เตือนจากค่า telemetry ด้วย (ดินแห้ง ถังต่ำ ร้อนเกิน บอร์ดเงียบ)")
    p.add_argument("--soil-min", type=float, default=30)
    p.add_argument("--tank-min", type=float, default=15)
    p.add_argument("--temp-max", type=float, default=35)
    p.add_argument("--silent-s", type=int, default=60, help="บอร์ดเงียบเกินกี่วินาทีจึงเตือน (ใช้กับ --telemetry)")
    p.add_argument("--cooldown", type=int, default=120, help="เรื่องเดิมเด้งซ้ำได้เมื่อพ้นกี่วินาที")
    p.add_argument("--per-hour", type=int, default=30, help="เด้งรวมได้ไม่เกินกี่ครั้งต่อชั่วโมง")
    p.add_argument("--all-events", action="store_true", help="เด้งทุกครั้งที่ปั๊มเปิด/ปิดด้วย")
    p.add_argument("--click", default="", help="ลิงก์ที่เปิดเมื่อแตะแจ้งเตือน เช่น หน้าแดชบอร์ดของกลุ่ม")
    p.add_argument("--test", action="store_true", help="ส่งข้อความทดสอบหนึ่งใบแล้วจบ")
    p.add_argument("--dry-run", action="store_true", help="พิมพ์ว่าจะส่งอะไร ไม่ส่งจริง")
    return p.parse_args()


def main():
    a = parse_args()
    team = a.team.strip().lower()
    if not TEAM_RE.match(team):
        sys.exit("ใส่เลขกลุ่มก่อน เช่น --team team05")
    topic = os.environ.get("NTFY_TOPIC", "").strip()
    if not a.dry_run and not TOPIC_RE.match(topic):
        sys.exit("ตั้งตัวแปร NTFY_TOPIC ก่อน (อังกฤษ ตัวเลข - _ ไม่เกิน 64 ตัว) เช่น\n"
                 "  export NTFY_TOPIC=farm-" + team + "-" + secrets.token_hex(8))
    if topic and len(topic) < 16:
        print("ระวัง: ชื่อหัวข้อ ntfy สั้น เดาง่าย ใครรู้ชื่อก็อ่านแจ้งเตือนของกลุ่มได้ (ควรยาว 16 ตัวขึ้นไป)")
    tg_token, tg_chat = os.environ.get("TELEGRAM_BOT_TOKEN", ""), os.environ.get("TELEGRAM_CHAT_ID", "")
    sender = Sender(os.environ.get("NTFY_SERVER", "https://ntfy.sh"), topic, os.environ.get("NTFY_TOKEN", ""),
                    tg_token, tg_chat, a.click, a.dry_run)
    print("ntfy: หัวข้อตั้งแล้ว (ยาว %d ตัว ไม่แสดงชื่อ)" % len(topic) if topic else "ntfy: dry-run ไม่ส่งจริง",
          "| Telegram:", "เปิด" if tg_token and tg_chat else "ไม่ได้ตั้ง")

    if a.test:
        sender.push("ทดสอบแจ้งเตือนฟาร์ม", "ถ้าเห็นข้อความนี้บนมือถือ แปลว่าต่อ ntfy ได้แล้ว · " + team, 3, ["seedling"])
        sender.q.join()
        codes = [c for _, c in sender.results]
        sys.exit(0 if a.dry_run or (codes and all(c == 200 for c in codes)) else 1)

    tr, limit = Translator(team, a), RateLimit(a.cooldown, a.per_hour)
    base = ROOT + "/" + team + "/"
    seen = {"telemetry": None, "silent": False}

    def notify(item):
        key, title, message, prio, tags = item
        ok, held = limit.allow(key)
        if not ok:
            print(now_s(), "พักไว้ (ยังอยู่ในช่วงเว้น):", title)
            return
        if held:
            message += " · (ช่วงก่อนหน้าพักไว้ %d ครั้ง)" % held
        sender.push(title, message, prio, tags)

    def on_connect(client, userdata, flags, reason_code, properties=None):
        # paho 2.x ส่ง 5 อาร์กิวเมนต์ 1.x ส่ง 4 · ต่อใหม่เมื่อไรต้อง subscribe ใหม่ จึงอยู่ในนี้
        if reason_code != 0:
            print(now_s(), "broker ไม่รับ:", reason_code)
            return
        subs = [(base + "event", 0)] + ([(base + "telemetry", 0)] if a.telemetry else [])
        client.subscribe(subs)
        print(now_s(), "ต่อแล้ว ฟัง", ", ".join(s for s, _ in subs), "| Ctrl+C เพื่อหยุด")

    def on_message(client, userdata, msg):
        if len(msg.payload) > 2048:
            return
        try:
            d = json.loads(msg.payload.decode("utf-8", "replace"))
        except ValueError:
            return
        if not isinstance(d, dict):                     # ต้องเป็น JSON object เท่านั้น (สัญญาข้อ 3)
            return
        if msg.topic.endswith("/event"):
            item = tr.event(d)
            print(now_s(), "event:", json.dumps(d, ensure_ascii=False)[:120], "->", item[1] if item else "ไม่เด้ง")
            if item:
                notify(item)
        else:
            seen["telemetry"] = time.monotonic()
            if seen["silent"]:
                seen["silent"] = False
                notify(("back", "บอร์ดกลับมาแล้ว", "ได้ยิน telemetry ของ " + team + " อีกครั้ง", 2, ["white_check_mark"]))
            for item in tr.telemetry(d):
                notify(item)

    client = make_client(team)
    client.on_connect, client.on_message = on_connect, on_message
    try:
        client.connect(a.broker, a.port, keepalive=60)
    except OSError as e:
        sys.exit("ต่อ broker ไม่ได้: %s | เน็ตกันพอร์ต 1883 หรือเปล่า ลองใช้ Hotspot มือถือ" % e)
    client.loop_start()                                  # paho ต่อใหม่เองเมื่อหลุด
    try:
        while True:
            time.sleep(1)
            last = seen["telemetry"]
            if a.telemetry and last and not seen["silent"] and time.monotonic() - last > max(a.silent_s, STALE_S):
                seen["silent"] = True
                notify(("silent", "บอร์ดเงียบ", "ไม่ได้ยิน telemetry ของ " + team + " เกิน %d วินาที" % a.silent_s, 4, ["warning"]))
    except KeyboardInterrupt:
        pass
    client.loop_stop()
    client.disconnect()
    sender.q.join()
    print("หยุดแล้ว")


if __name__ == "__main__":
    main()
