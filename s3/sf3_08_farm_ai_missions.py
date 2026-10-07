# sf3_08_farm_ai_missions.py - ภารกิจ AI ในฟาร์ม: กฎที่เขียนเอง + AI บนบอร์ด แล้วดูว่าเห็นตรงกันไหม
#
# ภารกิจ   : เลือกภารกิจด้วย MISSION บรรทัดเดียว (ตารางข้างล่าง) บอร์ดตัดสินสองทางพร้อมกัน
#            1) กฎที่อ่านออก เช่น "ค่าสั่นเกินค่าปกติ x 3"  2) โมเดล Edge AI (ป้ายของภารกิจชนะ >= 60 % ใน 3 ของ 5 ผลล่าสุด)
#            แล้วบอกว่าเห็นตรงกันไหม: both = ทั้งคู่ · ai = AI ฝ่ายเดียว · rule = กฎฝ่ายเดียว · none = ไม่มีใคร
#            ระดับ 2 (both) = เตือนจริง: เสียง + ไฟแดง + จอไฟ RGB ซ้ำจนมีคนรับทราบ (SW6 หรือแอปส่ง ack)
#            ระดับ 1 (ฝ่ายเดียว) = ให้คนไปดู ไม่ส่งเสียง · ระดับ 0 = ปกติ
#            รับทราบแล้ว จะเตือนใหม่เมื่อเหตุสงบต่อเนื่อง 10 วิ (ACK_REARM_MS) กันเตือนซ้ำตอนป้ายสลับไปมา
#            worker: นิ่งครบ 30 วิ = ถาม "โอเคไหม" (AI เห็นด้วย = ระดับ 2) · เตือนเมื่อไม่ตอบใน 30 วิ (alarm ไม่ขึ้นกับระดับ)
#            voice: ไม่มีการเตือน · ระดับ 2 = AI ได้ยินคำ + ยืนยันแล้ว (พูดซ้ำหรือกด SW6) · ปุ่มแทนเสียงไม่นับเป็น AI
#            ส่งสถานะขึ้น bento-aiot/<TEAM>/ai และเหตุการณ์ขึ้น .../event ตามสัญญา MQTT ข้อ 3.11
# ลองเล่น  : pump   วางบอร์ดนิ่ง 5 วิแรก (เรียนค่าปกติ) แล้วเขย่าแรง ๆ -> ระดับ 2 -> กด SW6 รับทราบ
#                   เคาะโต๊ะเบา ๆ = กฎเห็นฝ่ายเดียว (ระดับ 1) · ใน Emulator ใช้ปุ่ม Shake
#            siren  เปิดคลิปไซเรนจากมือถือใกล้บอร์ด · barn ไอใส่บอร์ด หรือเปิดก๊อกน้ำ
#            night  เปิดคลิปเลื่อยยนต์ ไฟปะทุ หรือสุนัขเห่า (ฝนตก = event rain)
#            voice  พูด go แล้วพูดซ้ำหรือกด SW6 ภายใน 3 วิ = เปิดปั๊ม · พูด stop = ปิดทันที
#            worker ติดบอร์ดที่อก นิ่ง 30 วิ บอร์ดจะถาม -> กด SW6 = ฉันโอเค · ไม่ตอบ 30 วิ = เตือน
#            drill  ผู้สอนสาธิตเท่านั้น: ยึดชิ้นงานให้แน่น สวมแว่นและที่ครอบหู
#            ดูผลในแอปของกลุ่ม หรือหน้าแจ้งเตือนของคาบ 3 ใน s3/app/ (ใช้ TEAM เดียวกัน)
# ของบนบอร์ดที่ใช้ : แกน AI (edge_ai) + IMU BMI270 หรือไมโครโฟนตามภารกิจ, ปุ่ม SW6 (บน) และ SW5 (ล่าง),
#            ลำโพง (ดังตอนเริ่มเตือน ซ้ำทุก 10 วิ และตอนหายเตือน), ไฟ RGB_RED = กำลังเตือน,
#            จอไฟ RGB: ข้อความอังกฤษตอนเตือน เช่น STOP PUMP, OPEN GATE, STOP DRILL · ปกติ = แถบความมั่นใจของ AI
# บนจอ     : ภารกิจและปัญหาในฟาร์ม · AI ตอบว่า (ป้าย ความหมายไทย มั่นใจ % อันดับสอง) · กฎ (ค่า / เกณฑ์ + หน่วย)
#            ตาราง 2x2 "AI กับกฎ เห็นตรงกันไหม" · ระดับ 0/1/2 (สีเขียว/ส้ม/แดง) · การเตือนและวิธีรับทราบ
#            "AI เงียบ / ไม่มีผล" เมื่อโมเดลไม่ส่งผลใหม่ · "กฎอ่านค่าไม่ได้ระหว่างโมเดลรัน" เมื่ออ่านเซนเซอร์ไม่ได้
# ปุ่ม      : SW6 (บน) = รับทราบการเตือน / ฉันโอเค · SW5 (ล่าง) = พัก / ทำต่อ
#            ภารกิจ voice ที่ไม่มีโมเดล ใช้ปุ่มแทนเสียง (ไม่ใช่ AI): SW5 = ได้ยิน go · SW6 = ยืนยัน go ที่รออยู่
#            ถ้าไม่มี go รอ SW6 = ได้ยิน stop (ปิดปั๊มทันที) · จอบอกตลอดว่าปุ่มไหนทำอะไร
# แนวคิด AIoT: AI ไม่ใช่ความจริง เป็น "ความเห็นที่สอง" คู่กับกฎที่อ่านออก ทั้งคู่ตรงกัน = มั่นใจพอให้ดังลั่น
#            เห็นไม่ตรงกัน = ให้คนไปดู แล้วคนตอบกลับ (ai/feedback) เป็นข้อมูลไปฝึกโมเดลรอบหน้า
#            บอร์ดตัดสินเองบนฟาร์ม (Edge) ส่งแค่ผลสรุปขึ้น MQTT ข้อมูลดิบไม่ออกนอกฟาร์ม
# โมเดล    : หาด้วย "ชื่อ" (ไม่สนตัวพิมพ์ใหญ่เล็ก) ตามลำดับในตาราง: โมเดลจาก Store ก่อน ไม่มีหรือเลือกไม่สำเร็จ
#            จึงใช้ตัวสำรองที่ติดมากับบอร์ด ไม่มีทั้งคู่ = ใช้กฎอย่างเดียว (ส่ง "model": "rule-only")
#   MISSION ปัญหาในฟาร์ม                โมเดลจาก Store           ตัวสำรองในตัว / Emulator    ป้ายที่นับเป็นเหตุ              กฎคู่ (rk)
#   pump    ปั๊มน้ำสั่นผิดปกติ            AnomalousVibration       Motion Detection          anomaly (ตัวสำรอง: shaking)     vib  ค่าสั่น > ค่าปกติ x 3
#   siren   รถฉุกเฉินมาหน้าฟาร์ม          SirenDetection           Siren Detection           sirens                        loud ดังต่อเนื่อง >= 2 วิ
#   barn    หมูไอถี่ / ก๊อกน้ำเปิดทิ้ง     HomeSounds               Cough Detection           cough water_tap baby_cry      rate ไอ >= 10 ครั้งใน 10 นาที · dur ก๊อกเปิด >= 30 วิ
#   night   ยามกลางคืน                  Environment Sounds       Alarm Detection           chainsaw crackling_fire dog   loud ดังต่อเนื่อง >= 2 วิ (+ event rain)
#                                                                                      (ตัวสำรอง: alarm)
#   voice   สั่งปั๊มด้วยเสียง (Gateway)    Voice Commands           ไม่มี: ปุ่มแทนเสียง          go stop                       btn  ยืนยัน go: พูดซ้ำใน 3 วิ หรือกด SW6 (+ voice_cmd, plc/cmd)
#   worker  คนงานอยู่คนเดียว             HumanActivity            ไม่มี: IMU อย่างเดียว        sitting standing (นิ่ง)         dur  ไม่ขยับ >= 30 วิ แล้วถาม (+ event check_in)
#   drill   สว่านทะลุชิ้นงาน (ผู้สอน)       DrillMaterialMic         ไม่มี: ความดังอย่างเดียว     wood_out plastic_out          loud สว่านเดินอยู่ (+ event drill_done)
#            night เฝ้าตลอดที่รัน (SW5 = พัก) · ไอเดียต่อยอด: เฝ้าเฉพาะชั่วโมงกลางคืน (ต้องตั้งนาฬิกาบอร์ดเองก่อน)
#            ส่งโมเดลลงบอร์ด: เปิด edgeai-store.tesaiot.dev -> sign in ด้วย GitHub -> เลือกโมเดลตามตาราง
#            -> กด Deploy ลงบอร์ดที่ต่อ USB อยู่ -> รันไฟล์นี้ใหม่ จอบรรทัด "โมเดล:" บอกว่าใช้ตัวไหนอยู่
#            ชื่อบนบอร์ดอาจไม่เหมือนชื่อในหน้า Store (เช่น Store "Siren" = บอร์ด "SirenDetection") ใช้ชื่อบนบอร์ดเสมอ
#            โมเดลตัวอย่างไม่ได้ฝึกจากฟาร์มจริง ใช้เรียนแนวคิดเท่านั้น ห้ามใช้แทนอุปกรณ์ความปลอดภัยจริง
# บอร์ด     : TESAIoT Dev Kit (firmware 2.4.2 ขึ้นไป) และ BENTO Emulator
#            Emulator มีแค่โมเดลในตัวและเป็นผลจำลอง ไม่มีโมเดลจาก Store · worker และ voice (ปุ่มแทนเสียง) เล่นได้ครบ
#            pump: ปุ่ม Shake = ค่าสั่นขึ้น + ป้าย shaking มั่นใจราว 90 % → ยืนยันสองทาง (ระดับ 2) · เคาะเบา ๆ = กฎฝ่ายเดียว
#            ภารกิจเสียง (siren barn night drill): ปุ่ม POTEN บนแผงคือ "ความดังรอบบอร์ด" หมุนเกินครึ่ง = ดังขึ้น
#            (mic.level() และโมเดลเสียงในตัวใช้ปุ่มเดียวกัน) · ตัวเลขยังเป็นของจำลอง ตั้งเกณฑ์จริงบนบอร์ด
# สัญญา MQTT: s2/app/MQTT_CONTRACT_th.md ข้อ 3.10 3.11 4.2 และ 5 (client id bento-ai-<TEAM>-<สุ่ม 4 ตัว>)
# ต้องแก้ก่อนรัน: MISSION, WIFI_SSID, WIFI_PASS และ TEAM · TEAM ยังเป็น teamXX = ออฟไลน์ (พิมพ์ข้อความลง Console แทน)

