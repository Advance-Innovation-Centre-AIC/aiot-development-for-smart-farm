# farm_monitor.py - แอปเฝ้าฟาร์มบนโน้ตบุ๊กของกลุ่ม: ฟังค่าจากบอร์ด จดลง CSV แล้วสั่งปั๊มกลับ
#
# ติดตั้งครั้งเดียว : pip install paho-mqtt          (Python 3.8 ขึ้นไป ใช้ได้ทั้ง paho 1.x และ 2.x)
# รัน              : python farm_monitor.py          กด Ctrl+C เพื่อหยุด
# ต้องแก้ก่อนรัน   : TEAM ให้ตรงกับบอร์ดของกลุ่ม (team01 ถึง team20) ที่ TODO 1
# ฟังจากบอร์ด      : bento-aiot/<TEAM>/telemetry  (sf2_02 ส่งค่าฟาร์ม, sf2_03 ส่งสถานะปั๊ม)
#                    bento-aiot/<TEAM>/event      (sf2_02 กด SW6, sf2_04 แจ้งเตือนพืช)
# สั่งกลับไปที่     : bento-aiot/<TEAM>/cmd        (sf2_03 รู้จัก pump beep say, sf2_04 รู้จัก ack set)
# ได้อะไร          : ตารางสดบนจอ + ไฟล์ farm_log_<TEAM>.csv ที่เปิดใน Excel ได้ทันที
# ระวัง            : broker สาธารณะ ไม่มีรหัสผ่าน ใครก็อ่านและสั่งหัวข้อเราได้ ห้ามส่งของลับ
#                    บอร์ดส่งแบบ retain ไม่ได้ แอปที่เปิดทีหลังจะเห็นแค่ใบถัดไป ไม่เห็นใบที่ผ่านไปแล้ว

import csv
import json
import os
import time
import uuid

import paho.mqtt.client as mqtt

# ===== TODO 1: ตั้งค่าของกลุ่ม =====
TEAM = "teamXX"                                  # ต้องตรงกับ TEAM บนบอร์ด
BROKER, PORT = "broker.hivemq.com", 1883         # สำรอง: "test.mosquitto.org" ถ้าผู้สอนประกาศ
ROOT = "bento-aiot"
T_TEL = ROOT + "/" + TEAM + "/telemetry"         # ค่าฟาร์มทุก 5 วินาที
T_EVT = ROOT + "/" + TEAM + "/event"             # เหตุการณ์ เช่น กดปุ่ม แจ้งเตือน
T_CMD = ROOT + "/" + TEAM + "/cmd"               # คำสั่งที่เราส่งกลับไปหาบอร์ด
LOG_FILE = "farm_log_" + TEAM + ".csv"

# ===== TODO 2: เลือกคอลัมน์ที่อยากเห็นในตารางและใน Excel (ชื่อต้องตรงกับคีย์ที่บอร์ดส่ง) =====
COLUMNS = ["n", "temp_c", "rh", "hpa", "az", "soil", "light", "tank", "pump"]

# ===== TODO 3: กฎของกลุ่ม ดินแห้งกว่าเกณฑ์ = สั่งรดน้ำ =====
SOIL_MIN = 30          # ดิน (VR1 บนบอร์ด) ต่ำกว่านี้ถือว่าแห้ง
PUMP_SEC = 10          # สั่งรดน้ำครั้งละกี่วินาที (บอร์ดตัดที่ 30 เองอยู่แล้ว)
COOLDOWN_S = 60        # สั่งแล้วรออย่างน้อยกี่วินาทีก่อนสั่งซ้ำ ไม่งั้นแอปสั่งรัวทุก 5 วินาที

state = {"last_cmd": 0.0, "rows": 0, "log": None, "writer": None}


def rule(data):
    """รับค่าหนึ่งใบจากบอร์ด คืนคำสั่งที่จะส่งกลับ (dict) หรือ None ถ้าไม่ต้องทำอะไร"""
    soil = data.get("soil")
    if isinstance(soil, (int, float)) and soil < SOIL_MIN and data.get("pump") != 1:
        return {"cmd": "pump", "on": 1, "sec": PUMP_SEC}
    # TODO 4: เพิ่มกฎของกลุ่มเอง เช่น ร้อนเกิน 32 C ให้ส่ง {"cmd": "say", "text": "HOT"}
    return None


