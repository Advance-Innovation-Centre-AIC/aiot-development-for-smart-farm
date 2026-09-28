# field_sim.py - แปลงผักจำลองบนโน้ตบุ๊ก: โหนดเซนเซอร์ไร้สาย + PLC WiFi คุมปั๊ม
#
# ภาพฟาร์มจริง : กลางแปลงมี "โหนดเซนเซอร์ไร้สาย" วัดความชื้นดินกับระดับน้ำในถัง แล้วส่งค่าขึ้น MQTT
#               ที่โรงสูบน้ำมี "PLC WiFi" ต่อรีเลย์คุมปั๊มจริง รอฟังคำสั่งจาก MQTT
#               บอร์ด Dev Kit ของกลุ่มเป็น "Smart IoT Gateway" อยู่ตรงกลาง: อ่านค่าแปลง ตัดสินใจ สั่ง PLC
#               ไฟล์นี้เล่นเป็น "โหนดเซนเซอร์ + PLC" ให้ ห้องเรียนจะได้ไม่ต้องมีของจริง
# ทำอะไร   : ส่ง bento-aiot/<TEAM>/field/soil และ field/tank ทุก 5 วิ (สลับกันทีละ 2.5 วิ)
#            ดินแห้งลงเรื่อย ๆ เอง ถ้าปั๊มเดิน ดินชื้นขึ้นและน้ำในถังลดลง
#            ฟัง bento-aiot/<TEAM>/plc/cmd แล้วเปิด/ปิดปั๊มจำลอง ส่ง plc/state ทุกครั้งที่เปลี่ยนและทุก 5 วิ
#            PLC มีระบบป้องกันของตัวเอง ไม่เชื่อใครทั้งนั้น: เปิดได้ครั้งละไม่เกิน 30 วิ และถังต่ำกว่า 10 % ไม่เปิด
# ติดตั้ง    : pip install paho-mqtt        รัน: python field_sim.py     หยุด: Ctrl+C
# ต้องแก้    : TEAM ให้ตรงกับบอร์ด Gateway ของกลุ่ม (sf2_06_smart_gateway.py) หรือแอปที่จะทดสอบ
# สัญญา      : หัวข้อและ JSON ทั้งหมดอยู่ใน MQTT_CONTRACT_th.md ข้อ 3.5-3.7 และ 4.2

import json
import time
import uuid

import paho.mqtt.client as mqtt

# ---- 1) ตั้งค่า (แก้ได้) ----
TEAM = "team99"
BROKER, PORT = "broker.hivemq.com", 1883     # สำรอง: "test.mosquitto.org"
ROOT = "bento-aiot"
RUN_S = 600                                  # จำลองนานกี่วินาที
SEND_S = 5.0                                 # โหนดส่งค่าทุก 5 วิ (อย่าถี่กว่านี้ กล่องรับของบอร์ดมีช่องเดียว)
SOIL_START, TANK_START = 45.0, 80.0          # ค่าเริ่มต้น (%)
SOIL_DRY_PER_S = 0.4                         # ดินแห้งลงวินาทีละเท่านี้ (แดดแรงก็เพิ่มเลข)
SOIL_WET_PER_S = 2.0                         # ปั๊มเดิน: ดินชื้นขึ้นวินาทีละเท่านี้
TANK_USE_PER_S = 1.0                         # ปั๊มเดิน: น้ำในถังลดวินาทีละเท่านี้
TANK_FILL_PER_S = 0.05                       # ฝนตก/น้ำประปาเติมถังช้า ๆ
PLC_MAX_S, PLC_TANK_MIN = 30, 10             # ระบบป้องกันของ PLC: เวลาเปิดสูงสุด, ถังต่ำสุดที่ยอมเปิด

T_FIELD = ROOT + "/" + TEAM + "/field/"      # + "soil" หรือ "tank"
T_PLC_CMD = ROOT + "/" + TEAM + "/plc/cmd"
T_PLC_STATE = ROOT + "/" + TEAM + "/plc/state"

field = {"soil": SOIL_START, "tank": TANK_START}
# was_on = ปั๊มเดินอยู่ตอนตรวจครั้งก่อน (ใช้จับ "ครบเวลาดับเอง") · last_pub = เวลาที่ส่ง plc/state ล่าสุด
plc = {"until": 0.0, "n": 0, "why": "start", "was_on": False, "last_pub": -99.0}
count = {"soil": 0, "tank": 0}


# ---- 2) แปลงผัก (ฟิสิกส์แบบง่าย) ----
def pump_on():
    return time.monotonic() < plc["until"]


def step_field(dt):
    """ขยับค่าแปลงไป dt วินาที: ดินแห้งเอง ถ้าปั๊มเดินดินชื้นขึ้นและถังลดลง"""
    if pump_on():
        field["soil"] += SOIL_WET_PER_S * dt
        field["tank"] -= TANK_USE_PER_S * dt
    else:
        field["soil"] -= SOIL_DRY_PER_S * dt
    field["tank"] += TANK_FILL_PER_S * dt
    field["soil"] = max(0.0, min(100.0, field["soil"]))
    field["tank"] = max(0.0, min(100.0, field["tank"]))


