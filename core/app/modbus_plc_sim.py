# modbus_plc_sim.py - PLC จำลองบนโน้ตบุ๊กที่พูด Modbus TCP ของจริง (ใช้แค่ Python 3 ไม่ต้องติดตั้งอะไรเพิ่ม)
#
# ภาพฟาร์มจริง : ในตู้ควบคุมโรงสูบน้ำมักมี PLC รุ่นเก่าที่พูด Modbus TCP ได้อย่างเดียว ไม่รู้จัก MQTT
#               ค่าทุกอย่างอยู่ใน "holding register" ช่องละ 16 บิต อยากรู้ค่าก็ขออ่าน อยากสั่งก็ขอเขียน
#               ไฟล์นี้เล่นเป็น PLC ตัวนั้น พร้อมแปลงผักจำลองแบบเดียวกับ s2/app/field_sim.py
# ทำไมต้องมี : เฟิร์มแวร์ Dev Kit ไม่มีโมดูล Modbus และไม่มี socket ให้ Python (ไม่มี socket/network/ssl)
#               บอร์ดจึงเปิดการเชื่อมต่อ TCP เองไม่ได้ บอร์ด "สร้างกรอบ Modbus TCP ของจริง" ด้วย struct
#               โชว์ทีละไบต์ แล้วส่งกรอบนั้น (เป็น hex) ขึ้น MQTT ให้ modbus_bridge.py ส่งต่อมาถึง PLC ตัวนี้
# ทำอะไร   : ฟังพอร์ต 5020 (มาตรฐานคือ 502 แต่พอร์ตต่ำกว่า 1024 ต้องใช้สิทธิ์ root/admin)
#            รับ FC03 อ่าน holding register และ FC06 เขียน register เดียว พิมพ์ทุกกรอบที่เข้าและออกเป็นไบต์
# register : (เลขช่องเริ่มที่ 0) HR0 ปั๊ม 0/1 อ่าน/เขียนได้ · HR1 ดิน % · HR2 ถัง % · HR3 ปั๊มเหลือกี่วิ
#            HR4 เหตุปฏิเสธล่าสุด (0 = ไม่มี, 1 = ถังต่ำกว่าเกณฑ์, 2 = ค่าที่เขียนผิด) · HR1-HR4 อ่านได้อย่างเดียว
# ระบบป้องกัน: PLC ไม่เชื่อใครทั้งนั้น เปิดปั๊มได้ครั้งละไม่เกิน 30 วิ · ถังต่ำกว่า 10 % ไม่ยอมเปิด (ข้อยกเว้น 04)
#            ถ้ากำลังเดินอยู่ก็ดับเอง แล้วรอน้ำกลับถึง 15 % ก่อนจึงยอมเปิดอีก (กันปั๊มติด ๆ ดับ ๆ ตรงขอบ 10 %)
#            เขียน HR0 เป็นค่าอื่นนอกจาก 0/1 = ข้อยกเว้น 03 · เขียน HR1-HR4 = ข้อยกเว้น 02
# ติดตั้ง    : ไม่ต้อง          รัน: python modbus_plc_sim.py          หยุด: Ctrl+C
#            ลองระบบป้องกันถังแห้ง: python modbus_plc_sim.py --tank 12   (เริ่มด้วยน้ำในถัง 12 %)
# ต้องแก้    : ไม่ต้อง · อยากให้เครื่องอื่นในวงเดียวกันต่อเข้ามาได้ เปลี่ยน HOST เป็น "0.0.0.0"

import argparse
import socketserver
import struct
import threading
import time