import buttons
import edge_ai
import gpio
import json
import math
import mqtt
import rgbmatrix
import sensors
import time
import ui
import wifi

try:
    import mic
except ImportError:          # เฟิร์มแวร์ที่ไม่มีโมดูลไมค์: กฎเสียงขึ้นว่าอ่านค่าไม่ได้ แต่โปรแกรมยังรัน
    mic = None

# ---- 1) ตั้งค่า (แก้ได้) ----
MISSION = "pump"                       # pump siren barn night voice worker drill (ดูตารางข้างบน)
WIFI_SSID = "<ชื่อ Hotspot ของกลุ่ม>"   # ตั้งเอง: อังกฤษ/ตัวเลขสั้น ๆ ไม่มีเว้นวรรค
WIFI_PASS = "<รหัส Hotspot ของกลุ่ม>"   # อย่างน้อย 8 ตัว · อย่าส่งไฟล์ที่ใส่รหัสจริงให้ใคร
TEAM = "teamXX"                        # เลขกลุ่มที่ผู้สอนแจก เช่น team05 (ต้องตรงกับแอป)
BROKER = "broker.hivemq.com"           # สำรอง test.mosquitto.org (ต้องเปลี่ยนพร้อมแอป)
CLIENT_ID = "bento-ai-" + TEAM         # + เลขสุ่ม 4 ตัวทุกครั้งที่ต่อ: ไม่ชน id เก่า
BASE = "bento-aiot/" + TEAM + "/"      # หัวข้อ: BASE + "ai" / "event" / "ai/cmd" / "plc/cmd"

# ฝั่ง AI (นับแบบเดียวกับ sf3_04)
CONF_MIN = 60            # มั่นใจไม่ถึงกี่ % ไม่นับเป็นเหตุ
CONFIRM_N = 3            # ป้ายของภารกิจต้องชนะอย่างน้อยกี่ครั้ง ใน WINDOW_N ผลล่าสุด
WINDOW_N = 5             # ดูผลย้อนหลังกี่ครั้ง (ต้องไม่น้อยกว่า CONFIRM_N)
STALL_MS = 8000          # ไม่มีผลใหม่นานเท่านี้ = "AI เงียบ / ไม่มีผล"
MUTE_MS = 800            # หลังบอร์ดส่งเสียงเอง: โมเดลที่ฟังไมค์และกฎเสียงไม่เชื่อช่วงนี้
SENSOR_MIC = 2           # ค่าช่อง "sensor" ของโมเดลที่ฟังไมโครโฟน
RAIN = "rain"            # night: ป้ายนี้ชนะ 3 ใน 5 = ส่ง event rain (ไม่ใช่การเตือน)

# ฝั่งกฎ
SAMPLE_MS = 50           # อ่าน IMU / ไมค์ทุกกี่ ms (= รอบของลูป)
WIN = 20                 # pump/worker: คิดค่าสั่นจากกี่ตัวอย่างล่าสุด (เหมือน sf3_03) · อ่านพลาดติดกันเท่านี้ = อ่านไม่ได้
LEARN_MS = 5000          # pump: เรียนค่าสั่นปกติกี่ ms แรก (วางนิ่ง) เหมือน sf3_03
MIN_BASE = 0.05          # pump: ค่าปกติต่ำสุด (m/s^2)
K_VIB = 3.0              # pump: ค่าสั่นเกินค่าปกติกี่เท่า = เกิดเหตุ (sf3_03 เรียกว่า K_WARN)
MIC_SENS = 3             # ความไวไมค์ 1-5 (เหมือน sf3_02)
LOUD_LV = 60             # mic.level() ตั้งแต่เท่านี้ = ดัง (เงียบ ~7 · พูด ~54 · ตะโกน/ตบมือ 92-100)
QUIET_GAP_MS = 400       # เงียบสั้นกว่านี้ยังนับว่าดังต่อเนื่อง (ไซเรนขึ้นลง สว่านสะดุด)
LOUD_S = {"siren": 2, "night": 2, "drill": 1}   # ดังต่อเนื่องกี่วินาที = เกิดเหตุ
COUGH_MAX = 10           # barn: ไอกี่ครั้งใน RATE_MS = ไอถี่ (1 ช่วงไอติดกันนับ 1 ครั้ง)
RATE_MS = 600000         # barn: นับย้อนหลัง 10 นาที
TAP_S = 30               # barn: ได้ยินก๊อกน้ำต่อเนื่องกี่วินาที = เปิดทิ้ง
MOVE_VIB = 0.3           # worker: ค่าสั่นเกินนี้ (m/s^2) = ขยับ
STILL_S = 30             # worker: ไม่ขยับกี่วินาที = ถาม "โอเคไหม"
CHECKIN_S = 30           # worker: ถามแล้วไม่ตอบกี่วินาที = เตือน

# ภารกิจ voice: บอร์ดเป็น Gateway สั่ง PLC (สัญญาข้อ 4.2)
PUMP_SEC = 10            # go = เปิดปั๊มกี่วินาที
PUMP_MAX_S = 30          # เพดานที่ส่งให้ PLC (PLC ตัดที่ 30 ซ้ำอีกชั้น)
GO_CONFIRM_MS = 3000     # go ต้องยืนยันภายในเท่านี้ (ได้ยิน go ซ้ำ หรือกด SW6)
PLC_GAP_MS = 3000        # plc/cmd ห่างกันอย่างน้อยเท่านี้ (stop ไม่ต้องรอ)

# MQTT และจังหวะ
HEARTBEAT_MS = 2000      # สถานะไม่เปลี่ยน ก็ยังส่งซ้ำทุกเท่านี้
GAP_MS = 200             # ห้ามส่งสถานะถี่กว่านี้ (broker สาธารณะ ใช้ร่วมกันหลายกลุ่ม)
MAX_BYTES = 255          # สถานะหนึ่งข้อความยาวไม่เกินกี่ไบต์ (เกินตัด p2/c2 ก่อน แล้วตัด ms)
CMD_MAX = 200            # คำสั่ง ai/cmd ยาวเกินนี้ไม่สนใจ
CMD_GAP_MS = 3000        # คำสั่ง select / stop / run ถี่กว่านี้ไม่ทำ (สัญญาข้อ 3.10)
ALARM_REPEAT_MS = 10000  # เตือนอยู่: ดังซ้ำทุกเท่านี้จนมีคนรับทราบ (0 = ดังครั้งเดียว)
ACK_REARM_MS = 10000     # รับทราบแล้ว จะเตือนใหม่เมื่อเหตุสงบต่อเนื่องนานเท่านี้ (กันเตือนซ้ำตอนป้ายสลับไปมา)
DRAW_MS = 500            # วาดจอทุกกี่ ms
MATRIX_MS = 3000         # ส่งภาพจอไฟ RGB ซ้ำทุกกี่ ms (กันภาพหล่นหาย)
SCROLL_MS = 80           # ความเร็วตัววิ่งบนจอไฟ RGB
VOICE_SHOW_MS = 5000     # voice: ข้อความคำสั่งค้างบนจอไฟ RGB กี่ ms
RUN_MS = 1800000         # รันนานเท่าไร (30 นาที)
VOLUME = 25              # ความดังเสียง 0-127 (≈20%)
SPEAKER = 40             # ความดังลำโพงรวม 0-100% (ใช้ได้กับ firmware 2.4.2 ขึ้นไป)
SW6, SW5 = 0, 1          # ลำดับปุ่มใน buttons: 0 = SW6 (บน) · 1 = SW5 (ล่าง)

