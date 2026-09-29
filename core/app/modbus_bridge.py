# modbus_bridge.py - Gateway แปลภาษา MQTT <-> Modbus TCP บนโน้ตบุ๊ก: พา PLC รุ่นเก่าเข้าระบบ IoT
#
# ภาพฟาร์มจริง : PLC ในตู้ควบคุมจำนวนมากพูด Modbus TCP ได้อย่างเดียว ส่วนระบบ IoT คุยกันด้วย MQTT
#               ตรงกลางจึงมี Gateway คอยแปลภาษา แบบนี้คือวิธีที่นิยมใช้พา PLC รุ่นเก่าเข้าระบบ IoT
# ทำไมต้องมี : เฟิร์มแวร์ Dev Kit ไม่มีโมดูล Modbus และไม่มี socket ให้ Python (ไม่มี socket/network/ssl)
#               บอร์ดจึงเปิด TCP ไปหา PLC เองไม่ได้ บอร์ดสร้างกรอบ Modbus TCP ของจริงด้วย struct โชว์ทีละไบต์
#               แล้วส่ง "สิ่งที่อยากทำ" พร้อมกรอบนั้น (hex) ขึ้น MQTT ไฟล์นี้รับไปพูด Modbus TCP กับ PLC แทน
# ทำอะไร   : ฟัง bento-aiot/<TEAM>/modbus/req ตรวจกรอบ ส่งไบต์เดิมทุกไบต์ไปที่ PLC รอคำตอบ
#            แล้วตอบที่ bento-aiot/<TEAM>/modbus/resp พิมพ์ทุกไบต์ที่ไปและกลับ ให้ทั้งห้องดูตามได้
# ติดตั้ง    : pip install paho-mqtt        รัน: python modbus_bridge.py (เปิด modbus_plc_sim.py ไว้ก่อน)
#            หยุด: Ctrl+C
# ต้องแก้    : TEAM ให้ตรงกับบอร์ด · ต่อ PLC ตัวจริง: แก้ PLC_HOST และ PLC_PORT (มาตรฐาน Modbus TCP คือ 502)
# สัญญา    : JSON สั้น ๆ เพราะกล่องรับของบอร์ดมีช่องเดียว ข้อความละไม่เกิน 255 ไบต์
#   ขอ .../modbus/req  {"n":7,"fc":3,"addr":0,"qty":3,"hex":"000700000006010300000003"}
#                      {"n":8,"fc":6,"addr":0,"value":1,"hex":"000800000006010600000001"}
#                      มี "hex" = ส่งกรอบนั้นทุกไบต์ตามที่บอร์ดวาด · ไม่มี = สร้างกรอบให้จาก fc/addr/qty/value
#   ตอบ .../modbus/resp {"n":7,"ok":1,"hex":"<กรอบคำตอบ>","v":[0,45,80]}   (FC06 ได้ "v":[ค่าที่เขียน])
#                      {"n":8,"ok":0,"hex":"<กรอบคำตอบ>","exc":4}         PLC ตอบข้อยกเว้น (รหัส 1-4)
#                      {"n":8,"ok":0,"err":"plc_offline"}                 ต่อ PLC ไม่ได้ หรือไม่ตอบใน 2 วิ
#                      err อื่น: bad_req (อ่าน JSON ไม่ได้/ช่องไม่ครบ, n เป็น null ถ้าอ่านไม่ได้)
#                                bad_frame (กรอบผิดรูป ไม่ส่งต่อ) · too_big (อ่านเกิน 10 ช่อง คำตอบจะล้นกล่อง)
#                                bad_reply (PLC ตอบผิดรูป)
#            "n" ของคำตอบ = "n" ของคำขอ ใช้จับคู่ · ok = 1 เฉพาะเมื่อ PLC ยืนยันแล้วเท่านั้น

import json
import queue
import socket
import struct
import time
import uuid

import paho.mqtt.client as mqtt

# ---- 1) ตั้งค่า (แก้ได้) ----
TEAM = "team99"                              # ต้องตรงกับ TEAM บนบอร์ด
BROKER, PORT = "broker.hivemq.com", 1883     # สำรอง: "test.mosquitto.org"
ROOT = "bento-aiot"
PLC_HOST, PLC_PORT = "127.0.0.1", 5020       # PLC จำลองบนเครื่องนี้ - ของจริงใส่ IP ของ PLC และพอร์ต 502
UNIT_ID = 1                                  # ใช้ตอนสร้างกรอบเองเท่านั้น
TIMEOUT_S = 2.0                              # PLC เงียบเกินนี้ถือว่าออฟไลน์
MAX_QTY = 10                                 # อ่านครั้งละไม่เกิน 10 ช่อง คำตอบจะไม่เกิน ~160 ไบต์