# ---- 1) ตั้งค่า (แก้ได้) ----
HOST = "127.0.0.1"                           # "0.0.0.0" = ให้เครื่องอื่นในวงเดียวกันต่อเข้ามาได้
PORT = 5020                                  # Modbus TCP มาตรฐานคือ 502 (ต้องใช้สิทธิ์ root/admin)
UNIT_ID = 1                                  # เลขเครื่องของ PLC ตัวนี้
SOIL_START, TANK_START = 45.0, 80.0          # ค่าเริ่มต้น (%)
SOIL_DRY_PER_S = 0.4                         # ดินแห้งลงวินาทีละเท่านี้
SOIL_WET_PER_S = 2.0                         # ปั๊มเดิน: ดินชื้นขึ้นวินาทีละเท่านี้
TANK_USE_PER_S = 1.0                         # ปั๊มเดิน: น้ำในถังลดวินาทีละเท่านี้
TANK_FILL_PER_S = 0.05                       # ฝนตก/น้ำประปาเติมถังช้า ๆ
PUMP_MAX_S, TANK_MIN = 30, 10                # ระบบป้องกัน: เวลาเปิดสูงสุด, ถังต่ำสุดที่ยอมเปิด
TANK_RESUME = 15                             # ถังแห้งตัดไปแล้ว ต้องกลับถึงเท่านี้ก่อนจึงยอมเปิดอีก
STATUS_S = 10                                # พิมพ์สถานะแปลงทุกกี่วิ
IDLE_S = 60                                  # ไคลเอนต์เงียบนานเท่านี้ PLC ตัดสายทิ้ง

NUM_REGS = 5
HR_PUMP = 0
EXC_FUNCTION, EXC_ADDRESS, EXC_VALUE, EXC_DEVICE = 1, 2, 3, 4
EXC_TEXT = {1: "ไม่รู้จักคำสั่งนี้", 2: "ไม่มี register นี้ให้ทำแบบนั้น", 3: "ค่าไม่ถูกต้อง",
            4: "PLC ไม่ยอมทำ (ระบบป้องกัน)"}
REFUSE_NONE, REFUSE_TANK, REFUSE_VALUE = 0, 1, 2

# until = เวลาที่ปั๊มต้องดับ (0 = ดับอยู่) · dry = ถังแห้งตัดไปแล้ว รอน้ำกลับถึง TANK_RESUME
plant = {"soil": SOIL_START, "tank": TANK_START, "until": 0.0, "refuse": REFUSE_NONE, "dry": False}
lock = threading.Lock()                      # แปลงถูกแตะจากหลายเธรด (ไคลเอนต์ละเธรด + ฟิสิกส์)
log_lock = threading.Lock()                  # กันบรรทัด log ของสองเธรดพิมพ์ทับกัน
T0 = time.monotonic()


def log(text):
    with log_lock:
        print("%6.1f วิ  %s" % (time.monotonic() - T0, text), flush=True)


# ---- 2) แปลงผัก (ฟิสิกส์แบบเดียวกับ field_sim.py) ----
def pump_on():
    return time.monotonic() < plant["until"]


def step_plant(dt):
    # ขยับแปลงไป dt วินาที แล้วตรวจระบบป้องกัน คืนข้อความถ้า PLC เพิ่งดับปั๊มเอง
    with lock:
        was_on = plant["until"] > 0.0
        if pump_on():
            plant["soil"] += SOIL_WET_PER_S * dt
            plant["tank"] -= TANK_USE_PER_S * dt
        else:
            plant["soil"] -= SOIL_DRY_PER_S * dt
        plant["tank"] += TANK_FILL_PER_S * dt
        plant["soil"] = max(0.0, min(100.0, plant["soil"]))
        plant["tank"] = max(0.0, min(100.0, plant["tank"]))
        if pump_on() and plant["tank"] < TANK_MIN:        # ถังแห้งระหว่างเดิน: ดับเองกันปั๊มพัง
            plant["until"], plant["refuse"], plant["dry"] = 0.0, REFUSE_TANK, True
            return "ถังต่ำกว่า %d %% ระหว่างเดิน PLC ดับปั๊มเอง (HR4 = 1) รอน้ำถึง %d %%" % (TANK_MIN, TANK_RESUME)
        if was_on and not pump_on():                      # ครบเวลาสูงสุด
            plant["until"] = 0.0
            return "ปั๊มเดินครบ %d วิ PLC ดับเอง" % PUMP_MAX_S
        if plant["dry"] and plant["tank"] >= TANK_RESUME:
            plant["dry"] = False
            return "น้ำในถังกลับถึง %d %% แล้ว PLC ยอมเปิดปั๊มได้อีก" % TANK_RESUME
    return None