# ภารกิจ: (ชื่อไทย, ปัญหาในฟาร์ม, ชื่อโมเดลที่ลองตามลำดับ, ป้ายที่นับเป็นเหตุ, กฎคู่ rk, ข้อความเตือนบนจอไฟ RGB)
MISSIONS = {
    "pump": ("หมอปั๊มน้ำ", "ปั๊มสั่นผิดปกติ ซ่อมก่อนพัง",
             ("AnomalousVibration", "Motion Detection"), ("anomaly", "shaking"), "vib", "STOP PUMP"),
    "siren": ("ยามฟังไซเรน", "รถฉุกเฉินมา ต้องเปิดประตู",
              ("SirenDetection", "Siren Detection"), ("sirens",), "loud", "OPEN GATE"),
    "barn": ("หูโรงเรือน", "หมูไอถี่ / ก๊อกน้ำเปิดทิ้ง",
             ("HomeSounds", "Cough Detection"), ("cough", "water_tap", "baby_cry"), "rate", "CHECK BARN"),
    "night": ("ยามกลางคืน", "เลื่อยยนต์ ไฟไหม้ สุนัข ตอนดึก",
              ("Environment Sounds", "Alarm Detection"), ("chainsaw", "crackling_fire", "dog", "alarm"),
              "loud", "CHECK FARM"),
    "voice": ("สั่งปั๊มด้วยเสียง", "พูด go / stop สั่งปั๊มผ่าน PLC",
              ("Voice Commands",), ("go", "stop"), "btn", "PUMP"),
    "worker": ("คนงานปลอดภัย", "คนงานคนเดียว ล้มหรือหมดสติ",
               ("HumanActivity",), ("sitting", "standing"), "dur", "SOS"),
    "drill": ("ช่างซ่อมโรงเรือน", "สว่านทะลุชิ้นงาน (ผู้สอนสาธิต)",
              ("DrillMaterialMic",), ("wood_out", "plastic_out"), "loud", "STOP DRILL"),
}
RULE_ONLY = "rule-only"  # ชื่อที่ส่งใน "model" เมื่อบอร์ดไม่มีโมเดลของภารกิจ ("" = หยุดอยู่ ตามข้อ 3.10)
NO_MODEL = {"voice": "ไม่มีโมเดล: ปุ่มแทนเสียง (ไม่ใช่ AI)", "worker": "ไม่มีโมเดล: ใช้ IMU อย่างเดียว",
            "drill": "ไม่มีโมเดล: ใช้ความดังอย่างเดียว"}
LOOK_TEXT = "LOOK"       # จอไฟ RGB ตอนระดับ 1 (อังกฤษเท่านั้น)
CHECK_TEXT = "OK? SW6"   # worker: จอไฟ RGB ตอนถาม
VOICE_SAY = {"go": "PUMP ON", "stop": "PUMP OFF", "no": "NO"}

# แปลป้ายของโมเดลเป็นภาษาฟาร์ม (ชื่อป้ายต้องตรงกับที่บอร์ดรายงาน)
MEANING = {
    "-": "ยังไม่มีผล", "unlabeled": "ปกติ / ไม่มีเหตุ", "unlabelled": "ปกติ / ไม่มีเหตุ",
    "anomaly": "สั่นผิดปกติ!", "idle": "นิ่ง", "circle": "หมุนเป็นจังหวะ", "shaking": "สั่นแรงผิดปกติ!",
    "sirens": "เสียงไซเรน!", "cough": "เสียงไอ", "water_tap": "ก๊อกน้ำเปิดอยู่", "baby_cry": "เสียงร้องแบบเด็ก",
    "alarm": "เสียงสัญญาณเตือน", "chainsaw": "เลื่อยยนต์!", "crackling_fire": "ไฟไหม้!", "dog": "สุนัขเห่า",
    "rain": "ฝนตก", "background": "เสียงพื้นหลัง", "crying_baby": "เด็กร้อง", "sneezing": "จาม",
    "helicopter": "เฮลิคอปเตอร์", "go": "go = เปิดปั๊ม", "stop": "stop = ปิดปั๊ม",
    "left": "ซ้าย", "right": "ขวา", "up": "ขึ้น", "down": "ลง",
    "sitting": "นั่งนิ่ง", "standing": "ยืนนิ่ง", "walking": "เดิน", "running": "วิ่ง", "jumping": "กระโดด",
    "air": "สว่านหมุนลอย", "wood": "กำลังเจาะไม้", "plastic": "กำลังเจาะพลาสติก",
    "wood_out": "ทะลุไม้แล้ว!", "plastic_out": "ทะลุพลาสติกแล้ว!",
}
LVL_TEXT = ("ระดับ 0: ปกติ", "ระดับ 1: ฝ่ายเดียว ไปดู", "ระดับ 2: ยืนยันสองทาง!")

COL_TEXT, COL_DIM = 0xE8EAED, 0x9AA3AF
COL_CARD = 0x171B22
COL_OK, COL_WARN, COL_BAD, COL_INFO = 0x30A46C, 0xF5A623, 0xE5484D, 0x4A9EFF
LVL_COL = (COL_OK, COL_WARN, COL_BAD)


# ---- 2) ฮาร์ดแวร์ ----
def beep(*notes):
    # เสียงเบา ๆ แทน ui.sfx (ui.sfx ดังคงที่ ปรับเบาไม่ได้) · เล่นโน้ต MIDI ทีละตัว ห่างกัน 120 ms
    for n in notes:
        ui.tone(n, ui.WAVE_SINE, VOLUME, 120)
        time.sleep_ms(120)


def led_named(name):
    # หา LED ด้วยชื่อ ไม่ใช่เลข: บน Dev Kit ดวง LED1/LED2 อยู่บน SoM มองไม่เห็น
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


class Button:
    # อ่านใน wait_ms จึงไม่พลาดการกดสั้น ๆ · pressed_now() = True ครั้งเดียวต่อการกด
    def __init__(self, index):
        self.index, self.down, self.clicked = index, False, False

    def sample(self):
        now_down = buttons.pressed(self.index)
        if now_down and not self.down:
            self.clicked = True
        self.down = now_down

    def pressed_now(self):
        fired, self.clicked = self.clicked, False
        return fired


def wait_ms(ms, btns):
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < ms:
        for b in btns:
            b.sample()
        time.sleep_ms(20)


def find_models(keys):
    # หาโมเดลจาก "ชื่อ" ตามลำดับใน keys เพราะลำดับบนแต่ละบอร์ดไม่เหมือนกัน (เปลี่ยนได้หลังรีบูต)
    # คืนรายการ (ลำดับ, ชื่อ, ชื่อคลาส) ทุกตัวที่ชื่อตรง ตัวแรกเลือกไม่สำเร็จจะลองตัวถัดไป
    try:
        ms = edge_ai.models()
    except Exception:
        return []               # แกน AI ไม่ตอบ = ถือว่าไม่พบ
    return [(m["index"], m["name"], m["labels"]) for key in keys for m in ms if key.lower() in m["name"].lower()]


def uses_mic(index):
    # โมเดลนี้ฟังไมโครโฟนไหม: อ่านช่อง "sensor" ช่องเดียว แล้วเก็บแค่ True/False
    try:
        return edge_ai.model(index)["sensor"] == SENSOR_MIC
    except Exception:
        return False


def read_result():
    # ขอคำตอบล่าสุด ถ้าลิงก์ไปแกน AI ตอบไม่ทัน (OSError) รอบนี้ข้ามไป ไม่ให้โปรแกรมหยุด
    try:
        return edge_ai.result()
    except OSError:
        return None


def stop_ai():
    try:
        edge_ai.stop()
    except OSError:
        pass


def magnitude():
    # ขนาดความเร่งรวมสามแกน (m/s^2) แบบ sf3_03 · อ่านไม่ได้คืน None (ไม่เดาค่าแทน)
    # Exception ทุกชนิด: บนบอร์ดที่โมเดล IMU รันอยู่ ยังไม่รู้ว่าอ่านพร้อมกันได้ไหม
    try:
        ax, ay, az, _, _, _ = sensors.bmi270.motion()
    except Exception:
        return None
    return math.sqrt(ax * ax + ay * ay + az * az)


def start_mic():
    # เปิดไมค์หลังเลือกโมเดลแล้ว (ให้ AI ได้ก่อน) · เปิดไม่ได้ = กฎเสียงอ่านค่าไม่ได้
    if mic is None:
        return False
    try:
        mic.start(sens=MIC_SENS)
        return True
    except Exception as e:
        print("mic.start:", e)
        return False


def mx(cmd, *args):
    # สั่งจอไฟ RGB · ตอบไม่ทันก็ข้ามภาพนี้ไป
    try:
        cmd(*args)
    except OSError:
        pass


def ascii_only(text):
    # จอไฟ RGB รับเฉพาะอักษรอังกฤษ ตัวเลข เครื่องหมาย (เหมือน sf2_06)
    return "".join(c for c in str(text) if " " <= c <= "~")[:20]


# ---- 3) สมอง (ตัดสินใจ) ----
def alert_rule(hist, alerting, danger):
    # จำผล WINDOW_N ครั้งล่าสุด (1 = เหตุ) · เกิดเหตุเมื่ออย่างน้อย CONFIRM_N ครั้งในนั้น (เหมือน sf3_04)
    # หายเมื่อผลทั้งหน้าต่างกลับมาปกติหมด (กันกะพริบ เพราะป้ายสลับไปมาได้)
    hist = (hist + [int(danger)])[-WINDOW_N:]
    hits = sum(hist)
    return hist, hits >= CONFIRM_N or (alerting and hits > 0)


def second_best(r, labels):
    # ป้ายอันดับสองและความมั่นใจ % (บอกว่าโมเดลลังเลแค่ไหน) · ไม่มี = ("-", 0)
    sc, top, j = r.get("scores") or (), r.get("top", -1), -1
    for i in range(len(sc)):
        if i != top and (j < 0 or sc[i] > sc[j]):
            j = i
    if j < 0:
        return "-", 0
    return (labels[j] if j < len(labels) else "-"), int(sc[j] * 100)


def ceil_ms(v):
    # เวลาคิดปัดขึ้นเป็นจำนวนเต็ม: 0.4 ms -> 1 (0 แปลว่า "ไม่รู้" ตามสัญญา)
    return int(v) + (1 if v > int(v) else 0)


def spread(xs):
    # ส่วนเบี่ยงเบนมาตรฐาน = ค่าสั่น (เหมือน sf3_03)
    m = sum(xs) / len(xs)
    return math.sqrt(sum((x - m) * (x - m) for x in xs) / len(xs))


def agreement(ai_on, rule_on):
    # ตาราง 2x2: ทั้งคู่ = ระดับ 2 · ฝ่ายเดียว = ระดับ 1 · ไม่มีใคร = ระดับ 0
    if ai_on and rule_on:
        return "both", 2
    if ai_on:
        return "ai", 1
    if rule_on:
        return "rule", 1
    return "none", 0


