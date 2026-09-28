# fake_board.py - บอร์ดจำลองบนโน้ตบุ๊ก ใช้ทดสอบแอปของกลุ่มตอนไม่มีบอร์ดจริง
#
# ทำอะไร   : ต่อ broker สาธารณะแล้วส่งข้อความ "หน้าตาเดียวกับบอร์ดจริง" ทุกตัวอักษร
#            ช่วง 1 (20 วิแรก)  = แบบ sf2_02: ค่าฟาร์มทุก 5 วิ (ดินชื้น 55 %) + กด SW6 หนึ่งครั้ง
#            ช่วง 2 (ที่เหลือ)   = แบบ sf2_03: ดิน ถังน้ำ ปั๊ม ทุก 5 วิ (ดินแห้ง 20 % ให้แอปสั่งรดน้ำ)
#            วินาทีที่ 40        = แจ้งเตือนพืช 1 ใบ แบบ sf2_04 (ร้อนไป ระดับ 2)
#            และฟังหัวข้อ cmd ทำตามคำสั่งด้วยกฎเดียวกับ sf2_03 (ปั๊มไม่เกิน 30 วิ ถังต่ำกว่า 10 % ไม่เปิด)
# ติดตั้ง    : pip install paho-mqtt        รัน: python fake_board.py     หยุด: Ctrl+C
# ต้องแก้    : TEAM ให้ตรงกับแอปที่จะทดสอบ (ใช้เลขที่ไม่ชนกลุ่มอื่น เช่น team99)
# ใช้คู่กับ  : farm_monitor.py หรือ farm_web.html?team=<TEAM> หรือแอปที่กลุ่มเขียนเอง (ดู MQTT_CONTRACT_th.md)
# ระวัง      : อย่ารันพร้อมบอร์ดจริงเลขกลุ่มเดียวกัน ข้อความจะปนกันจนแยกไม่ออกว่าอะไรมาจากไหน

import json
import time
import uuid

import paho.mqtt.client as mqtt

# ---- 1) ตั้งค่า (แก้ได้) ----
TEAM = "team99"
BROKER, PORT = "broker.hivemq.com", 1883     # สำรอง: "test.mosquitto.org"
ROOT = "bento-aiot"
RUN_S = 180                                  # จำลองนานกี่วินาที
SEND_S = 5                                   # ส่งค่าทุก 5 วิ เหมือนบอร์ด
PHASE2_S = 20                                # วินาทีที่เปลี่ยนจากแบบ sf2_02 เป็นแบบ sf2_03
PUMP_DEFAULT_S, PUMP_MAX_S, TANK_MIN = 10, 30, 10    # กฎเดียวกับ sf2_03
TANK = 80                                    # น้ำในถัง % (ลองแก้เป็น 5 ดูว่าปั๊มยอมเปิดไหม)

T_TEL = ROOT + "/" + TEAM + "/telemetry"
T_EVT = ROOT + "/" + TEAM + "/event"
T_CMD = ROOT + "/" + TEAM + "/cmd"
state = {"pump_until": 0.0, "t0": time.monotonic()}


# ---- 2) ข้อความที่ส่ง (หน้าตาเดียวกับบอร์ดจริงทุกคีย์) ----
def report_sf2_02(n):
    """ค่าฟาร์มแบบ sf2_02 (เลขอากาศเป็นค่าคงที่สมมติ ไม่ได้อ่านเซนเซอร์จริง)"""
    body = {"id": TEAM, "n": n, "temp_c": 29.3, "rh": 61.2, "hpa": 1008.4, "az": 9.79,
            "soil": 55, "light": 70, "tank": TANK}
    body["sim"], body["by"] = "soil light tank", "timer"
    return body


def report_sf2_03(n):
    """สถานะปั๊มแบบ sf2_03: pump = 1 ระหว่างที่ปั๊มยังเปิดอยู่"""
    pump = 1 if time.monotonic() < state["pump_until"] else 0
    return {"id": TEAM, "n": n, "soil": 20, "tank": TANK, "pump": pump, "sim": "soil tank"}