def registers():
    # ค่าทั้ง 5 ช่องตอนนี้ เป็นจำนวนเต็ม 16 บิต (register เก็บทศนิยมไม่ได้) ผู้เรียกต้องถือ lock อยู่
    # ถังปัดลงเสมอ: 9.9 % รายงานเป็น 9 ตรงกับที่ระบบป้องกันเห็นว่า "ต่ำกว่า 10" ไม่ใช่ปัดขึ้นเป็น 10
    left = max(0, int(plant["until"] - time.monotonic() + 0.999)) if pump_on() else 0
    return [1 if pump_on() else 0, int(round(plant["soil"])), int(plant["tank"]), left, plant["refuse"]]


# ---- 3) Modbus: PDU เข้า -> PDU ออก (ไม่แตะเครือข่าย ทดสอบได้โดยไม่ต้องเปิดพอร์ต) ----
def exception_pdu(fc, code):
    # คำตอบข้อยกเว้น = FC ที่ติดบิตสูง (0x80) + รหัสหนึ่งไบต์
    return bytes([(fc | 0x80) & 0xFF, code])


def read_holding(pdu):
    # FC03: [03][addr 2 ไบต์][จำนวนช่อง 2 ไบต์] -> [03][จำนวนไบต์][ค่าช่องละ 2 ไบต์ ...]
    if len(pdu) != 5:
        return exception_pdu(3, EXC_VALUE)
    addr, qty = struct.unpack(">HH", pdu[1:5])
    if not 1 <= qty <= 125:
        return exception_pdu(3, EXC_VALUE)
    if addr + qty > NUM_REGS:
        return exception_pdu(3, EXC_ADDRESS)
    with lock:
        regs = registers()[addr:addr + qty]
    return struct.pack(">BB%dH" % qty, 3, 2 * qty, *regs)


def write_single(pdu):
    # FC06: [06][addr 2 ไบต์][ค่า 2 ไบต์] -> ทำสำเร็จ = ทวนคำขอกลับทั้งก้อน
    if len(pdu) != 5:
        return exception_pdu(6, EXC_VALUE)
    addr, value = struct.unpack(">HH", pdu[1:5])
    if addr != HR_PUMP:                                   # HR1-HR4 อ่านได้อย่างเดียว เกินตารางก็ไม่มีอยู่จริง
        return exception_pdu(6, EXC_ADDRESS)
    with lock:
        if value not in (0, 1):
            plant["refuse"] = REFUSE_VALUE
            return exception_pdu(6, EXC_VALUE)
        if value == 1 and plant["tank"] < (TANK_RESUME if plant["dry"] else TANK_MIN):
            plant["refuse"] = REFUSE_TANK
            return exception_pdu(6, EXC_DEVICE)
        if value == 0:
            plant["until"] = 0.0
        elif not pump_on():                               # เขียน 1 ซ้ำตอนเดินอยู่ไม่ต่อเวลา กันคนสั่งรัว
            plant["until"] = time.monotonic() + PUMP_MAX_S
        plant["refuse"] = REFUSE_NONE
    return bytes(pdu)


def handle_pdu(pdu):
    # หัวใจของ PLC: รับ PDU หนึ่งก้อน คืน PDU คำตอบ
    fc = pdu[0] if pdu else 0
    if fc == 3:
        return read_holding(pdu)
    if fc == 6:
        return write_single(pdu)
    return exception_pdu(fc, EXC_FUNCTION)


def build_adu(tid, unit, pdu):
    # MBAP 7 ไบต์: เลขรายการ, protocol id (0 เสมอ), จำนวนไบต์ที่ตามมา (unit + PDU), unit id
    return struct.pack(">HHHB", tid, 0, len(pdu) + 1, unit) + pdu


def spaced(data):
    return " ".join("%02X" % b for b in data)