def should_send(changed, since_ms):
    # ส่งเมื่อ: ห่างครั้งก่อนพอ และ (มีอะไรเปลี่ยน หรือ ถึงเวลา heartbeat) เหมือน sf3_06
    if since_ms < GAP_MS:
        return False
    return changed or since_ms >= HEARTBEAT_MS


def fit(msg):
    # ข้อความสถานะต้องไม่เกิน MAX_BYTES ไบต์ วัดจากข้อความจริงที่บอร์ดจัดรูปแล้ว (นับเป็นไบต์)
    # เกินเมื่อไร ตัด p2/c2 ก่อน แล้วค่อยตัด ms (สัญญาข้อ 3.11) · ทางสุดท้าย: ตัดท้ายชื่อโมเดลให้พอดี
    line = json.dumps(msg)
    for keys in (("p2", "c2"), ("ms",)):
        if len(line.encode()) <= MAX_BYTES:
            return line
        for k in keys:
            msg.pop(k, None)
        line = json.dumps(msg)
    while len(line.encode()) > MAX_BYTES and msg["model"]:
        msg["model"] = msg["model"][:-1]
        line = json.dumps(msg)
    return line


def read_cmd(raw):
    # คำสั่งจาก ai/cmd -> (คำสั่ง, ชื่อโมเดล) หรือ None = ไม่สนใจ (ยาวเกิน ไม่ใช่ JSON object ไม่รู้จัก)
    if len(raw) > CMD_MAX:
        return None
    try:
        body = json.loads(raw.decode())
    except Exception:
        return None
    if not isinstance(body, dict):
        return None
    act = body.get("cmd")
    if act in ("ack", "stop", "run"):
        return act, None
    if act == "select" and isinstance(body.get("model"), str):
        return act, body["model"]
    return None


class Ai:
    # ฝั่ง AI: หาโมเดลด้วยชื่อ เลือก อ่านเฉพาะผลใหม่ แล้วนับ CONFIRM_N ใน WINDOW_N
    def __init__(self, keys, hits):
        self.keys, self.hits, self.found = keys, hits, []
        self.idx, self.name, self.labels, self.mic, self.running = -1, RULE_ONLY, (), False, False
        self.clear(time.ticks_ms())

    def clear(self, now):
        self.seq, self.hist, self.on, self.stalled = None, [], False, False
        self.rain_hist, self.rain_on = [], False
        self.label, self.conf, self.p2, self.c2, self.ms = "-", 0, "-", 0, 0
        self.hit, self.hit_conf, self.last_new = "-", 0, now

    def start(self):
        # ลองตามลำดับ: โมเดลจาก Store ก่อน ไม่มีหรือเลือกไม่สำเร็จค่อยใช้ตัวสำรองในตัว
        self.found = find_models(self.keys)
        for idx, name, labels in self.found:
            if self.select(idx, name, labels):
                return True
        return False

    def select(self, idx, name, labels):
        try:
            edge_ai.select(idx)             # โหลดและเริ่มโมเดล (บนบอร์ดอาจนานถึง 15 วินาที)
        except OSError as e:
            print("select:", name, e)
            return False
        self.idx, self.name, self.labels = idx, name, labels
        self.mic, self.running = uses_mic(idx), True
        self.clear(time.ticks_ms())
        return True

    def stop(self):
        if self.running:
            stop_ai()
        self.running = False
        self.clear(time.ticks_ms())

    def resume(self):
        return self.idx >= 0 and self.select(self.idx, self.name, self.labels)

    def poll(self, now, quiet):
        if not self.running:
            return
        r = read_result()
        if not r or r["seq"] == self.seq or r.get("index", self.idx) != self.idx:
            if not self.stalled and time.ticks_diff(now, self.last_new) > STALL_MS:
                self.stalled = True         # ผลเก่าไม่ใช้แล้ว: ป้าย "-" = ไม่มีผล
                self.hist, self.on, self.label, self.conf = [], False, "-", 0
                self.p2, self.c2, self.ms = "-", 0, 0
            return
        self.seq, self.last_new, self.stalled = r["seq"], now, False
        if self.mic and quiet:
            return                          # เสียงจากลำโพงบอร์ดเองเข้าไมค์: ไม่เชื่อผลช่วงนี้
        self.label, self.conf = r["label"] or "-", int(r["conf"] * 100)
        self.p2, self.c2 = second_best(r, self.labels)
        self.ms = ceil_ms(r.get("latency_ms") or 0)
        hit = self.label in self.hits and self.conf >= CONF_MIN
        if hit:
            self.hit, self.hit_conf = self.label, self.conf
        self.hist, self.on = alert_rule(self.hist, self.on, hit)
        if MISSION == "night":
            rain = self.label == RAIN and self.conf >= CONF_MIN
            self.rain_hist, self.rain_on = alert_rule(self.rain_hist, self.rain_on, rain)


class Vib:
    # ค่าสั่นแบบ sf3_03: ส่วนเบี่ยงเบนมาตรฐานของขนาดความเร่ง WIN ตัวอย่างล่าสุด
    # LEARN_MS แรก = เรียนค่าปกติ (เฉลี่ยค่าสั่นตอนวางนิ่ง ไม่ต่ำกว่า MIN_BASE)
    def __init__(self, now):
        self.buf, self.vib = [], 0.0
        self.base, self.total, self.n, self.t_learn = None, 0.0, 0, now

    def push(self, m, now):
        # True เมื่อครบ WIN ตัว และได้ self.vib ใหม่
        self.buf.append(m)
        if len(self.buf) > WIN:
            self.buf.pop(0)
        if len(self.buf) < WIN:
            return False
        self.vib = spread(self.buf)
        if self.base is None:
            self.total += self.vib
            self.n += 1
            if time.ticks_diff(now, self.t_learn) >= LEARN_MS:
                self.base = max(MIN_BASE, self.total / max(1, self.n))
        return True


class Rules:
    # ฝั่งกฎ: อ่านเซนเซอร์ของภารกิจ แล้วคืน (rule 0/1, rk, rv ค่า, rl เกณฑ์)
    # อ่านไม่ได้ (เช่น โมเดลจองไมค์หรือ IMU อยู่) = rule 0 แล้วนับไว้ให้เห็นบนจอและใน Console
    def __init__(self, rk):
        self.rk, self.fails, self.bad_run, self.lv = rk, 0, 0, 0
        self.mic_on = rk == "loud" and start_mic()
        if rk == "loud" and not self.mic_on:
            self.failed("mic.start")
        self.reset(time.ticks_ms())

    def reset(self, now):
        self.vib = Vib(now)                 # pump: เรียนค่าปกติใหม่ทุกครั้งที่เริ่มหรือทำต่อ
        self.loud_t = self.last_loud = None
        self.coughs, self.tap_t, self.still_t = [], None, now

    def failed(self, what):
        self.fails += 1
        self.bad_run += 1
        if self.fails == 1 or self.fails % 100 == 0:
            print("กฎอ่านค่าไม่ได้:", what, "รวม", self.fails, "ครั้ง")

    def broken(self):
        return self.bad_run >= WIN or (self.rk == "loud" and not self.mic_on)

    def learning(self, now):
        # pump: เหลืออีกกี่ ms จะเรียนค่าปกติเสร็จ (0 = เสร็จแล้ว)
        if self.rk != "vib" or self.vib.base is not None:
            return 0
        return max(0, LEARN_MS - time.ticks_diff(now, self.vib.t_learn))

    def read(self, now, quiet):
        # อ่านเซนเซอร์หนึ่งครั้งต่อรอบ · quiet = บอร์ดเพิ่งส่งเสียงเอง: ไม่ฟังไมค์ช่วงนี้
        if self.rk in ("vib", "dur"):
            m = magnitude()
            if m is None:
                self.failed("IMU")
                return
            self.bad_run = 0
            if self.vib.push(m, now) and self.rk == "dur" and self.vib.vib > MOVE_VIB:
                self.still_t = now          # worker ขยับ: เริ่มนับนิ่งใหม่
        elif self.rk == "loud" and self.mic_on and not quiet:
            try:
                self.lv = mic.level()
            except Exception:
                self.failed("mic")
                return
            self.bad_run = 0
            self.track_loud(self.lv >= LOUD_LV, now)

    def track_loud(self, loud, now):
        # ดังต่อเนื่อง: เงียบสั้นกว่า QUIET_GAP_MS ยังนับต่อ
        if loud:
            if self.loud_t is None:
                self.loud_t = now
            self.last_loud = now
        elif self.loud_t is not None and time.ticks_diff(now, self.last_loud) > QUIET_GAP_MS:
            self.loud_t = None

    def judge(self, now, ai, edge):
        rk = self.rk
        if self.broken():
            return 0, rk, 0, {"loud": LOUD_S.get(MISSION, 2), "dur": STILL_S, "rate": COUGH_MAX}.get(rk, 0)
        if rk == "vib":
            if self.vib.base is None:
                return 0, rk, round(self.vib.vib, 2), 0
            rl = self.vib.base * K_VIB
            return int(self.vib.vib > rl), rk, round(self.vib.vib, 2), round(rl, 2)
        if rk == "loud":
            secs = 0 if self.loud_t is None else time.ticks_diff(now, self.loud_t) / 1000
            lim = LOUD_S.get(MISSION, 2)
            return int(secs >= lim), rk, round(secs, 1), lim
        if rk == "dur":
            secs = time.ticks_diff(now, self.still_t) / 1000
            return int(secs >= STILL_S), rk, round(secs, 1), STILL_S
        if rk == "rate":
            return self.barn(now, ai, edge)
        return 0, rk, 0, 1

    def barn(self, now, ai, edge):
        # rate: นับครั้งที่ AI ยืนยันเสียงไอ (ขอบขาขึ้นของ 3 ใน 5) ภายใน RATE_MS
        # dur : AI ได้ยิน water_tap ต่อเนื่องกี่วินาที · ก๊อกเปิดอยู่ จอและข้อความโชว์กฎ dur แทน
        if edge and ai.hit == "cough":
            self.coughs.append(now)
        while self.coughs and time.ticks_diff(now, self.coughs[0]) > RATE_MS:
            self.coughs.pop(0)
        if ai.on and ai.hit == "water_tap":
            if self.tap_t is None:
                self.tap_t = now
        else:
            self.tap_t = None
        busy = len(self.coughs) >= COUGH_MAX
        if self.tap_t is not None:
            secs = time.ticks_diff(now, self.tap_t) / 1000
            return int(secs >= TAP_S or busy), "dur", round(secs, 1), TAP_S
        return int(busy), "rate", len(self.coughs), COUGH_MAX