T_REQ = ROOT + "/" + TEAM + "/modbus/req"
T_RESP = ROOT + "/" + TEAM + "/modbus/resp"

inbox = queue.Queue()                        # คำขอจาก MQTT รอคิวให้เธรดหลักทำทีละข้อ
T0 = time.monotonic()


def log(text):
    print("%6.1f วิ  %s" % (time.monotonic() - T0, text), flush=True)


# ---- 2) กรอบ Modbus TCP (ไม่แตะเครือข่าย ทดสอบได้ลำพัง) ----
def spaced(data):
    return " ".join("%02X" % b for b in data)


def build_frame(tid, fc, addr, arg):
    # MBAP (เลขรายการ, protocol 0, ยาว 6 ไบต์, unit) + PDU (fc, addr, qty หรือ value) = 12 ไบต์
    return struct.pack(">HHHBBHH", tid & 0xFFFF, 0, 6, UNIT_ID, fc, addr, arg)


def frame_from_fields(req, n):
    # บอร์ดไม่ได้ส่งกรอบมา: สร้างจาก fc/addr/qty/value คืน None ถ้าช่องไม่ครบหรือค่าเกิน 16 บิต
    fc, addr = req.get("fc"), req.get("addr")
    arg = req.get("qty") if fc == 3 else req.get("value")
    if fc not in (3, 6) or not all(isinstance(x, int) and 0 <= x <= 0xFFFF for x in (fc, addr, arg)):
        return None
    return build_frame(n or 0, fc, addr, arg)


def check_frame(adu):
    # คืน None ถ้าเป็นกรอบคำขอ FC03/FC06 ที่ส่งต่อได้ ไม่งั้นคืนรหัส err
    if not 8 <= len(adu) <= 260:
        return "bad_frame"
    pid, length = struct.unpack(">HH", adu[2:6])
    if pid != 0 or length != len(adu) - 6 or adu[7] not in (3, 6) or len(adu) != 12:
        return "bad_frame"                             # FC03/FC06 ขอ = 12 ไบต์พอดีเสมอ
    if adu[7] == 3 and struct.unpack(">H", adu[10:12])[0] > MAX_QTY:
        return "too_big"
    return None


def read_reply(req, rep):
    # แปลกรอบคำตอบเป็นส่วนที่เหลือของ JSON คำตอบ: ok + hex + v หรือ exc หรือ err
    if len(rep) < 9 or rep[:4] != req[:2] + b"\x00\x00" or struct.unpack(">H", rep[4:6])[0] != len(rep) - 6:
        return {"ok": 0, "err": "bad_reply"}          # เลขรายการต้องตรงกับที่ส่งไป ไม่งั้นเป็นคำตอบของคนอื่น
    fc, pdu = req[7], rep[7:]
    out = {"ok": 0, "hex": rep.hex().upper()}
    if pdu[0] == fc | 0x80 and len(pdu) == 2:
        out["exc"] = pdu[1]
        return out
    arg = struct.unpack(">H", req[10:12])[0]           # FC03 = จำนวนช่อง, FC06 = ค่าที่เขียน
    if fc == 3 and pdu[0] == 3 and pdu[1] == 2 * arg == len(pdu) - 2:
        out["ok"], out["v"] = 1, list(struct.unpack(">%dH" % arg, pdu[2:]))
        return out
    if fc == 6 and pdu == req[7:]:                     # FC06 ทำสำเร็จ = PLC ทวนคำขอกลับทุกไบต์
        out["ok"], out["v"] = 1, [arg]
        return out
    return {"ok": 0, "err": "bad_reply"}


# ---- 3) คุยกับ PLC ----
def recv_exact(sock, n):
    data = b""
    while len(data) < n:                               # TCP ส่งมาเป็นท่อนได้ วนอ่านจนครบ
        part = sock.recv(n - len(data))
        if not part:
            raise ConnectionError("PLC ปิดสายกลางคัน")
        data += part
    return data


