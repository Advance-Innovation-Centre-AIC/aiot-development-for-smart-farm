# cp4_03_practise.py - แบบฝึกเติมโค้ด (Code Quest ระดับ 3 โบนัส): ตารางส่งต่อเหตุการณ์
#
# วิธีเล่น  : ไฟล์นี้เหมือน cp4_03_event_router.py ทุกอย่าง ยกเว้นฟังก์ชัน build_router()
#            ในส่วน "3) สมอง" ที่เว้นช่อง ____ (ขีดล่างสี่ตัว) ไว้ 3 ช่อง: A, B, C
#            เติมให้ครบแล้วรัน โปรแกรมจะป้อนเหตุการณ์ปลอม 5 กรณีเข้าตารางก่อนเปิดจอ (self_test)
#            ผ่านครบ = Console ขึ้น "ผ่าน!" แล้วเล่นต่อได้เหมือนไฟล์ตัวอย่าง
#            ยังไม่ถูก = Console บอกว่ากรณีไหนผิด แล้วหยุด (ยังไม่เปิดจอ)
# ถ้าเจอ   : NameError: name '____' isn't defined = ยังมีช่องที่ไม่ได้เติม (ตั้งใจให้หยุดชัด ๆ แบบนี้)
# ทบทวน    : เหตุการณ์หนึ่งใบ = {'handle': id ของ widget, 'type': ชื่อเหตุการณ์, 'value': ตัวเลข}
#            ปุ่ม = clicked (มาเสมอ) และ long_pressed (ต้อง listen ก่อน) · Slider/Roller = value_changed
#            Switch = toggled · ตารางใช้คู่ (id, type) เป็นกุญแจ
# ติดขัด?  : รันไฟล์ตัวอย่างเต็ม cp4_03_event_router.py เพื่อไปต่อก่อน แล้วค่อยกลับมาเทียบกับของตัวเอง
# เฉลย     : โจทย์โบนัส เฉลยอยู่กับผู้สอน (ลองเองก่อน แล้วค่อยขอดูเมื่อจำเป็น)
# บอร์ด    : TESAIoT Dev Kit และ BENTO Emulator
#
# (ทำจาก cp4_03_event_router.py 84b9ebbd339f)

import pots
import rgbmatrix
import time
import ui

# ---- 1) ตั้งค่า (แก้ได้) ----
TH_START = 40        # เกณฑ์ความชื้นดินเริ่มต้น (%) ตั้งต่อด้วย Slider
MODES = ("น้ำหยด", "พ่นฝอย", "สปริงเกอร์")   # ตัวเลือกบน Roller (value ที่ได้ = ลำดับ เริ่มที่ 0)
LOOP_MS = 30         # ดึงเหตุการณ์ทุกกี่ ms (คิวจุ 16 เต็มแล้วทิ้งของใหม่)
TICK_MS = 500        # อ่านดินและอัปเดตตัวเลขทุกกี่ ms
RUN_MS = 180000      # เล่นนาน 3 นาทีแล้วจบเอง
VOLUME = 25              # ความดังเสียง 0-127 (≈20%) ใช้กับทุกเสียงในไฟล์นี้
BTN_NAMES = ("SW5", "SW6")   # ชื่อบนแผง: SW5 = ปุ่มล่าง, SW6 = ปุ่มบน (ไฟล์นี้ใช้จอสัมผัสแทน)

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF


# ---- 2) ฮาร์ดแวร์ ----
def soil_percent():
    return pots.read(0) * 100 // 4095      # VR1: 0 = แห้งสนิท, 100 = แฉะ


# เสียงเตือนใช้ ui.tone เพราะปรับความดังได้ (ui.sfx ในเฟิร์มแวร์นี้ปรับความดังไม่ได้)
TUNES = {"tap": (76,), "start": (72, 79), "stop": (79, 72), "good": (72, 79, 84),
         "bad": (84, 76), "empty": (84, 76, 69), "hit": (88,)}


def beep(name):
    for n in TUNES[name]:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 90)
        time.sleep_ms(100)


def draw_matrix(pump, mode):
    # จอไฟ RGB: ปั๊มเดิน = เต็มจอสีตามวิธีรดน้ำ, ปั๊มหยุด = ดับ (เรียกเฉพาะตอนภาพเปลี่ยน)
    try:
        if pump:
            rgbmatrix.fill((rgbmatrix.BLUE, rgbmatrix.CYAN, rgbmatrix.GREEN)[mode])
        else:
            rgbmatrix.clear()
    except OSError:
        pass                    # จอไฟ RGB ตอบไม่ทัน: ข้ามภาพนี้ไป ไม่ให้โปรแกรมหยุด


# ---- 3) สมอง (ตัดสินใจ) ไม่แตะฮาร์ดแวร์ ----
# ตัวจัดการ: รับ (สถานะ s, เหตุการณ์ ev) แก้สถานะ แล้วคืนข้อความบอกคน (หรือ None)
def on_hold(s, ev):
    # กดค้างครบเวลา = ยืนยันแล้ว: เปิดปั๊มเอง (ปิดโหมดอัตโนมัติ)
    s["held"] = True               # จำไว้: clicked ที่ตามมาตอนปล่อย ไม่ใช่การแตะใหม่
    s["pump"], s["auto"] = True, False
    return "เปิดปั๊มแล้ว"