class Alarm:
    # เตือนเมื่อ want (ปกติ = ระดับ 2 · worker = ถามแล้วไม่ตอบ) · หายเองเมื่อ gone (by clear) · คนรับทราบ (by ack)
    # รับทราบแล้ว จะเตือนใหม่ได้ก็ต่อเมื่อเหตุสงบ (ไม่ want) ต่อเนื่อง ACK_REARM_MS
    # กันเตือนซ้ำรัว ๆ ตอนป้ายสลับไปมา (คนเพิ่งรับทราบ ไม่ควรโดนเรียกซ้ำทุกไม่กี่วินาที)
    def __init__(self):
        self.on, self.acked, self.count, self.calm_t = False, False, 0, None

    def update(self, want, gone, now):
        if self.acked:
            if want:
                self.calm_t = None          # ยังไม่สงบ: เริ่มนับใหม่
            elif self.calm_t is None:
                self.calm_t = now
            elif time.ticks_diff(now, self.calm_t) >= ACK_REARM_MS:
                self.acked = False          # สงบนานพอ: เตือนใหม่ได้
        if not self.on and want and not self.acked:
            self.on, self.count = True, self.count + 1
            return "start"
        if self.on and gone:
            self.on = False
            return "clear"
        return None

    def ack(self):
        if not self.on:
            return False
        self.on, self.acked, self.calm_t = False, True, None
        return True


class VoiceGate:
    # ภารกิจ voice: บอร์ดเป็น Gateway สั่ง PLC (สัญญาข้อ 4.2)
    # stop = ทำทันทีเสมอ ไม่ต้องยืนยัน ไม่รอช่วงห่าง (ปิดปั๊มปลอดภัยเสมอ) · stop ซ้ำในช่วงห่างไม่ส่งซ้ำ
    # go = ต้องยืนยันภายใน GO_CONFIRM_MS (ได้ยิน go ซ้ำ หรือกด SW6) และห่าง plc/cmd ก่อนหน้า >= PLC_GAP_MS
    # คืน (คำสั่ง, ok, why, ข้อความถึง PLC หรือ None) · ok None = ได้ยิน go แล้ว รอยืนยัน
    # t_ok = เวลาที่ยืนยัน go ล่าสุด (กฎ btn) · t_ai = เวลาที่ AI ได้ยินคำล่าสุด (ปุ่มแทนเสียงไม่นับเป็น AI)
    def __init__(self):
        self.wait = self.t_plc = self.last = self.t_ok = self.t_ai = None

    def heard(self, word, now, by_ai=False):
        if by_ai:
            self.t_ai = now
        if word == "stop":
            self.wait = None
            return self.stop(now)
        if self.wait is not None and time.ticks_diff(now, self.wait) <= GO_CONFIRM_MS:
            self.wait, self.t_ok = None, now
            return self.go(now)             # ได้ยิน go ซ้ำทันเวลา = ยืนยัน
        self.wait = now
        return "go", None, "wait", None

    def confirm(self, now):
        # SW6 ยืนยัน go ที่รออยู่ · ไม่มี go รอ คืน None
        if self.wait is None:
            return None
        self.wait, self.t_ok = None, now
        return self.go(now)

    def expire(self, now):
        if self.wait is not None and time.ticks_diff(now, self.wait) > GO_CONFIRM_MS:
            self.wait = None
            return "go", 0, "unconfirmed", None
        return None

    def go(self, now):
        if self.t_plc is not None and time.ticks_diff(now, self.t_plc) < PLC_GAP_MS:
            return "go", 0, "gap", None
        self.t_plc, self.last = now, "go"
        return "go", 1, "", {"pump": 1, "sec": min(PUMP_SEC, PUMP_MAX_S)}

    def stop(self, now):
        if self.last == "stop" and time.ticks_diff(now, self.t_plc) < PLC_GAP_MS:
            return "stop", 1, "", None      # เพิ่งสั่งปิดไป ไม่ส่งซ้ำให้กล่องรับของ PLC
        self.t_plc, self.last = now, "stop"
        return "stop", 1, "", {"pump": 0}

    def recent(self, t, now):
        # เกิดภายใน GO_CONFIRM_MS ที่ผ่านมาไหม (1/0) · กฎ btn = recent(t_ok) · ฝั่ง AI = recent(t_ai)
        return int(t is not None and time.ticks_diff(now, t) < GO_CONFIRM_MS)


class CheckIn:
    # worker: เวลาที่เริ่มถาม (None = ไม่ได้ถาม) และถามแล้วไม่มีคนตอบหรือยัง
    def __init__(self):
        self.asked, self.failed = None, False


class State:
    # ทุกอย่างที่โปรแกรมจำ (ไม่แตะฮาร์ดแวร์ ไม่แตะจอ)
    def __init__(self, now, alarm_text):
        self.alarm_text, self.paused, self.ai_off, self.online = alarm_text, False, False, False
        self.n, self.sent_key, self.force = 0, None, True
        self.t_sent = time.ticks_add(now, -HEARTBEAT_MS)
        self.t_cmd = time.ticks_add(now, -CMD_GAP_MS)
        self.quiet_at = self.t_beep = now
        self.rule, self.rk, self.rv, self.rl, self.agree, self.lvl = 0, "", 0, 0, "none", 0
        self.alarm, self.gate, self.checkin = Alarm(), VoiceGate(), CheckIn()
        self.holes, self.voice_txt, self.voice_say = {}, " ", None     # drill: {"wood": รู, "plastic": รู}
        self.mx, self.mx_t = None, now


# ---- 4) MQTT ----
def connect_broker(w):
    # broker สาธารณะบางเครื่องไม่ตอบเป็นพัก ๆ: ลอง 3 ครั้ง ใช้ client_id ใหม่ทุกครั้ง (เหมือน sf3_06)
    for n in (1, 2, 3):
        if n > 1:
            note(w, "ลองต่อ broker ใหม่ %d/3" % n, COL_WARN)
            ui.poll()
        try:
            if mqtt.connect(BROKER, port=1883, keepalive=60,
                            client_id=CLIENT_ID + "-%04x" % (time.ticks_ms() & 0xFFFF)):
                return True
        except OSError:
            pass
    return False


def go_online(w):
    # WiFi -> broker -> subscribe ai/cmd · ขั้นไหนพังคืน False แล้วทำงานต่อแบบออฟไลน์ (พิมพ์ลง Console)
    # TEAM ยังเป็น teamXX = ส่งทับหัวข้อของกลุ่มอื่น จึงไม่ต่อเลย
    if not (len(TEAM) == 6 and TEAM[:4] == "team" and TEAM[4:].isdigit() and TEAM != "team00"):
        note(w, "แก้ TEAM ก่อน (team01-team99): ออฟไลน์", COL_WARN)
        return False
    if WIFI_SSID[:1] in ("<", "") and not wifi.is_connected():   # ห้ามส่งชื่อตัวอย่างให้ wifi.connect
        note(w, "ตั้งชื่อ Hotspot ก่อน: ออฟไลน์", COL_WARN)
        return False
    note(w, "ต่อ WiFi...", COL_WARN)
    ui.poll()                          # ป้ายต้องขึ้นจอก่อนบรรทัดที่บล็อก
    step = "WiFi"
    try:
        if (wifi.is_connected() or wifi.connect(WIFI_SSID, WIFI_PASS)) and wifi.ip() != "0.0.0.0":
            step = "broker"
            if connect_broker(w):
                ok = mqtt.subscribe(BASE + "ai/cmd")
                note(w, ("ออนไลน์ " if ok else "ส่งได้แต่รับคำสั่งไม่ได้ ") + BASE + "ai", COL_OK if ok else COL_WARN)
                return True
    except OSError:
        pass
    note(w, "ต่อ " + step + " ไม่ได้: ออฟไลน์ (ทำงานต่อ)", COL_WARN)
    return False


def send(s, topic, obj):
    # ส่ง JSON (QoS 0) ไป BASE + topic · คืน True ถ้าขึ้น broker ได้ · ออฟไลน์ก็พิมพ์ลง Console
    line = fit(obj) if topic == "ai" else json.dumps(obj)
    print(line)
    if s.online:
        try:
            return bool(mqtt.publish(BASE + topic, line))
        except OSError:
            pass
    return False


def model_name(s, ai):
    # "" = หยุดอยู่ (สัญญาข้อ 3.10) · "rule-only" = บอร์ดไม่มีโมเดล ใช้กฎอย่างเดียว · ไม่ส่งรหัสโมเดล
    if s.paused or (ai.idx >= 0 and not ai.running):
        return ""
    return ai.name