def talk_to_plc(adu):
    # ต่อใหม่ทุกคำขอ: คำขอมาห่างกันหลายวินาที ต่อสายใหม่ใช้ไม่กี่มิลลิวินาที และไม่ต้องคอยเดาว่า
    # สายเก่ายังดีอยู่ไหม (PLC หลายรุ่นตัดสายที่เงียบนานทิ้งเอง) · ต่อไม่ติด/เงียบเกินเวลา = OSError
    with socket.create_connection((PLC_HOST, PLC_PORT), timeout=TIMEOUT_S) as sock:
        sock.sendall(adu)
        head = recv_exact(sock, 7)
        length = struct.unpack(">H", head[4:6])[0]
        if not 2 <= length <= 254:
            return head                                # ผิดรูป ปล่อยให้ read_reply ตัดสินเป็น bad_reply
        return head + recv_exact(sock, length - 1)


def handle_request(text):
    # ข้อความ JSON หนึ่งก้อนจากบอร์ด -> dict คำตอบ (ยังไม่ส่ง)
    try:
        req = json.loads(text)
    except ValueError:
        req = None
    if not isinstance(req, dict):
        return {"n": None, "ok": 0, "err": "bad_req"}
    n = req.get("n") if isinstance(req.get("n"), int) else None
    if "hex" in req:
        try:
            adu, how = bytes.fromhex(str(req["hex"])), "กรอบจากบอร์ด ส่งต่อทุกไบต์"
        except ValueError:
            return {"n": n, "ok": 0, "err": "bad_frame"}
    else:
        adu, how = frame_from_fields(req, n), "บอร์ดไม่ได้ส่งกรอบมา สร้างให้จาก fc/addr"
        if adu is None:
            return {"n": n, "ok": 0, "err": "bad_req"}
    err = check_frame(adu)
    if err:
        log("กรอบใช้ไม่ได้ (%s) ไม่ส่งต่อ: %s" % (err, spaced(adu)))
        return {"n": n, "ok": 0, "err": err}
    log("-> PLC %s  (%s)" % (spaced(adu), how))
    try:
        rep = talk_to_plc(adu)
    except OSError as e:                               # ไม่แกล้งว่าสำเร็จ บอกตรง ๆ ว่า PLC ไม่อยู่
        log("PLC ไม่ตอบ (%s)" % e)
        return {"n": n, "ok": 0, "err": "plc_offline"}
    log("<- PLC %s" % spaced(rep))
    out = {"n": n}
    out.update(read_reply(adu, rep))
    return out


# ---- 4) MQTT ----
def make_client():
    cid = "modbus-bridge-" + TEAM + "-" + uuid.uuid4().hex[:6]  # ไม่ชนกับบอร์ดหรือแอป
    if hasattr(mqtt, "CallbackAPIVersion"):                    # paho 2.x
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
    return mqtt.Client(client_id=cid)                          # paho 1.x


def on_connect(client, userdata, flags, rc, properties=None):
    client.subscribe(T_REQ)                            # สมัครทุกครั้งที่ต่อติด หลุดแล้วต่อใหม่ก็ยังฟังอยู่
    log("ต่อ broker แล้ว (%s) ฟัง %s" % (rc, T_REQ))


def on_message(client, userdata, msg):
    inbox.put(msg.payload.decode("utf-8", "replace"))  # งาน TCP ช้า ทำในเธรดหลัก ไม่ถ่วงเธรดของ MQTT


# ---- 5) โปรแกรมหลัก ----
def main():
    if len(TEAM) != 6 or not TEAM.startswith("team") or not TEAM[4:].isdigit():
        print("แก้ TEAM ก่อน ให้เป็นเลขกลุ่มเดียวกับบอร์ด เช่น team05")
        return 1
    client = make_client()
    client.on_connect = on_connect
    client.on_message = on_message
    try:
        client.connect(BROKER, PORT, keepalive=60)
    except OSError as e:
        print("ต่อ broker %s ไม่ได้ (%s)" % (BROKER, e))
        return 1
    client.loop_start()
    print("Modbus bridge", TEAM, "|", T_REQ, "-> PLC %s:%d ->" % (PLC_HOST, PLC_PORT), T_RESP)
    try:
        while True:
            try:
                text = inbox.get(timeout=1.0)
            except queue.Empty:
                continue
            log("MQTT <- %s" % text[:120])
            body = json.dumps(handle_request(text), separators=(",", ":"))
            client.publish(T_RESP, body)
            log("MQTT -> %s" % body)
    except KeyboardInterrupt:
        pass
    client.loop_stop()
    client.disconnect()
    print("ปิด bridge แล้ว")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
