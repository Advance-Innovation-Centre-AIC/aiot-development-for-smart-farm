// config.js - ทุกอย่างที่ "ตั้งค่าได้" ของแดชบอร์ดอยู่ในไฟล์นี้ไฟล์เดียว
//
// หัวข้อ คีย์ JSON และคำสั่งทั้งหมดคัดมาจาก s2/app/MQTT_CONTRACT_th.md (สัญญาตัวจริง)
// อยากเพิ่มการ์ด กราฟ หรือกฎเตือน ให้แก้ CARDS / CHARTS / DEFAULT_RULES ข้างล่าง ไม่ต้องแตะไฟล์อื่น

// ---- broker (สัญญาข้อ 1) : เบราว์เซอร์ต่อพอร์ต 1883 ตรง ๆ ไม่ได้ ต้องใช้ WebSocket ----
export const BROKERS = {
  hivemq: { name: "HiveMQ (หลัก)", url: "wss://broker.hivemq.com:8884/mqtt" },
  mosquitto: { name: "Mosquitto (สำรอง)", url: "wss://test.mosquitto.org:8081/mqtt" },
};
export const DEFAULT_BROKER = "hivemq";

// ---- หัวข้อ (สัญญาข้อ 2) ----
export const ROOT = "bento-aiot";
// เลขกลุ่ม team01-team99 (team00 = บอร์ดหน้าห้อง ไม่ให้แอปไปฟังหรือสั่ง)
export const TEAM_RE = /^team(0[1-9]|[1-9][0-9])$/;

export function topics(team) {
  const base = ROOT + "/" + team + "/";
  return {
    base,
    all: base + "#",                 // subscribe ทีเดียวเห็นทุกหัวข้อของกลุ่ม
    telemetry: base + "telemetry",   // บอร์ด -> แอป ทุก 5 วิ (3.1 / 3.2 / 3.7)
    event: base + "event",           // บอร์ด -> แอป (3.3 / 3.4 / 3.8)
    cmd: base + "cmd",               // แอป -> บอร์ด (ข้อ 4 / 4.1)
    field: base + "field/",          // + ชื่อเซนเซอร์ เช่น field/soil (3.5)
    plcCmd: base + "plc/cmd",        // Gateway -> PLC (4.2) แอปฟังได้ แต่ไม่ส่งเอง
    plcState: base + "plc/state",    // PLC -> Gateway (3.6)
    ai: base + "ai",                 // บอร์ด -> แอป ผล AI จาก sf3_06 (3.9)
  };
}

// ---- กติกา (สัญญาข้อ 5) ----
export const STALE_S = 15;          // ไม่ได้ยินเกินนี้ = ค่าเก่า / PLC หลุด (3.6, 3.7)
export const CMD_GAP_MS = 1200;     // กล่องรับของบอร์ดมีช่องเดียว ส่งห่างกันอย่างน้อย 1 วิ (ข้อ 5.4)
export const MAX_PAYLOAD_B = 255;   // บอร์ดรับได้ไม่เกิน 255 ไบต์ (ข้อ 5.7)
export const HISTORY_MAX = 3000;    // เก็บย้อนหลังในเบราว์เซอร์กี่ใบ (ต่อกลุ่ม)
export const SOUND_VOLUME = 0.2;    // ความดังเสียงเตือนในแอป 0-1 (≈20% เท่ากับเสียงบนบอร์ดทุกตัวอย่าง)