def report(w, s, ai):
    # สถานะตามสัญญาข้อ 3.11: ทุก HEARTBEAT_MS และทันทีที่ label / lvl / alarm เปลี่ยน ห่างกัน >= GAP_MS
    # ใช้เวลาตอนส่งจริง ไม่ใช่เวลาต้นรอบ: เสียงในรอบเดียวกันกินเวลาไปได้ถึงครึ่งวินาที
    now = time.ticks_ms()
    key = (ai.label, s.lvl, s.alarm.on)
    if not should_send(s.force or key != s.sent_key, time.ticks_diff(now, s.t_sent)):
        return
    s.n += 1
    ok = send(s, "ai", {"id": TEAM, "n": s.n, "m": MISSION, "model": model_name(s, ai),
                        "label": ai.label, "conf": ai.conf, "p2": ai.p2, "c2": ai.c2, "ms": ai.ms,
                        "rk": s.rk, "rule": s.rule, "rv": s.rv, "rl": s.rl,
                        "agree": s.agree, "lvl": s.lvl, "alarm": 1 if s.alarm.on else 0})
    s.t_sent, s.sent_key, s.force = now, key, False
    put(w, "mq_t", ("MQTT: ส่งแล้ว %d" if ok else "ออฟไลน์: Console %d") % s.n)


def event(s, name, **keys):
    # เหตุการณ์ไปหัวข้อ event (ไม่มี n เหมือน event อื่น · ส่งครั้งเดียวตอนเกิด)
    obj = {"id": TEAM, "event": name, "m": MISSION}
    obj.update(keys)
    send(s, "event", obj)


# ---- 5) จอ ----
def label(text, x, y, col=COL_DIM, size=16):
    return ui.Label(text, x=x, y=y, color=col, value=size)


def card(x, y, w, h, title):
    ui.Panel(x=x, y=y, w=w, h=h, color=COL_CARD, min=COL_DIM, max=12, value=1)
    ui.Label(title, x=x + 12, y=y + 6, color=COL_INFO, value=16)


def put(w, key, text, col=None):
    # เขียนป้ายเฉพาะเมื่อข้อความหรือสีเปลี่ยน (จอบนบอร์ดทำงานน้อยลง)
    if w["_t"].get(key) != text:
        w["_t"][key] = text
        w[key].text(text)
    if col is not None and w["_c"].get(key) != col:
        w["_c"][key] = col
        w[key].color(col)


def setv(w, key, v, col=None):
    # ค่าของ Arc / Bar / Led เฉพาะเมื่อเปลี่ยน
    if w["_v"].get(key) != v:
        w["_v"][key] = v
        w[key].value(v)
    if col is not None and w["_c"].get(key) != col:
        w["_c"][key] = col
        w[key].color(col)


def note(w, text, col):
    put(w, "note", text, col)


def hint_text():
    if MISSION == "voice":
        return "SW5 = แทนเสียง go · SW6 = ยืนยัน go / stop"
    if MISSION == "worker":
        return "SW6 = ฉันโอเค / รับทราบ · SW5 = พัก"
    return "SW6 = รับทราบการเตือน · SW5 = พัก/ทำต่อ"