def on_tap(s, ev):
    # แตะสั้น = ยังไม่สั่ง แค่บอกวิธี
    if s["held"]:
        s["held"] = False          # นี่คือ clicked ตอนปล่อยหลังกดค้าง: ข้าม
        return None
    return "กดค้างเพื่อยืนยัน"


def on_stop(s, ev):
    # หยุด ชนะทุกอย่าง: ปิดปั๊มและปิดโหมดอัตโนมัติ
    s["pump"], s["auto"], s["held"] = False, False, False
    return "หยุดแล้ว"


def on_threshold(s, ev):
    s["th"] = ev["value"]          # Slider ส่ง value_changed พร้อมค่าบน Slider
    return None


def on_mode(s, ev):
    s["mode"] = ev["value"]        # Roller ส่ง value_changed พร้อมลำดับแถว เริ่มที่ 0
    return None


def on_auto(s, ev):
    s["auto"] = ev["value"] == 1   # Switch ส่ง toggled พร้อม 1 = เปิด, 0 = ปิด
    return None


def build_router(ids):
    # ตาราง (id, type) -> ตัวจัดการ  ids = {"ชื่อ": w.id()}  เหตุการณ์ที่ไม่อยู่ในตาราง = ไม่สนใจ
    return {
        (ids["pump"], "long_pressed"): on_hold,
        (ids["pump"], ____): on_tap,              # ช่อง A: แตะสั้นแล้วปล่อย ส่งเหตุการณ์ชื่ออะไร (มาเสมอ)
        (ids["stop"], "clicked"): ____,           # ช่อง B: ปุ่มหยุดควรไปหาตัวจัดการตัวไหน
        (ids["th"], "value_changed"): on_threshold,
        (ids["mode"], "value_changed"): on_mode,
        (ids["auto"], ____): on_auto,             # ช่อง C: Switch ส่งเหตุการณ์ชื่ออะไร
    }


def dispatch(router, s, ev):
    # หาตัวจัดการด้วยคู่ (id, type) แล้วเรียก คืนข้อความบอกคน หรือ None
    h = router.get((ev["handle"], ev["type"]))
    return h(s, ev) if h else None


def self_test():
    # ป้อนเหตุการณ์ปลอมเข้าตารางก่อนเปิดจอ (id ปลอม: ปั๊ม 1, หยุด 2, Switch 5)
    # กรณี 1 แตะเปิดปั๊ม · 2 กดค้าง · 3 ปล่อยหลังกดค้าง · 4 แตะหยุด · 5 เปิดสวิตช์อัตโนมัติ
    # แต่ละกรณี: (id, type, ปั๊มควรเปิดไหม, ควรมีข้อความบอกคนไหม, อัตโนมัติควรเปิดไหม)
    # ทุกกรณีส่ง value = 1 (Switch: 1 = เปิด) ตัวจัดการอื่นในกรณีเหล่านี้ไม่ได้อ่าน value
    # ไม่ผ่าน = Console พิมพ์ (id, type) ของกรณีที่ผิด ตามด้วยสิ่งที่ควรได้
    r = build_router({"pump": 1, "stop": 2, "th": 3, "mode": 4, "auto": 5})
    s = {"pump": False, "held": False, "auto": False}
    for c in ((1, "clicked", False, True, False), (1, "long_pressed", True, True, False),
              (1, "clicked", True, False, False), (2, "clicked", False, True, False),
              (5, "toggled", False, False, True)):
        note = dispatch(r, s, {"handle": c[0], "type": c[1], "value": 1})
        if (s["pump"], note is not None, s["auto"]) != c[2:]:
            print("ยังไม่ถูก:", c[:2], "ควรได้", c[2:])
            raise SystemExit
    print("ผ่าน! ครบ 5 กรณี")


# ---- 4) เครือข่าย (ไฟล์นี้ไม่ใช้) ----