def alert_sf2_04():
    """แจ้งเตือนพืชแบบ sf2_04: ร้อนไป ระดับ 2 (แย่แล้ว)"""
    return {"id": TEAM, "crop": "tomato", "level": 2, "alert": "hot",
            "temp_c": 33.5, "rh": 62.0, "sun_c": 5}


# ---- 3) รับคำสั่ง: ตรวจทุกคำสั่งก่อนทำ เหมือน sf2_03 ----
def handle_command(cmd):
    """คืนข้อความบอกว่าบอร์ดจริงจะทำอะไรกับคำสั่งนี้"""
    if not isinstance(cmd, dict):
        return "ไม่ใช่คำสั่งที่อ่านได้"
    act, on, sec = cmd.get("cmd", ""), cmd.get("on", 1), cmd.get("sec", PUMP_DEFAULT_S)
    if act == "led" and cmd.get("n", 0) != 0:
        return "ไม่มีปั๊มดวงที่ " + str(cmd.get("n"))
    if act in ("led", "pump") and not on:
        state["pump_until"] = 0.0
        return "ปิดปั๊ม"
    if act in ("led", "pump") and TANK < TANK_MIN:
        return "น้ำเหลือ %d%% ไม่ยอมเปิด" % TANK
    if act in ("led", "pump"):
        if not isinstance(sec, int) or sec <= 0:
            sec = PUMP_DEFAULT_S
        sec = min(sec, PUMP_MAX_S)
        state["pump_until"] = time.monotonic() + sec
        return "เปิดปั๊ม %d วิ" % sec
    if act in ("beep", "say", "ack", "set"):
        return "รับคำสั่ง " + act + " (บอร์ดจริงทำตาม ไฟล์นี้แค่จดไว้)"
    return "ไม่รู้จักคำสั่ง " + str(act)[:12]


def on_message(client, userdata, msg):
    text = msg.payload.decode("utf-8", "replace")
    try:
        cmd = json.loads(text)
    except ValueError:
        cmd = None
    print("%6.1f วิ  คำสั่งเข้า: %s  ->  %s" % (elapsed(), text[:80], handle_command(cmd)))


# ---- 4) โปรแกรมหลัก ----
def elapsed():
    return time.monotonic() - state["t0"]


def main():
    cid = "fake-board-" + TEAM + "-" + uuid.uuid4().hex[:6]   # ไม่ชนกับบอร์ดจริงหรือแอป
    if hasattr(mqtt, "CallbackAPIVersion"):                    # paho 2.x
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
    else:                                                      # paho 1.x
        client = mqtt.Client(client_id=cid)
    client.on_message = on_message
    client.connect(BROKER, PORT, keepalive=60)
    client.subscribe(T_CMD)
    client.loop_start()
    print("บอร์ดจำลอง", TEAM, "ส่งเข้า", T_TEL, "| ฟัง", T_CMD, "| Ctrl+C เพื่อหยุด")
    n2 = n3 = 0
    next_send, sw6_sent, alert_sent = 0.0, False, False
    try:
        while elapsed() < RUN_S:
            if elapsed() >= next_send:
                next_send += SEND_S
                if elapsed() < PHASE2_S:
                    n2 += 1
                    body = report_sf2_02(n2)
                else:
                    n3 += 1
                    body = report_sf2_03(n3)
                client.publish(T_TEL, json.dumps(body))
                print("%6.1f วิ  ส่ง: %s" % (elapsed(), json.dumps(body)))
            if not sw6_sent and elapsed() >= 12:
                sw6_sent = True
                client.publish(T_EVT, json.dumps({"id": TEAM, "event": "sw6", "msg": "call"}))
                print("%6.1f วิ  กด SW6 (event)" % elapsed())
            if not alert_sent and elapsed() >= 40:
                alert_sent = True
                client.publish(T_EVT, json.dumps(alert_sf2_04()))
                print("%6.1f วิ  แจ้งเตือนพืช (event)" % elapsed())
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    client.loop_stop()
    client.disconnect()
    print("จบการจำลอง")


if __name__ == "__main__":
    main()