def rule_text(rk):
    # กฎที่อ่านออก ภาษาคน
    if rk == "vib":
        return "กฎ: ค่าสั่น > ค่าปกติ x %g" % K_VIB
    if rk == "loud":
        return "กฎ: ดัง >= %d ต่อเนื่อง %g วิ" % (LOUD_LV, LOUD_S.get(MISSION, 2))
    if rk == "rate":
        return "กฎ: ไอ >= %d ครั้งใน %d นาที" % (COUGH_MAX, RATE_MS // 60000)
    if rk == "dur" and MISSION == "barn":
        return "กฎ: ก๊อกเปิดต่อเนื่อง >= %d วิ" % TAP_S
    if rk == "dur":
        return "กฎ: ไม่ขยับ >= %d วิ แล้วถาม" % STILL_S
    return "กฎ: SW6 ยืนยัน go ภายใน %d วิ" % (GO_CONFIRM_MS // 1000)


def value_text(s):
    # ค่าของกฎเทียบเกณฑ์ พร้อมหน่วย
    if s.rk == "vib":
        return "%.2f / %s m/s^2" % (s.rv, ("%.2f" % s.rl) if s.rl else "-")
    if s.rk in ("loud", "dur"):
        return "%.1f / %g วิ" % (s.rv, s.rl)
    if s.rk == "rate":
        return "%d / %d ครั้ง" % (s.rv, s.rl)
    return "ยืนยัน %d / 1" % s.rv


def build_screen(thai, problem):
    ui.screen()
    time.sleep_ms(200)
    w = {"_t": {}, "_c": {}, "_v": {}}
    label("ภารกิจ AI: " + thai, 12, 6, COL_TEXT, 24)
    label(MISSION + " · " + problem, 12, 38, COL_DIM, 16)
    w["mq"] = ui.Led(x=606, y=12, w=18, h=18, color=COL_OK, value=0)
    w["mq_t"] = label("MQTT: ออฟไลน์", 632, 10, COL_DIM, 16)
    card(12, 64, 380, 172, "AI ตอบว่า")
    w["model"] = label("กำลังหาโมเดล...", 24, 88, COL_DIM, 14)
    w["arc"] = ui.Arc(x=300, y=92, w=80, h=80, min=0, max=100, value=0)
    w["label"] = label("-", 24, 108, COL_TEXT, 28)
    w["meaning"] = label(" ", 24, 146, COL_INFO, 16)
    w["conf"] = label(" ", 24, 176, COL_DIM, 14)
    w["ai"] = label(" ", 24, 204, COL_DIM, 16)
    card(402, 64, 378, 172, "กฎที่เขียนเอง")
    w["rdesc"] = label(" ", 414, 88, COL_DIM, 14)
    w["rv"] = label("-", 414, 108, COL_TEXT, 28)
    w["rbar"] = ui.Bar(x=414, y=148, w=354, h=14, min=0, max=100, value=0)
    w["rstate"] = label(" ", 414, 174, COL_DIM, 16)
    w["rnote"] = label(" ", 414, 204, COL_DIM, 14)
    card(12, 244, 380, 100, "AI กับกฎ เห็นตรงกันไหม")
    label("กฎ: ใช่", 130, 266, COL_DIM, 14)
    label("กฎ: ไม่", 270, 266, COL_DIM, 14)
    label("AI: ใช่", 24, 290, COL_DIM, 14)
    label("AI: ไม่", 24, 316, COL_DIM, 14)
    for name, x, y, col in (("both", 130, 288, COL_BAD), ("ai", 270, 288, COL_WARN),
                            ("rule", 130, 314, COL_WARN), ("none", 270, 314, COL_OK)):
        w["c_" + name] = ui.Led(x=x, y=y, w=18, h=18, color=col, value=0)
        label(name, x + 26, y, COL_TEXT, 16)
    card(402, 244, 378, 100, "ระดับและการเตือน")
    w["lvl_led"] = ui.Led(x=414, y=274, w=34, h=34, color=COL_OK, value=1)
    w["lvl"] = label(" ", 460, 270, COL_TEXT, 20)
    w["alarm"] = label(" ", 460, 298, COL_DIM, 16)
    label(hint_text(), 414, 322, COL_DIM, 14)
    w["note"] = label("กำลังเริ่ม...", 12, 352, COL_WARN, 16)
    ui.poll()
    return w


def show_link(w, online):
    # ไฟ MQTT: ติดเขียว = ต่อ broker อยู่ ข้อความขึ้นแอปได้ · ดับ = ออฟไลน์ ข้อความพิมพ์ลง Console แทน
    setv(w, "mq", 1 if online else 0)
    put(w, "mq_t", "MQTT: เชื่อมต่อแล้ว" if online else "MQTT: ออฟไลน์", COL_OK if online else COL_BAD)


def show_model(w, ai):
    if ai.idx < 0:
        put(w, "model", "โมเดล: " + RULE_ONLY + " (บอร์ดไม่มีโมเดลของภารกิจ)", COL_WARN)
    else:
        put(w, "model", "โมเดล: " + ai.name + ("  (ฟังไมค์)" if ai.mic else ""), COL_DIM)


def ai_status(s, ai):
    if s.paused:
        return "พักอยู่: กด SW5 ทำต่อ", COL_WARN
    if ai.idx < 0:
        return NO_MODEL.get(MISSION, "ไม่มีโมเดล: ใช้กฎอย่างเดียว"), COL_WARN
    if not ai.running:
        return "หยุดโมเดลอยู่ (แอปส่ง run)", COL_WARN
    if ai.stalled:
        return "AI เงียบ / ไม่มีผล", COL_BAD
    if ai.seq is None:
        return "รอผลแรกจาก AI...", COL_DIM
    text = "AI: %s  (ป้ายเหตุ %d/%d)" % ("เกิดเหตุ" if ai.on else "ปกติ", sum(ai.hist), len(ai.hist))
    return text, COL_BAD if ai.on else COL_OK


def rule_state(s, rules, now):
    if s.paused:
        return "พัก", COL_WARN
    if MISSION != "voice" and rules.broken():
        return "กฎ: อ่านค่าไม่ได้ (rule = 0)", COL_BAD
    left = rules.learning(now)
    if left:
        return "วางนิ่ง เรียนค่าปกติ อีก %d วิ" % ((left + 999) // 1000), COL_WARN
    if MISSION == "voice" and s.gate.wait is not None:
        left = GO_CONFIRM_MS - time.ticks_diff(now, s.gate.wait)
        return "ได้ยิน go: รอ SW6 / go ซ้ำ %d วิ" % ((max(0, left) + 999) // 1000), COL_WARN
    return ("กฎ: เกิดเหตุ" if s.rule else "กฎ: ปกติ"), COL_BAD if s.rule else COL_OK


def rule_note(rules):
    if MISSION == "voice":
        return " ", COL_DIM
    if rules.broken():
        return "กฎอ่านค่าไม่ได้ระหว่างโมเดลรัน (%d)" % rules.fails, COL_BAD
    if rules.fails:
        return "อ่านพลาด %d ครั้ง" % rules.fails, COL_WARN
    if rules.rk == "loud":
        return "ไมค์ %d (ดัง >= %d)" % (rules.lv, LOUD_LV), COL_DIM
    if rules.rk == "vib" and rules.vib.base is not None:
        return "ค่าปกติที่เรียนมา %.3f" % rules.vib.base, COL_DIM
    return " ", COL_DIM


def alarm_line(s, now):
    if MISSION == "voice":
        return s.voice_txt, COL_INFO
    if s.checkin.asked is not None:
        left = CHECKIN_S * 1000 - time.ticks_diff(now, s.checkin.asked)
        return "โอเคไหม? กด SW6 (อีก %d วิ)" % ((max(0, left) + 999) // 1000), COL_WARN
    if s.alarm.on:
        return "เตือนอยู่! กด SW6 รับทราบ", COL_BAD
    if s.alarm.acked:
        return "รับทราบแล้ว (เตือนใหม่หลังสงบ %d วิ)" % (ACK_REARM_MS // 1000), COL_WARN
    return "ไม่มีการเตือน (เตือนแล้ว %d)" % s.alarm.count, COL_DIM


def matrix(s, ai, now):
    # จอไฟ RGB: เตือน = ข้อความแดง · ถาม = เหลือง · ระดับ 1 = LOOK เหลือง · ปกติ = แถบความมั่นใจของ AI
    if s.alarm.on:
        want = ("say", s.alarm_text, rgbmatrix.RED)
    elif s.checkin.asked is not None:
        want = ("say", CHECK_TEXT, rgbmatrix.YELLOW)
    elif s.voice_say and time.ticks_diff(now, s.voice_say[2]) < VOICE_SHOW_MS:
        want = ("say", s.voice_say[0], s.voice_say[1])
    elif s.lvl >= 1 and MISSION != "voice":
        want = ("say", LOOK_TEXT, rgbmatrix.YELLOW)
    else:
        want = ("bar", min(8, ai.conf * 8 // 100), rgbmatrix.GREEN)
    if want == s.mx and (want[0] == "say" or time.ticks_diff(now, s.mx_t) < MATRIX_MS):
        return                               # ตัววิ่งวิ่งเองบนบอร์ด ส่งครั้งเดียวพอ
    if want[0] == "say":
        mx(rgbmatrix.scroll, ascii_only(want[1]), want[2], SCROLL_MS)
    else:
        if s.mx is None or s.mx[0] == "say":
            mx(rgbmatrix.clear)
        mx(rgbmatrix.bar, want[1], 8, want[2])
    s.mx, s.mx_t = want, now


def draw(w, s, ai, rules, now):
    put(w, "label", ai.label)
    put(w, "meaning", MEANING.get(ai.label, ai.label), COL_BAD if ai.on else COL_INFO)
    put(w, "conf", "มั่นใจ %d %% · อันดับสอง %s %d %%" % (ai.conf, ai.p2, ai.c2) if ai.running else " ")
    setv(w, "arc", ai.conf, COL_OK if ai.conf >= CONF_MIN else COL_WARN)
    put(w, "ai", *ai_status(s, ai))
    put(w, "rdesc", rule_text(s.rk))
    put(w, "rv", value_text(s), COL_BAD if s.rule else COL_TEXT)
    setv(w, "rbar", min(100, int(s.rv * 100 / s.rl)) if s.rl else 0, COL_BAD if s.rule else COL_INFO)
    put(w, "rstate", *rule_state(s, rules, now))
    put(w, "rnote", *rule_note(rules))
    for name in ("both", "ai", "rule", "none"):
        setv(w, "c_" + name, 1 if s.agree == name else 0)
    setv(w, "lvl_led", 1, LVL_COL[s.lvl])
    put(w, "lvl", LVL_TEXT[s.lvl], LVL_COL[s.lvl])
    put(w, "alarm", *alarm_line(s, now))
    matrix(s, ai, now)
    ui.poll()


# ---- 6) โปรแกรมหลัก ----
def sound(s, *notes):
    # ทุกครั้งที่ลำโพงดัง = ปิดหู MUTE_MS (เสียงของบอร์ดเองไม่ใช่เหตุในฟาร์ม)
    beep(*notes)
    s.quiet_at = time.ticks_add(time.ticks_ms(), MUTE_MS)


def end_alarm(s, led, by):
    event(s, "ai_alarm_end", by=by)
    sound(s, 79, 84)
    set_led(led, False)
    s.force = True


def ack(w, s, rules, led, now):
    # รับทราบการเตือน (SW6 หรือ {"cmd":"ack"} จากแอป)
    if not s.alarm.ack():
        note(w, "ไม่มีการเตือนให้รับทราบ", COL_DIM)
        return
    end_alarm(s, led, "ack")
    s.checkin.failed, rules.still_t = False, now        # worker: เริ่มนับนิ่งใหม่
    note(w, "รับทราบแล้ว", COL_OK)


def alarm_step(w, s, ai, led, now):
    # ปกติ: เตือนเมื่อระดับ 2 หายเมื่อระดับ 0 · worker: เตือนเมื่อถามแล้วไม่ตอบ (alarm ไม่ขึ้นกับระดับ)
    if MISSION == "worker":
        act = s.alarm.update(s.checkin.failed, not s.checkin.failed, now)
    else:
        act = s.alarm.update(s.lvl == 2, s.lvl == 0, now)
    if act == "start":
        lab, conf = (ai.hit, ai.hit_conf) if ai.on else ("-", 0)
        event(s, "ai_alarm_start", model=model_name(s, ai), label=lab, conf=conf, lvl=s.lvl)
        sound(s, 84, 76)
        set_led(led, True)
        s.t_beep, s.force = now, True
        note(w, "เตือน! " + s.agree + " - กด SW6 รับทราบ", COL_BAD)
        print("เตือน:", MISSION, s.agree, lab, conf)
    elif act == "clear":
        end_alarm(s, led, "clear")
        note(w, "เหตุหายไปเอง", COL_OK)
    elif s.alarm.on and ALARM_REPEAT_MS > 0 and time.ticks_diff(now, s.t_beep) >= ALARM_REPEAT_MS:
        sound(s, 84, 76)                    # ยังไม่มีใครรับทราบ: ดังซ้ำ
        s.t_beep = now


def voice_do(w, s, res, now):
    # ผลของ VoiceGate -> event voice_cmd + plc/cmd + เสียง + จอ
    if res is None:
        return
    cmd, ok, why, plc = res
    if ok is None:
        note(w, "ได้ยิน go: กด SW6 หรือ go ซ้ำ ภายใน %d วิ" % (GO_CONFIRM_MS // 1000), COL_WARN)
        return
    event(s, "voice_cmd", cmd=cmd, ok=ok, why=why)
    if plc is not None:
        send(s, "plc/cmd", plc)
    if ok and cmd == "go":
        s.voice_txt, s.voice_say = "go: เปิดปั๊ม %d วิ" % min(PUMP_SEC, PUMP_MAX_S), (VOICE_SAY["go"], rgbmatrix.GREEN, now)
        sound(s, 72, 79)
    elif ok:
        s.voice_txt, s.voice_say = "stop: ปิดปั๊มทันที", (VOICE_SAY["stop"], rgbmatrix.RED, now)
        sound(s, 79, 72)
    else:
        s.voice_txt = "go ไม่ทำ: " + ("ยังไม่ยืนยัน" if why == "unconfirmed" else "สั่งถี่เกิน %d วิ" % (PLC_GAP_MS // 1000))
        s.voice_say = (VOICE_SAY["no"], rgbmatrix.YELLOW, now)
        sound(s, 69)
    note(w, "voice_cmd %s ok %d %s" % (cmd, ok, why), COL_OK if ok else COL_WARN)


def worker_step(w, s, rules, now):
    # ไม่ขยับครบ STILL_S -> ถาม "โอเคไหม" (SW6) · ไม่ตอบใน CHECKIN_S -> check_in ok 0 แล้วเตือน
    # ระดับยังมาจากตาราง 2x2 ตามปกติ (AI + กฎว่านิ่งนาน = ระดับ 2) · การเตือนมาจากการไม่ตอบเท่านั้น
    # จึงเตือนได้แม้ไม่มีโมเดล (ระดับ 1 + alarm 1)
    ck = s.checkin
    if s.paused or not s.rule:
        ck.asked, ck.failed = None, False   # ขยับแล้ว (หรือพักอยู่): เลิกถาม
    elif ck.asked is None and not ck.failed:
        ck.asked = now
        sound(s, 72, 79)
        note(w, "นิ่งนาน %d วิ: โอเคไหม? กด SW6" % STILL_S, COL_WARN)
    elif ck.asked is not None and time.ticks_diff(now, ck.asked) >= CHECKIN_S * 1000:
        ck.asked, ck.failed = None, True
        event(s, "check_in", ok=0)


def drill_step(w, s, ai, edge):
    # เจาะเสร็จ 1 รู = AI ยืนยันว่าทะลุ (wood_out / plastic_out) -> drill_done นับรูแยกตามวัสดุ
    # ไม่มีโมเดล = ไม่รู้วัสดุ จึงไม่ส่ง drill_done (สัญญา: mat ต้องเป็น wood หรือ plastic) · กฎ loud ยังบอกว่าสว่านเดินอยู่
    if edge and ai.hit in ("wood_out", "plastic_out"):
        mat = ai.hit[:-4]
        s.holes[mat] = s.holes.get(mat, 0) + 1
        event(s, "drill_done", mat=mat, holes=s.holes[mat])
        note(w, "ทะลุ%s แล้ว %d รู" % ("ไม้" if mat == "wood" else "พลาสติก", s.holes[mat]), COL_WARN)


def judge(w, s, ai, rules, now, edge, was_rain):
    ai_on = ai.on
    if MISSION == "voice":
        if edge and ai.hit in ("go", "stop"):            # คำที่ AI ได้ยิน (3 ใน 5)
            voice_do(w, s, s.gate.heard(ai.hit, now, True), now)
        voice_do(w, s, s.gate.expire(now), now)
        ai_on = ai.on or (ai.running and s.gate.recent(s.gate.t_ai, now))   # AI ได้ยินคำภายใน 3 วิ
    if s.paused:
        s.rule, s.rk, s.rv, s.rl = 0, rules.rk, 0, 0
    elif MISSION == "voice":
        on = s.gate.recent(s.gate.t_ok, now)             # ยืนยันแล้ว: go ซ้ำ หรือ SW6
        s.rule, s.rk, s.rv, s.rl = on, "btn", on, 1
    else:
        s.rule, s.rk, s.rv, s.rl = rules.judge(now, ai, edge)
    s.agree, s.lvl = agreement(ai_on, s.rule)
    if MISSION == "night" and ai.rain_on and not was_rain:
        event(s, "rain", conf=ai.conf)
    elif MISSION == "worker":
        worker_step(w, s, rules, now)
    elif MISSION == "drill":
        drill_step(w, s, ai, edge)


def pause(w, s, ai, led):
    s.paused = True
    if s.alarm.ack():                       # คนกดปุ่มที่บอร์ด = รับทราบ
        end_alarm(s, led, "ack")
    ai.stop()
    s.checkin.asked, s.checkin.failed = None, False
    note(w, "พักอยู่: AI และกฎหยุด - กด SW5 ทำต่อ", COL_WARN)
    s.force = True


def resume(w, s, ai, rules):
    s.paused = False
    if not s.ai_off and ai.idx >= 0 and not ai.resume():
        note(w, "โหลดโมเดลไม่สำเร็จ: ใช้กฎอย่างเดียว", COL_BAD)
    else:
        note(w, "ทำต่อ", COL_OK)
    rules.reset(time.ticks_ms())
    s.force = True


def press(w, s, ai, rules, led, p6, p5, now):
    if MISSION == "voice":                  # ปุ่มแทนเสียง: SW5 = go · SW6 = ยืนยัน go ที่รอ / ไม่มีรอ = stop
        if p5:
            voice_do(w, s, s.gate.heard("go", now), now)
        if p6:
            voice_do(w, s, s.gate.confirm(now) or s.gate.heard("stop", now), now)
        return
    if p5:
        sound(s, 72)
        if s.paused:
            resume(w, s, ai, rules)
        else:
            pause(w, s, ai, led)
    if not p6:
        return
    if MISSION == "worker" and s.checkin.asked is not None:
        s.checkin.asked, rules.still_t = None, now
        event(s, "check_in", ok=1)
        sound(s, 72, 79, 84)
        note(w, "โอเค - เริ่มนับใหม่", COL_OK)
    elif MISSION == "worker" and not s.alarm.on:
        rules.still_t = now                 # กดเองก็แปลว่ายังโอเค
        note(w, "โอเค - เริ่มนับใหม่", COL_OK)
    else:
        ack(w, s, rules, led, now)


def inbox(w, s, ai, rules, led, now):
    # คำสั่งจากแอปทาง ai/cmd (สัญญาข้อ 3.10 + ack ข้อ 3.11) · อย่างอื่นไม่สนใจ
    if not s.online:
        return
    try:
        msg = mqtt.get_message()
    except OSError:
        return
    if not msg or msg[0] != BASE + "ai/cmd":
        return
    cmd = read_cmd(msg[1])
    if cmd is None:
        return
    act, name = cmd
    if act == "ack":
        ack(w, s, rules, led, now)
        return
    if ai.idx < 0:
        note(w, "ไม่มีโมเดล: ไม่ทำคำสั่ง " + act, COL_DIM)
        return
    if time.ticks_diff(now, s.t_cmd) < CMD_GAP_MS:
        note(w, "คำสั่งถี่เกิน %d วิ: ไม่ทำ" % (CMD_GAP_MS // 1000), COL_WARN)
        return
    s.t_cmd = now
    if act == "stop":
        s.ai_off = True
        ai.stop()
        note(w, "แอปสั่งหยุดโมเดล (กฎยังทำงาน)", COL_WARN)
    elif act == "run":
        s.ai_off = False
        if s.paused:
            resume(w, s, ai, rules)
        elif not ai.running and not ai.resume():
            note(w, "โหลดโมเดลไม่สำเร็จ", COL_BAD)
    else:
        pick = [f for f in ai.found if f[1] == name]
        if not pick:
            note(w, "ไม่มีโมเดลชื่อนี้ในภารกิจ: ไม่ทำ", COL_WARN)
            return
        old = (ai.idx, ai.name, ai.labels)
        ai.stop()
        if not ai.select(*pick[0]) and not ai.select(*old):
            note(w, "เปลี่ยนโมเดลไม่สำเร็จ", COL_BAD)
        s.ai_off, s.paused = False, False
        rules.reset(now)
        show_model(w, ai)
    s.force = True                          # ตอบรับ: ส่งสถานะทันที คีย์ model คือคำยืนยัน


def watch(w, s, ai, rules, led):
    b6, b5 = Button(SW6), Button(SW5)
    t0 = t_draw = time.ticks_ms()
    rules.reset(t0)
    while time.ticks_diff(time.ticks_ms(), t0) < RUN_MS:
        now = time.ticks_ms()
        quiet = time.ticks_diff(now, s.quiet_at) < 0
        was, was_rain = ai.on, ai.rain_on
        if not s.paused:                                  # 1) อ่าน: AI + เซนเซอร์ของกฎ
            ai.poll(now, quiet)
            rules.read(now, quiet)
        edge = ai.on and not was                          # AI เพิ่งยืนยันเหตุ (ขอบขาขึ้น)
        judge(w, s, ai, rules, now, edge, was_rain)       # 2) ตัดสิน: กฎ + AI -> agree / lvl
        if MISSION != "voice":                            # 3) ทำ: voice ไม่มีการเตือน มีแต่คำสั่ง
            alarm_step(w, s, ai, led, now)
        press(w, s, ai, rules, led, b6.pressed_now(), b5.pressed_now(), now)
        inbox(w, s, ai, rules, led, now)
        report(w, s, ai)                                  # 4) ส่ง
        if s.online and not mqtt.is_connected():
            s.online = False
            show_link(w, False)
            note(w, "เน็ตหลุด ทำงานต่อแบบออฟไลน์", COL_WARN)
        if time.ticks_diff(now, t_draw) >= DRAW_MS:       # 5) โชว์
            t_draw = now
            draw(w, s, ai, rules, now)
        wait_ms(SAMPLE_MS, (b6, b5))


def main():
    if hasattr(ui, "volume"):    # บอร์ดที่ยังเป็น 2.4.1 ข้ามบรรทัดนี้
        ui.volume(SPEAKER)
    spec = MISSIONS.get(MISSION)
    if spec is None:
        ui.screen()
        time.sleep_ms(200)
        label("ไม่รู้จัก MISSION นี้: " + ascii_only(MISSION), 12, 120, COL_BAD, 24)
        label("เลือก: " + " ".join(sorted(MISSIONS)), 12, 170, COL_TEXT, 16)
        ui.poll()
        print("MISSION ต้องเป็นหนึ่งใน", sorted(MISSIONS))
        return
    thai, problem, keys, hits, rk, alarm_text = spec
    w = build_screen(thai, problem)
    s = State(time.ticks_ms(), alarm_text)
    ai, rules, led = Ai(keys, hits), None, led_named("RGB_RED")
    try:
        note(w, "กำลังหาโมเดล (บนบอร์ดอาจนาน 15 วิ)...", COL_WARN)
        ui.poll()
        if not ai.start():
            print("ไม่พบหรือโหลดโมเดลไม่ได้:", " / ".join(keys), "- ใช้กฎอย่างเดียว")
        show_model(w, ai)
        rules = Rules(rk)                   # เปิดไมค์หลังโมเดล
        s.online = go_online(w)
        show_link(w, s.online)
        watch(w, s, ai, rules, led)
        note(w, "ครบเวลา - กด Program to Device เพื่อเล่นใหม่", COL_WARN)
    finally:                                # หยุดกลางทางก็ปล่อยแกน AI ไมค์ ไฟ และ broker เสมอ
        stop_ai()
        if rules and rules.mic_on:
            try:
                mic.stop()
            except Exception:
                pass
        set_led(led, False)
        mx(rgbmatrix.clear)
        try:
            mqtt.disconnect()
        except OSError:
            pass
        print("ภารกิจ", MISSION, "จบ: เตือน", s.alarm.count, "ครั้ง · กฎอ่านค่าไม่ได้",
              rules.fails if rules else 0, "ครั้ง")
        ui.poll()


main()

# ----- ตาคุณ แก้แล้วรันใหม่ -----
# 1) pump: เคาะโต๊ะเบา ๆ แล้วดูตาราง 2x2 ว่าขึ้นช่อง rule (กฎฝ่ายเดียว) กี่ครั้ง ลอง K_VIB = 1.5 แล้วนับใหม่
# 2) ในแอปของกลุ่ม ทำปุ่ม "เหตุจริง / เตือนผิด" ส่ง ai/feedback (สัญญาข้อ 3.11) แล้วนับว่า both / ai / rule ถูกกี่ครั้ง
# 3) ตั้ง CONFIRM_N = 1 แล้วดูว่าช่อง ai ขึ้นมั่วบ่อยขึ้นแค่ไหน เทียบกับ CONFIRM_N = 3