// ---- การ์ด (ui.js) : หนึ่งการ์ดต่อหนึ่งคีย์ของ telemetry การ์ดขึ้นเองเมื่อบอร์ดเริ่มส่งคีย์นั้น ----
// kind: "num" ตัวเลข | "onoff" 0/1 · min/max: ค่านอกช่วงนี้ถือว่าเป็นขยะ ไม่แสดง · bar: มีหลอด 0-100
export const CARDS = [
  { key: "temp_c", label: "อุณหภูมิ", unit: "°C", digits: 1, min: -20, max: 80, icon: "thermo" },
  { key: "rh", label: "ความชื้นอากาศ", unit: "%", digits: 1, min: 0, max: 100, icon: "drop" },
  { key: "soil", label: "ความชื้นดิน", unit: "%", digits: 0, min: 0, max: 100, bar: true, icon: "soil" },
  { key: "tank", label: "น้ำในถัง", unit: "%", digits: 0, min: 0, max: 100, bar: true, icon: "tank" },
  { key: "light", label: "แสง", unit: "%", digits: 0, min: 0, max: 100, bar: true, icon: "sun" },
  { key: "hpa", label: "ความกดอากาศ", unit: "hPa", digits: 1, min: 800, max: 1100, icon: "gauge" },
  { key: "az", label: "แรงโน้มถ่วงแกนตั้ง", unit: "m/s²", digits: 2, min: -20, max: 20, icon: "tilt",
    note: "ตกลงมาก = กระถางล้ม" },
  { key: "pump", label: "ปั๊ม", kind: "onoff", on: "เดิน", off: "หยุด", icon: "pump" },
  { key: "auto", label: "รดน้ำอัตโนมัติ", kind: "onoff", on: "เปิด", off: "ปิด", icon: "auto" },
];

// ---- กราฟ (charts.js) : ทุกเส้นในกราฟเดียวต้องหน่วยเดียวกัน (ห้ามแกน y สองแกน) ----
// src: "telemetry" = คีย์ใน telemetry · "field/<ชื่อ>" = value ของโหนดนั้น · "plc/state" = คีย์ใน plc/state
// color: ชื่อสีใน css (--series-*) สีตามสิ่งที่วัด ถังน้ำสีเดียวกันทุกกราฟ
export const CHARTS = [
  { id: "chart-water", title: "ความชื้นดินและน้ำในถัง (จากบอร์ด)", unit: "%", min: 0, max: 100,
    series: [{ src: "telemetry", key: "tank", label: "น้ำในถัง", color: "tank" },
             { src: "telemetry", key: "soil", label: "ความชื้นดิน", color: "soil" }] },
  { id: "chart-field", title: "โหนดเซนเซอร์ในแปลง (field/*)", unit: "%", min: 0, max: 100,
    series: [{ src: "field/tank", key: "value", label: "tank-1", color: "tank" },
             { src: "field/soil", key: "value", label: "soil-1", color: "soil" }] },
  { id: "chart-temp", title: "อุณหภูมิ", unit: "°C",
    series: [{ src: "telemetry", key: "temp_c", label: "อุณหภูมิ", color: "temp" }] },
  { id: "chart-rh", title: "ความชื้นอากาศ", unit: "%", min: 0, max: 100,
    series: [{ src: "telemetry", key: "rh", label: "ความชื้นอากาศ", color: "rh" }] },
];
export const CHART_WINDOWS_MIN = [5, 15, 60];   // ช่วงเวลาที่เลือกดูได้ (นาที)