def node_message(sensor):
    """ข้อความของโหนดเซนเซอร์หนึ่งตัว ตามสัญญาข้อ 3.5"""
    count[sensor] += 1
    return {"node": sensor + "-1", "value": round(field[sensor]), "unit": "%", "n": count[sensor]}


# ---- 3) PLC: ตรวจคำสั่งด้วยกฎความปลอดภัยของตัวเองก่อนแตะรีเลย์ ----
def plc_decide(cmd):
    """คืน (วินาทีที่จะเปิด หรือ 0 = ปิด หรือ None = ไม่ทำอะไร, เหตุผล) ตามสัญญาข้อ 4.2"""
    if not isinstance(cmd, dict) or "pump" not in cmd:
        return None, "bad_cmd"
    if not cmd["pump"]:
        return 0, "off"
    if field["tank"] < PLC_TANK_MIN:
        return None, "blocked_tank"
    sec = cmd.get("sec", 10)
    if not isinstance(sec, int) or sec <= 0:
        sec = 10
    return min(sec, PLC_MAX_S), "on"


def plc_state():
    """สถานะ PLC ตามสัญญาข้อ 3.6"""
    left = max(0, int(plc["until"] - time.monotonic() + 0.999)) if pump_on() else 0
    plc["n"] += 1
    return {"pump": 1 if left else 0, "left_s": left, "why": plc["why"], "n": plc["n"]}


# ---- 4) เครือข่าย ----
def make_client():
    cid = "field-sim-" + TEAM + "-" + uuid.uuid4().hex[:6]    # ไม่ชนกับบอร์ดหรือแอป
    if hasattr(mqtt, "CallbackAPIVersion"):                    # paho 2.x
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
    return mqtt.Client(client_id=cid)                          # paho 1.x


def publish_state(client):
    body = plc_state()
    client.publish(T_PLC_STATE, json.dumps(body))
    plc["last_pub"] = elapsed()
    plc["was_on"] = pump_on()          # สถานะที่เพิ่งบอกไป ไม่ใช่ "ครบเวลา"
    print("%6.1f วิ  PLC -> %s" % (elapsed(), json.dumps(body)))


def on_message(client, userdata, msg):
    text = msg.payload.decode("utf-8", "replace")
    try:
        cmd = json.loads(text)
    except ValueError:
        cmd = None
    sec, why = plc_decide(cmd)
    print("%6.1f วิ  PLC ได้คำสั่ง %s -> %s" % (elapsed(), text[:60], why))
    plc["why"] = why
    if sec is not None:
        plc["until"] = time.monotonic() + sec if sec else 0.0
    publish_state(client)                                      # ตอบสถานะทันทีทุกครั้งที่มีคำสั่ง


# ---- 5) โปรแกรมหลัก ----
T0 = time.monotonic()


def elapsed():
    return time.monotonic() - T0


def main():
    client = make_client()
    client.on_message = on_message
    client.connect(BROKER, PORT, keepalive=60)
    client.subscribe(T_PLC_CMD)
    client.loop_start()
    print("แปลงจำลอง", TEAM, "| โหนดส่ง", T_FIELD + "soil/tank", "| PLC ฟัง", T_PLC_CMD)
    publish_state(client)
    last = time.monotonic()
    # สามสายส่งเหลื่อมกัน (0, 1.25, 2.5 วิ ในทุกรอบ 5 วิ) เพราะกล่องรับของบอร์ดมีช่องเดียว ส่งพร้อมกันจะหายไปหนึ่ง
    next_soil, next_state, next_tank = 0.0, SEND_S / 4, SEND_S / 2
    try:
        while elapsed() < RUN_S:
            now = time.monotonic()
            step_field(now - last)
            last = now
            if pump_on() and field["tank"] < PLC_TANK_MIN:     # ถังแห้งระหว่างเดิน: PLC ดับเองกันปั๊มพัง
                plc["until"], plc["why"] = 0.0, "blocked_tank"
                publish_state(client)
            if plc["was_on"] and not pump_on():                # ครบเวลา: PLC ดับปั๊มเองแล้วบอก
                plc["why"] = "timeout"
                publish_state(client)
            for sensor, due in (("soil", next_soil), ("tank", next_tank)):
                if elapsed() >= due:
                    body = node_message(sensor)
                    client.publish(T_FIELD + sensor, json.dumps(body))
                    print("%6.1f วิ  โหนด %s -> %s" % (elapsed(), sensor, json.dumps(body)))
            if elapsed() >= next_soil:
                next_soil += SEND_S
            if elapsed() >= next_tank:
                next_tank += SEND_S
            if elapsed() >= next_state:                        # ชีพจรทุก 5 วิ ให้ Gateway รู้ว่า PLC ยังอยู่
                if elapsed() - plc["last_pub"] >= 1.0:         # เพิ่งตอบคำสั่งไป ไม่ต้องส่งซ้ำติด ๆ กัน
                    plc["why"] = "tick"
                    publish_state(client)
                next_state += SEND_S
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    plc["until"] = 0.0
    client.loop_stop()
    client.disconnect()
    print("จบการจำลอง")


if __name__ == "__main__":
    main()