def describe(pdu, reply):
    # คำอธิบายภาษาคนของ PDU หนึ่งก้อน ไว้พิมพ์ต่อท้ายไบต์
    fc = pdu[0] if pdu else 0
    if fc & 0x80 and len(pdu) == 2:
        return "ข้อยกเว้น %02X = %s" % (pdu[1], EXC_TEXT.get(pdu[1], "?"))
    if fc == 3 and reply:
        return "ค่า = %s" % list(struct.unpack(">%dH" % (pdu[1] // 2), pdu[2:]))
    if fc in (3, 6) and len(pdu) == 5:
        a, b = struct.unpack(">HH", pdu[1:5])
        if fc == 3:
            return "FC03 อ่าน %d ช่อง เริ่ม HR%d" % (b, a)
        return ("FC06 ทำแล้ว HR%d = %d" if reply else "FC06 เขียน HR%d = %d") % (a, b)
    return "FC%02X" % fc


# ---- 4) เครือข่าย: Modbus TCP = MBAP 7 ไบต์ + PDU ----
def recv_exact(sock, n):
    # TCP ส่งมาเป็นท่อนได้ ต้องวนอ่านจนครบ n ไบต์ คืน None ถ้าอีกฝั่งปิดสายหรือเงียบเกินเวลา
    data = b""
    while len(data) < n:
        try:
            part = sock.recv(n - len(data))
        except OSError:
            return None
        if not part:
            return None
        data += part
    return data


class ModbusHandler(socketserver.BaseRequestHandler):
    # ไคลเอนต์ละหนึ่งเธรด ถามตอบได้หลายรอบบนสายเดียว จนอีกฝั่งปิดสายหรือเงียบเกิน IDLE_S
    def handle(self):
        self.request.settimeout(IDLE_S)
        while True:
            head = recv_exact(self.request, 7)
            if head is None:
                return
            tid, pid, length, unit = struct.unpack(">HHHB", head)
            if pid != 0 or not 2 <= length <= 254:
                log("<- %s  (ไม่ใช่ Modbus TCP ตัดสายทิ้ง)" % spaced(head))
                return
            pdu = recv_exact(self.request, length - 1)
            if pdu is None:
                return
            note = describe(pdu, False)
            if unit != UNIT_ID:        # Modbus TCP ส่วนใหญ่ไม่สน unit id (ใช้กับเกตเวย์ต่อสาย RS-485) จึงตอบให้
                note += ", unit %d ไม่ตรงแต่ตอบให้" % unit
            log("<- %s  (%s)" % (spaced(head + pdu), note))
            reply = handle_pdu(pdu)
            adu = build_adu(tid, unit, reply)
            log("-> %s  (%s)" % (spaced(adu), describe(reply, True)))
            try:
                self.request.sendall(adu)
            except OSError:
                return


class PLCServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True                 # ปิดแล้วเปิดใหม่ได้ทันที ไม่ต้องรอพอร์ตว่าง
    daemon_threads = True                      # Ctrl+C แล้วไม่ต้องรอไคลเอนต์ที่ค้างสายอยู่
    block_on_close = False


# ---- 5) โปรแกรมหลัก ----
def main():
    parser = argparse.ArgumentParser(description="PLC จำลองที่พูด Modbus TCP")
    parser.add_argument("--tank", type=float, default=TANK_START, help="น้ำในถังตอนเริ่ม (%%)")
    plant["tank"] = max(0.0, min(100.0, parser.parse_args().tank))
    try:
        server = PLCServer((HOST, PORT), ModbusHandler)
    except OSError as e:
        print("เปิดพอร์ต %d ไม่ได้ (%s) อาจมี PLC จำลองอีกตัวเปิดอยู่แล้ว" % (PORT, e))
        return 1
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print("PLC จำลอง Modbus TCP ที่ %s:%d unit %d | HR0 ปั๊ม HR1 ดิน HR2 ถัง HR3 เหลือวิ HR4 เหตุปฏิเสธ"
          % (HOST, PORT, UNIT_ID))
    last, next_status = time.monotonic(), 0.0
    try:
        while True:
            now = time.monotonic()
            event = step_plant(now - last)
            last = now
            if event:
                log(event)
            if now - T0 >= next_status:
                with lock:
                    regs = registers()
                log("สถานะ HR0-HR4 = %s  (ปั๊ม ดิน ถัง เหลือวิ เหตุปฏิเสธ)" % regs)
                next_status += STATUS_S
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    server.shutdown()
    server.server_close()
    print("ปิด PLC จำลองแล้ว")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