# ---- 5) หน้าจอ ----
def build_screen():
    ui.screen()
    time.sleep_ms(200)
    ui.Label("เหตุการณ์ -> ตาราง -> ตัวจัดการ", x=12, y=6, color=COL_TEXT, value=24)
    w = {}
    # long_pressed ต้องขอก่อน (listen คืนตัว widget เอง จึงต่อท้ายได้) เรียก listen ซ้ำ = แทนชุดเดิม
    w["pump"] = ui.Button("เปิดปั๊ม (กดค้าง)", x=12, y=56, w=250, h=80,
                          color=COL_INFO).listen("long_pressed")
    w["stop"] = ui.Button("หยุด", x=274, y=56, w=150, h=80, color=COL_BAD)
    # Roller = วิธีรดน้ำ: ตัวเลือกคั่นด้วย \n ส่งไปทีเดียวตอนสร้าง (หรือเติมทีละตัวด้วย add_option)
    w["mode"] = ui.Roller("\n".join(MODES), x=440, y=56, w=160, h=130, color=COL_TEXT)
    w["led"] = ui.Led(x=620, y=60, w=56, h=56, color=COL_INFO, value=0)       # ไฟปั๊มบนจอ
    ui.Label("ปั๊ม", x=690, y=74, color=COL_DIM, value=20)
    w["auto"] = ui.Switch(x=620, y=140, w=64, h=32, value=0)
    ui.Label("อัตโนมัติ", x=694, y=146, color=COL_DIM, value=16)
    w["soil"] = ui.Label(" ", x=12, y=150, color=COL_TEXT, value=20)
    w["th"] = ui.Slider(x=24, y=190, w=388, h=24, min=0, max=100, value=TH_START)
    w["last"] = ui.Label(" ", x=12, y=236, color=COL_WARN, value=24)
    w["note"] = ui.Label(" ", x=12, y=278, color=COL_TEXT, value=20)
    w["help"] = ui.Label("แตะ = ถาม  กดค้าง = เปิด  หยุด = ปิด", x=12, y=352,
                         color=COL_DIM, value=16)
    ui.poll()
    return w


def show_pump(w, s, was):
    # ปั๊มหรือโหมดเปลี่ยน: ไฟบนจอ จอไฟ RGB เสียง และ Switch ให้ตรงสถานะจริง
    w["led"].value(1 if s["pump"] else 0)            # Led: 0 = หรี่ (ไม่ดับมืด), 1 = สว่าง
    # สองทาง: บอร์ดเปลี่ยนสถานะเอง (กดค้าง/หยุด) ก็ต้องเลื่อน Switch ตาม
    # .value() ไม่ส่ง toggled กลับมา จึงไม่เกิดคำสั่งซ้ำ
    w["auto"].value(1 if s["auto"] else 0)
    draw_matrix(s["pump"], s["mode"])
    if was is not None and s["pump"] != was[0]:
        beep("start" if s["pump"] else "stop")


# ---- 6) โปรแกรมหลัก ----
def main():
    self_test()                           # ตรวจตารางก่อน ไม่ผ่าน = หยุดตรงนี้
    w = build_screen()
    ids = {}
    for k in ("pump", "stop", "th", "mode", "auto"):
        ids[k] = w[k].id()
    router = build_router(ids)
    s = {"pump": False, "held": False, "th": TH_START, "mode": 0, "auto": False}
    shown = soil_shown = None
    soil = 100
    t_tick = t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        evs = ui.poll()                                # 1) ฟัง: ดึงจนคิวว่าง (ครั้งละ <= 8)
        last = None
        while evs:
            for ev in evs:
                p = s["pump"]
                note = dispatch(router, s, ev)         # 2) ตาราง -> ตัวจัดการ
                last = ev
                if note:
                    w["note"].text(note)
                    if s["pump"] == p:
                        beep("tap")                       # ปั๊มไม่เปลี่ยน = เสียงกด (ปั๊มเปลี่ยน show_pump ส่งเสียงเอง)
            evs = ui.poll()
        if last:                                       # ลาก Slider ได้หลายใบต่อรอบ: เขียนจอครั้งเดียว
            w["last"].text("%s = %d" % (last["type"], last["value"]))
        now = time.ticks_ms()
        if time.ticks_diff(now, t_tick) >= TICK_MS:
            t_tick = now
            soil = soil_percent()
            if (soil, s["th"]) != soil_shown:
                w["soil"].text("ดิน %d %%  เกณฑ์ %d %%" % (soil, s["th"]))     # ดิน = VR1
                soil_shown = (soil, s["th"])
        if s["auto"]:
            s["pump"] = soil < s["th"]                 # โหมดอัตโนมัติ: ดินแห้งกว่าเกณฑ์ = เปิด
        pic = (s["pump"], s["mode"], s["auto"])
        if pic != shown:                               # 3) ทำ/โชว์ เฉพาะตอนสถานะเปลี่ยน
            show_pump(w, s, shown)
            shown = pic
        time.sleep_ms(LOOP_MS)
    draw_matrix(False, 0)                              # จบรอบ: ดับจอไฟ RGB
    w["help"].color(COL_WARN)
    w["help"].text("จบรอบแล้ว - กด Program to Device อีกครั้งเพื่อเล่นใหม่")
    ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) เดาก่อนรัน: แตะ "เปิดปั๊ม" หนึ่งครั้ง บรรทัดตัวส้ม (เหตุการณ์ล่าสุด) จะขึ้นชนิดอะไร
#    กดค้าง 1 วิ แล้วปล่อย จะมีกี่เหตุการณ์ ชื่ออะไรบ้าง ตามลำดับไหน
#    (pressed ไม่มาเพราะเราไม่ได้ขอด้วย listen)
# 2) ลบ .listen("long_pressed") ออก แล้วลองกดค้าง ปั๊มยังเปิดได้ไหม เพราะอะไร
# 3) ลบบรรทัด s["held"] = True ใน on_hold แล้วกดค้าง ดูว่าข้อความไหนโผล่ผิดจังหวะ