// ---- กฎเตือน (alerts.js) : ค่าเริ่มต้น แก้ในหน้าเว็บได้ (เก็บในเบราว์เซอร์) ----
// src: telemetry | event | plc/state | field/<ชื่อ> | silence (key = หัวข้อที่เฝ้า, val = วินาที)
// op: < <= > >= == != · cool: กี่วินาทีก่อนเตือนกฎเดิมซ้ำ · level: info | warn | crit
export const DEFAULT_RULES = [
  { name: "ดินแห้ง", src: "telemetry", key: "soil", op: "<", val: 30, cool: 60, level: "warn" },
  { name: "น้ำในถังใกล้หมด", src: "telemetry", key: "tank", op: "<", val: 15, cool: 120, level: "crit" },
  { name: "ร้อนเกิน", src: "telemetry", key: "temp_c", op: ">", val: 35, cool: 120, level: "warn" },
  { name: "พืชแย่แล้ว (sf2_04)", src: "event", key: "level", op: "==", val: 2, cool: 30, level: "crit" },
  { name: "มีคนกดเรียกที่บอร์ด (SW6)", src: "event", key: "event", op: "==", val: "sw6", cool: 10, level: "info" },
  { name: "PLC หลุด (Gateway แจ้ง)", src: "event", key: "event", op: "==", val: "plc_lost", cool: 30, level: "crit" },
  { name: "PLC ไม่ยอมเปิด ถังต่ำ", src: "plc/state", key: "why", op: "==", val: "blocked_tank", cool: 60, level: "warn" },
  { name: "บอร์ดเงียบเกิน 15 วิ", src: "silence", key: "telemetry", op: ">", val: 15, cool: 60, level: "crit" },
  { name: "AI พบความผิดปกติ (sf3_06)", src: "ai", key: "label", op: "==", val: "anomaly", win: 5, need: 3, cool: 30, level: "crit" },   // ป้ายสลับไปมา: 3 ใน 5 ใบล่าสุด
  { name: "AI alarm เริ่ม (event)", src: "event", key: "event", op: "==", val: "ai_alarm_start", cool: 30, level: "crit" },   // บอร์ดตัดสิน 3 ใน 5 เองแล้วส่งครั้งเดียว
  { name: "AI alarm ค้างอยู่ (state)", src: "ai", key: "alarm", op: "==", val: 1, cool: 60, level: "warn" },   // แอปที่เปิดกลางทางก็รู้ว่ากำลังเตือน
];

// ---- คำสั่ง (สัญญาข้อ 4, 4.1) : ไฟล์บนบอร์ดที่ฟังคำสั่งนั้น ----
// ไฟล์บนบอร์ดที่ฟังคำสั่งแต่ละชนิด (ไฟล์อื่นไม่รู้จัก บอร์ดจะไม่ทำอะไร)
export const CMD_FILES = {
  pump: ["sf2_03", "sf2_06"], led: ["sf2_03"], beep: ["sf2_03"], say: ["sf2_03"],
  ack: ["sf2_04", "sf2_03"], set: ["sf2_04"], auto: ["sf2_06"],
};
export const EXPECT_MS = 8000;      // สั่งแล้วควรเห็นผลใน telemetry / plc ภายในกี่ ms (บอร์ดรายงานทุก 5 วิ)
export const PUMP_SEC = { def: 10, min: 1, max: 30 };   // ไม่ใส่ = 10 · สูงสุด 30
export const SAY_MAX = 20;                                // จอไฟ RGB รับไม่เกิน 20 ตัว อังกฤษเท่านั้น
export const T_HI_MAX = 45;                               // {"cmd":"set","t_hi":..} ไม่เกิน 45
// เกณฑ์หนาวของพืชใน sf2_04 (CROPS) t_hi ต้องมากกว่านี้ บอร์ดถึงยอมรับ
export const CROP_T_LO = { tomato: 20, lettuce: 15, mushroom: 22, orchid: 22 };
export const CROP_TH = { tomato: "มะเขือเทศ", lettuce: "ผักสลัด", mushroom: "เห็ดนางฟ้า", orchid: "กล้วยไม้" };

// ---- ความหมายภาษาไทย ----
// why ของ plc/state (สัญญาข้อ 3.6)
export const WHY_TH = {
  start: "PLC เพิ่งเปิดเครื่อง",
  on: "เปิดตามคำสั่ง",
  off: "ปิดตามคำสั่ง",
  timeout: "ครบเวลา ดับเอง",
  blocked_tank: "ถังต่ำกว่า 10 % ไม่ยอมเปิด",
  bad_cmd: "คำสั่งอ่านไม่ได้",
  stop: "หยุดฉุกเฉินที่ PLC",
  tick: "รายงานตามรอบ",
};
export const LEVEL_TH = ["สบายดี", "เริ่มเครียด", "แย่แล้ว"];   // level ของแจ้งเตือนพืช (3.4)
export const ALERT_TH = { ok: "ปกติ", cold: "หนาวไป", hot: "ร้อนไป", dry: "แห้งไป", wet: "ชื้นไป" };