def open_log(path):
    """เปิด CSV แบบต่อท้าย ไฟล์ใหม่ใส่หัวตารางและ BOM ให้ Excel อ่านภาษาไทยถูก"""
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    f = open(path, "a", newline="", encoding="utf-8-sig" if new else "utf-8")
    w = csv.writer(f)
    if new:
        w.writerow(["time", "kind"] + COLUMNS + ["note"])
    return f, w


def print_row(data):
    if state["rows"] % 15 == 0:                  # พิมพ์หัวตารางซ้ำทุก 15 แถว จะได้ไม่ต้องเลื่อนขึ้นไปดู
        print("\n" + "time".ljust(9) + "".join(c[:7].rjust(8) for c in COLUMNS))
    cells = ["-" if data.get(c) is None else str(data.get(c))[:7] for c in COLUMNS]
    print(time.strftime("%H:%M:%S").ljust(9) + "".join(x.rjust(8) for x in cells))
    state["rows"] += 1


def on_connect(client, userdata, flags, reason_code, properties=None):
    # paho 2.x ส่ง 5 อาร์กิวเมนต์ paho 1.x ส่ง 4 ตัวสุดท้ายจึงมีค่าตั้งต้น ใช้ได้ทั้งสองรุ่น
    if reason_code != 0:
        print("broker ไม่รับ:", reason_code)
        return
    client.subscribe([(T_TEL, 0), (T_EVT, 0)])   # ต่อใหม่เมื่อไรต้อง subscribe ใหม่ จึงอยู่ในนี้
    print("ต่อแล้ว ฟัง", T_TEL, "และ", T_EVT, "| กด Ctrl+C เพื่อหยุด")


def on_message(client, userdata, msg):
    text = msg.payload.decode("utf-8", "replace")
    try:
        data = json.loads(text)
    except ValueError:
        data = None
    if not isinstance(data, dict):               # คนอื่นส่งอะไรมาก็ได้ ไม่ใช่ JSON object ก็แค่จดไว้
        data = {}
    now = time.strftime("%Y-%m-%d %H:%M:%S")
    if msg.topic == T_EVT:
        print(">>> เหตุการณ์:", text[:120])
        state["writer"].writerow([now, "event"] + [""] * len(COLUMNS) + [text[:200]])
    else:
        print_row(data)
        state["writer"].writerow([now, "telemetry"] + [data.get(c, "") for c in COLUMNS] + [""])
        cmd = rule(data)
        if cmd and time.time() - state["last_cmd"] >= COOLDOWN_S:
            state["last_cmd"] = time.time()
            client.publish(T_CMD, json.dumps(cmd))
            print("<<< สั่งกลับ:", json.dumps(cmd))
            state["writer"].writerow([now, "command"] + [""] * len(COLUMNS) + [json.dumps(cmd)])
    state["log"].flush()                         # เขียนลงดิสก์ทันที ปิดแอปแบบไหนข้อมูลก็ไม่หาย


def make_client():
    # client_id ต้องไม่ซ้ำกับบอร์ด (bento-farm-teamXX) ไม่งั้น broker เตะบอร์ดหลุด จึงสุ่มท้ายให้
    cid = "farm-app-" + TEAM + "-" + uuid.uuid4().hex[:6]
    if hasattr(mqtt, "CallbackAPIVersion"):      # paho 2.x บังคับให้บอกรุ่นของ callback
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
    return mqtt.Client(client_id=cid)            # paho 1.x ไม่รู้จักอาร์กิวเมนต์นั้น


def main():
    if len(TEAM) != 6 or not TEAM.startswith("team") or not TEAM[4:].isdigit():
        raise SystemExit("แก้ TEAM เป็นเลขกลุ่มก่อน เช่น team05")
    state["log"], state["writer"] = open_log(LOG_FILE)
    client = make_client()
    client.on_connect = on_connect
    client.on_message = on_message
    print("กำลังต่อ", BROKER, "พอร์ต", PORT, "...")
    try:
        client.connect(BROKER, PORT, keepalive=60)
        client.loop_forever()                    # วนรับข้อความจนกด Ctrl+C
    except KeyboardInterrupt:
        pass
    except OSError as e:
        print("ต่อ broker ไม่ได้:", e, "| เน็ตกันพอร์ต 1883 หรือเปล่า ลองใช้ Hotspot มือถือ")
    finally:
        client.disconnect()
        state["log"].close()
        print("\nหยุดแล้ว บันทึกไว้ที่", os.path.abspath(LOG_FILE), "เปิดใน Excel ได้เลย")


if __name__ == "__main__":
    main()
