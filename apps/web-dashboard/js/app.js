// app.js - ตัวหลัก: ต่อทุกชิ้นเข้าด้วยกัน (MQTT -> การ์ด / ผังฟาร์ม / กราฟ / กฎเตือน / ประวัติ) และปุ่มควบคุม
//
// ไหลแบบนี้: ข้อความเข้า -> onMessage() แยกตามหัวข้อ -> ตรวจค่า -> อัปเดตทุกส่วน -> จดประวัติ
// อยากเพิ่มอะไร เริ่มที่ config.js ก่อน (การ์ด กราฟ กฎ) ส่วนไฟล์นี้แก้เมื่ออยากรับหัวข้อใหม่หรือเพิ่มปุ่ม

import { BROKERS, DEFAULT_BROKER, TEAM_RE, CARDS, topics, PUMP_SEC, SAY_MAX, T_HI_MAX,
  CROP_T_LO, CROP_TH, WHY_TH, LEVEL_TH, ALERT_TH, CHART_WINDOWS_MIN, CMD_FILES, EXPECT_MS, STALE_S } from "./config.js";
import { FarmMqtt, STATUS } from "./mqtt-client.js";
import { $, $$, el, Cards, EventLog, setStatus, toast, initTabs, initTheme, hydrateIcons, num, short, ago } from "./ui.js";
import { GatewayMap } from "./map.js";
import { TrendCharts } from "./charts.js";
import { Alerts } from "./alerts.js";
import { History, load, save } from "./storage.js";

// ---- สถานะของแอป ----
const S = {
  team: null,
  broker: DEFAULT_BROKER,
  status: "idle",
  format: "",                // ไฟล์บนบอร์ดที่เดาจาก telemetry ใบล่าสุด
  tel: {},                   // คีย์ -> { v, t } ค่าล่าสุดจาก telemetry
  sim: new Set(),            // คีย์ที่เป็นค่าจำลอง (จาก "sim")
  lastSeen: {},              // หัวข้อ -> เวลาที่ได้ยินล่าสุด (ใช้กับกฎ "เงียบเกิน")
  lastN: null,
  lost: 0,
  crop: null,
  counts: { telemetry: 0, event: 0, field: 0, plc_state: 0, plc_cmd: 0, cmd_echo: 0, cmd_other: 0, bad: 0, sent: 0 },
  sent: [],                  // คำสั่งที่เราส่ง { t, text }
};

// ---- ชิ้นส่วน ----
hydrateIcons();
const selectTab = initTabs((name) => { if (name === "charts" && charts) charts.refreshAll(); save("tab", name); });
let charts = null;
initTheme(load("theme", null), (mode) => { save("theme", document.documentElement.dataset.theme || null); if (charts) charts.applyTheme(); });
const cards = new Cards($("#cards"));
const map = new GatewayMap($("#map"));
const log = new EventLog($("#log"), $$("#log-filters [data-filter]"));
charts = new TrendCharts($("#charts"));
const alerts = new Alerts({
  onFire: (rule, text) => {
    log.add({ t: Date.now(), kind: "alert", title: text, level: rule.level });
    toast("เตือน: " + text, rule.level);
    records && records.add("alert", rule.src, { name: rule.name, level: rule.level, text });
  },
  onChange: () => renderCards(),
});
let records = null;

const mq = new FarmMqtt({
  onMessage,
  onStatus: (code, detail) => {
    setStatus(code, STATUS[code] + (code === "online" ? " · " + S.team : ""), detail);
    const online = code === "online";
    for (const b of $$("[data-needs-online]")) b.disabled = !online;
    const changed = code !== S.status;
    S.status = code;
    if (changed && (code === "online" || code === "reconnecting" || code === "nolib")) {   // ลงบันทึกเฉพาะตอนเปลี่ยน
      log.add({ t: Date.now(), kind: "sys", title: STATUS[code] + (detail ? " (" + short(detail, 60) + ")" : ""),
        level: code === "online" ? "info" : "warn" });
    }
  },
  onSent: (obj, ok, why) => {
    const text = JSON.stringify(obj);
    if (!ok) { toast("ส่งไม่ได้: " + why, "warn"); return; }
    S.counts.sent += 1;
    S.sent.push({ t: Date.now(), text });
    log.add({ t: Date.now(), kind: "out", title: "ส่งแล้ว: " + describeCmd(obj), raw: text });
    records.add("out", "cmd", obj);
    toast("ส่งแล้ว: " + describeCmd(obj), "ok");
    expectEffect(obj);
  },
});

// ---- ข้อความเข้า: แยกตามหัวข้อ ----
function onMessage(topic, text) {
  const t = Date.now();
  const T = topics(S.team);
  if (!topic.startsWith(T.base)) return;
  const sub = topic.slice(T.base.length);
  let data = null;
  try { data = JSON.parse(text); } catch (e) { /* ไม่ใช่ JSON */ }
  const obj = data && typeof data === "object" && !Array.isArray(data) ? data : null;   // ต้องเป็น JSON object เท่านั้น
  if (sub === "cmd") return onCmd(text, obj, t);
  if (!obj) {
    S.counts.bad += 1;
    log.add({ t, kind: "sys", title: "ข้อความบน " + short(sub, 30) + " ไม่ใช่ JSON object (ข้ามไป)", raw: text, level: "warn" });
    return;
  }
  if (sub === "telemetry") onTelemetry(obj, t);
  else if (sub === "event") onEvent(obj, t, text);
  else if (sub === "plc/state") onPlcState(obj, t, text);
  else if (sub === "plc/cmd") onPlcCmd(obj, t, text);
  else if (sub.startsWith("field/") && /^[A-Za-z0-9_-]{1,24}$/.test(sub.slice(6))) onField(sub.slice(6), obj, t);
}

// ไฟล์ไหนบนบอร์ดส่งใบนี้มา (ดูจากคีย์ ตามสัญญาข้อ 3.1 / 3.2 / 3.7)
function guessFormat(d) {
  if (d.by === "gateway") return "sf2_06 Gateway";
  if ("temp_c" in d && d.sim === "soil light tank") return "sf2_02 ค่าฟาร์ม";
  if ("pump" in d && d.sim === "soil tank") return "sf2_03 ปั๊ม";
  return "ไฟล์ของกลุ่มเอง";
}

function onTelemetry(d, t) {
  S.counts.telemetry += 1;
  S.lastSeen.telemetry = t;
  for (const c of CARDS) {
    if (!(c.key in d)) continue;
    const raw = d[c.key];
    const v = c.kind === "onoff" ? (raw === 1 ? 1 : raw === 0 ? 0 : null) : num(raw, c.min, c.max);
    S.tel[c.key] = { v, t };
  }
  S.sim = new Set(typeof d.sim === "string" ? d.sim.split(/\s+/).slice(0, 10) : []);
  // n = ลำดับใบ: กระโดด = มีใบหาย · ลดลง = บอร์ดเริ่มรันไฟล์ใหม่
  const n = num(d.n, 0, 1e9);
  if (n !== null && S.lastN !== null && n > S.lastN + 1) S.lost += n - S.lastN - 1;
  if (n !== null) S.lastN = n;
  const format = guessFormat(d);
  S.format = format;
  map.onTelemetry(d, t, format);
  checkExpect("telemetry", d);
  highlightCommands(format);
  records.add("telemetry", "telemetry", d);
  charts.add("telemetry", d, t);
  alerts.check("telemetry", d, t);
  renderCards();
}

function describeEvent(d) {
  const e = d.event;
  if (e === "sw6") return "มีคนกดปุ่มเรียก (SW6) ที่บอร์ด";
  if (e === "plc_lost") return "Gateway แจ้ง: PLC เงียบเกิน 15 วิ";
  if (e === "auto_water") return "Gateway รดน้ำเองเพราะดินแห้ง (" + (num(d.soil, 0, 100) ?? "?") + " %)";
  if (e === "pump") return "ปั๊ม" + (d.pump === 1 ? "เดิน" : "หยุด") + " · " + (WHY_TH[d.why] || short(d.why ?? "", 16));
  if ("level" in d && "crop" in d) {
    const lv = num(d.level, 0, 2);
    const why = String(d.alert ?? "").split("+").map((x) => ALERT_TH[x] || short(x, 8)).join(" + ");
    return "พืช" + (CROP_TH[d.crop] || short(d.crop, 16)) + ": " + (lv === null ? "?" : LEVEL_TH[lv]) +
      " (" + why + ") · " + (num(d.temp_c, -50, 100) ?? "—") + " °C " + (num(d.rh, 0, 100) ?? "—") + " %";
  }
  return "เหตุการณ์: " + short(e ?? "ไม่มีชื่อ", 24);
}

function onEvent(d, t, text) {
  S.counts.event += 1;
  S.lastSeen.event = t;
  const lv = num(d.level, 0, 2);
  const level = lv === 2 || d.event === "plc_lost" ? "crit" : lv === 1 ? "warn" : "info";
  log.add({ t, kind: "event", title: describeEvent(d), raw: text, level });
  if (typeof d.crop === "string" && d.crop in CROP_T_LO) {
    S.crop = d.crop;
    $("#thi-hint").textContent = "พืชตอนนี้: " + CROP_TH[d.crop] + " · ต้องมากกว่า " + CROP_T_LO[d.crop] + " และไม่เกิน " + T_HI_MAX;
    if (!S.lastSeen.telemetry || t - S.lastSeen.telemetry > 15000) map.board = { t, format: "sf2_04 แจ้งเตือนพืช" };
  }
  map.onEvent(d, t);
  records.add("event", "event", d);
  alerts.check("event", d, t);
}

function onPlcState(d, t, text) {
  S.counts.plc_state += 1;
  S.lastSeen["plc/state"] = t;
  map.onPlcState(d, t);
  checkExpect("plc/state", d);
  records.add("plc", "plc/state", d);
  charts.add("plc/state", d, t);
  alerts.check("plc/state", d, t);
  if (d.why !== "tick") {             // รายงานตามรอบไม่ลงบันทึก ไม่งั้นบันทึกล้นทุก 5 วิ
    log.add({ t, kind: "plc", title: "PLC: ปั๊ม" + (d.pump === 1 ? "เดิน" : "หยุด") + " · " + (WHY_TH[d.why] || short(d.why ?? "", 16)),
      raw: text, level: d.why === "blocked_tank" || d.why === "bad_cmd" ? "warn" : "info" });
  }
}

function onPlcCmd(d, t, text) {
  S.counts.plc_cmd += 1;
  map.onPlcCmd(d, t);
  checkExpect("plc/cmd", d);
  records.add("plccmd", "plc/cmd", d);
  log.add({ t, kind: "plc", title: "Gateway สั่ง PLC: " + (d.pump ? "เปิดปั๊ม " + (num(d.sec, 0, 999) ?? 10) + " วิ" : "ปิดปั๊ม"), raw: text });
}

function onField(name, d, t) {
  S.counts.field += 1;
  S.lastSeen["field/" + name] = t;
  map.onField(name, d, t);
  records.add("field", "field/" + name, d);
  charts.add("field/" + name, d, t);
  alerts.check("field/" + name, d, t);
}

function onCmd(text, obj, t) {
  map.onAppCmd(obj ? describeCmd(obj) : "ไม่ใช่ JSON", t);
  if (mq.isOurs(text)) { S.counts.cmd_echo += 1; return; }    // broker ส่งคำสั่งของเรากลับมา = ถึง broker แล้วจริง
  S.counts.cmd_other += 1;
  log.add({ t, kind: "cmd", title: "มีคนอื่นส่งคำสั่ง: " + (obj ? describeCmd(obj) : "ไม่ใช่ JSON"), raw: text, level: "warn" });
  records.add("cmd", "cmd", obj || { text: short(text, 200) });
}

function renderCards() {
  cards.render(S.tel, S.sim, alerts.hotKeys());
}

// ---- คำสั่ง (สัญญาข้อ 4 และ 4.1) ----
function describeCmd(c) {
  switch (c.cmd) {
    case "pump": return c.on ? "เปิดปั๊ม " + (c.sec ?? PUMP_SEC.def) + " วิ" : "ปิดปั๊ม";
    case "led": return "led " + (c.on ? "เปิด" : "ปิด");
    case "auto": return "โหมดรดน้ำอัตโนมัติ " + (c.on ? "เปิด" : "ปิด");
    case "beep": return "เรียกคนที่ฟาร์ม (beep)";
    case "say": return 'ขึ้นจอไฟ "' + short(c.text ?? "", SAY_MAX) + '"';
    case "ack": return "รับทราบแจ้งเตือน";
    case "set": return "ตั้งเกณฑ์ร้อน t_hi = " + short(c.t_hi, 6);
    default: return "คำสั่ง " + short(c.cmd ?? "?", 16);
  }
}

function send(obj) {
  // บอร์ดส่ง telemetry อยู่ แต่ไฟล์ที่รันไม่ฟังคำสั่งนี้: บอกก่อน (ยังส่งให้ เผื่อกลุ่มแก้ไฟล์บอร์ดเอง)
  const files = CMD_FILES[obj.cmd] || [];
  const live = S.lastSeen.telemetry && Date.now() - S.lastSeen.telemetry < STALE_S * 1000;
  if (live && S.format.startsWith("sf2_") && !files.includes(S.format.slice(0, 6))) {
    toast("บอร์ดตอนนี้ดูเหมือนรัน " + S.format.slice(0, 6) + " ซึ่งไม่ฟังคำสั่ง " + obj.cmd + " (ฟังใน " + files.join(", ") + ")", "warn");
  }
  mq.sendCommand(obj);
}

// ---- คำสั่งได้ผลไหม : กล่องรับของบอร์ดมีช่องเดียว คำสั่งอาจถูกข้อความอื่นทับ (สัญญาข้อ 5.4 และ 5.8) ----
// จึงดูผลจริงจากสิ่งที่บอร์ดรายงานกลับ แทนการเชื่อว่า "ส่งแล้ว = ทำแล้ว"
const expects = [];
function expectEffect(obj) {
  const t = Date.now();
  if (!S.lastSeen.telemetry || t - S.lastSeen.telemetry > STALE_S * 1000) return;   // บอร์ดเงียบ ตรวจไม่ได้
  const gw = S.format.startsWith("sf2_06"), want = obj.on ? 1 : 0;
  let ok = null;
  if (obj.cmd === "auto" && gw) ok = (k, d) => k === "telemetry" && d.by === "gateway" && d.auto === want;
  else if (obj.cmd === "pump" && gw) ok = (k, d) => (k === "plc/cmd" || k === "plc/state") && d.pump === want;
  else if (obj.cmd === "pump" && S.format.startsWith("sf2_03")) ok = (k, d) => k === "telemetry" && d.pump === want;
  if (ok) expects.push({ obj, t, ok });
}
function checkExpect(kind, d) {
  const now = Date.now();
  for (let i = expects.length - 1; i >= 0; i--) {
    const e = expects[i];
    if (now - e.t > 300 && e.ok(kind, d)) {
      expects.splice(i, 1);
      log.add({ t: now, kind: "sys", title: "บอร์ดทำตามแล้ว: " + describeCmd(e.obj) + " (" + ((now - e.t) / 1000).toFixed(1) + " วิ)" });
    }
  }
}
function expireExpect(now) {
  for (let i = expects.length - 1; i >= 0; i--) {
    const e = expects[i];
    if (now - e.t < EXPECT_MS) continue;
    expects.splice(i, 1);
    const text = "ยังไม่เห็นผลของ \"" + describeCmd(e.obj) + "\" ใน " + EXPECT_MS / 1000 + " วิ: คำสั่งอาจถูกข้อความอื่นทับในกล่องรับของบอร์ด หรือบอร์ดปฏิเสธ (เช่น ถังต่ำ) ลองส่งอีกครั้ง";
    log.add({ t: now, kind: "sys", title: text, level: "warn" });
    toast(text, "warn");
  }
}

// เน้นป้ายไฟล์ที่ฟังคำสั่งนั้น ถ้าตรงกับไฟล์ที่บอร์ดน่าจะรันอยู่
function highlightCommands(format) {
  const file = format.slice(0, 6);
  for (const chip of $$("#controls [data-file]")) chip.classList.toggle("match", chip.dataset.file === file);
}

// เฉพาะอักษรอังกฤษ ตัวเลข เครื่องหมาย (ช่วง " " ถึง "~") ไม่เกิน 20 ตัว เหมือนบอร์ด
function asciiOnly(text) {
  return Array.from(String(text)).filter((ch) => ch >= " " && ch <= "~").join("").slice(0, SAY_MAX);
}

function initControls() {
  const sec = $("#pump-sec"), secOut = $("#pump-sec-out");
  sec.min = PUMP_SEC.min; sec.max = PUMP_SEC.max; sec.value = load("pump-sec", PUMP_SEC.def);
  const showSec = () => { secOut.textContent = sec.value + " วิ"; };
  sec.addEventListener("input", showSec);
  showSec();
  $("#pump-on").addEventListener("click", () => {
    const s = Math.max(PUMP_SEC.min, Math.min(PUMP_SEC.max, Math.round(Number(sec.value)) || PUMP_SEC.def));
    save("pump-sec", s);
    send({ cmd: "pump", on: 1, sec: s });
  });
  $("#pump-off").addEventListener("click", () => send({ cmd: "pump", on: 0 }));
  $("#auto-on").addEventListener("click", () => send({ cmd: "auto", on: 1 }));
  $("#auto-off").addEventListener("click", () => send({ cmd: "auto", on: 0 }));
  $("#beep").addEventListener("click", () => send({ cmd: "beep" }));
  $("#ack").addEventListener("click", () => send({ cmd: "ack" }));

  const say = $("#say"), sayOut = $("#say-preview");
  say.maxLength = 40;
  const preview = () => {
    const clean = asciiOnly(say.value).trim();
    sayOut.textContent = say.value && clean !== say.value ? "บอร์ดจะได้: \"" + clean + "\" (ตัดภาษาไทย/เกิน " + SAY_MAX + " ตัวทิ้ง)" : "";
  };
  say.addEventListener("input", preview);
  $("#say-form").addEventListener("submit", (e) => {
    e.preventDefault();
    const clean = asciiOnly(say.value).trim();
    if (!clean) { toast("พิมพ์ภาษาอังกฤษหรือตัวเลข จอไฟ RGB แสดงภาษาไทยไม่ได้", "warn"); return; }
    send({ cmd: "say", text: clean });
  });

  $("#thi-form").addEventListener("submit", (e) => {
    e.preventDefault();
    const v = Number($("#thi").value);
    const lo = S.crop ? CROP_T_LO[S.crop] : 0;
    if (!Number.isInteger(v) || v <= lo || v > T_HI_MAX) {
      toast("t_hi ต้องเป็นจำนวนเต็ม มากกว่า " + lo + " และไม่เกิน " + T_HI_MAX, "warn");
      return;
    }
    send({ cmd: "set", t_hi: v });
  });
  $("#thi").max = T_HI_MAX;
}

// ---- ส่วนแจ้งเตือน ----
function initAlertsPanel() {
  const box = $("#rules");
  alerts.renderEditor(box);
  const permText = { granted: "อนุญาตแล้ว", denied: "ถูกปฏิเสธ (แก้ในการตั้งค่าเว็บไซต์ของเบราว์เซอร์)", default: "ยังไม่ได้อนุญาต",
    unsupported: "เบราว์เซอร์นี้ไม่มีแจ้งเตือนเว็บ (iPhone: เพิ่มลงหน้าจอโฮมก่อน)", insecure: "ต้องเปิดผ่าน https หรือ localhost" };
  const paintPerm = () => {
    const p = alerts.permission();
    $("#perm-state").textContent = permText[p] || p;
    $("#perm-state").dataset.state = p;
    $("#perm-btn").hidden = p !== "default";
  };
  paintPerm();
  $("#perm-btn").addEventListener("click", async () => { alerts.unlockAudio(); await alerts.askPermission(); paintPerm(); });
  for (const k of ["notify", "sound", "vibrate"]) {
    const box2 = $("#pref-" + k);
    box2.checked = !!alerts.prefs[k];
    box2.addEventListener("change", () => { alerts.prefs[k] = box2.checked; if (k === "sound") alerts.unlockAudio(); alerts.persist(); });
  }
  if (!("vibrate" in navigator)) $("#pref-vibrate-note").textContent = "(เครื่องนี้สั่นไม่ได้)";
  $("#test-alert").addEventListener("click", () => alerts.test());
  $("#add-rule").addEventListener("click", () => alerts.addRule(box));
  $("#reset-rules").addEventListener("click", () => { if (confirm("คืนกฎเตือนเป็นค่าเริ่มต้น?")) alerts.reset(box); });
  // เบราว์เซอร์ห้ามเล่นเสียงก่อนคนแตะจอ: แตะครั้งแรกที่ไหนก็ได้ = ปลดล็อกเสียง
  document.addEventListener("pointerdown", () => { if (alerts.prefs.sound) alerts.unlockAudio(); }, { once: true });
}

// ---- ส่วนบันทึก ----
function initLogPanel() {
  $("#csv-btn").addEventListener("click", () => {
    const name = records.download();
    toast("ดาวน์โหลด " + name + " (" + records.items.length + " แถว)", "ok");
  });
  $("#clear-btn").addEventListener("click", () => {
    if (!confirm("ล้างประวัติที่เก็บในเครื่องนี้ของ " + S.team + "?")) return;
    records.clear();
    charts.clear();
    toast("ล้างประวัติแล้ว", "info");
  });
  const seg = $("#chart-window");
  for (const m of CHART_WINDOWS_MIN) {
    const b = el("button", { type: "button", class: "seg", "aria-pressed": String(m === charts.windowMin), text: m + " นาที" });
    b.addEventListener("click", () => {
      charts.setWindow(m);
      for (const x of seg.children) x.setAttribute("aria-pressed", String(x === b));
    });
    seg.append(b);
  }
}

// ---- ทุก 1 วิ: อายุข้อความ ค่าเก่า นับถอยหลังปั๊ม กฎเงียบ สถิติ ----
function tick() {
  const now = Date.now();
  renderCards();
  map.render(now);
  alerts.tick(S.lastSeen, now);
  expireExpect(now);
  const last = Math.max(0, ...Object.values(S.lastSeen));
  $("#last-seen").textContent = last ? "ข้อความล่าสุด " + ago(last, now) : "ยังไม่มีข้อความ";
  const c = S.counts;
  $("#stat-telemetry").textContent = c.telemetry;
  $("#stat-event").textContent = c.event;
  $("#stat-field").textContent = c.field;
  $("#stat-plc").textContent = c.plc_state + c.plc_cmd;
  $("#stat-sent").textContent = c.sent;
  $("#stat-lost").textContent = S.lost;
  $("#stat-history").textContent = records ? records.items.length : 0;
  $("#queue").textContent = mq.queued ? "คิวคำสั่ง " + mq.queued + " ใบ (ส่งห่างกันใบละ 1.2 วิ)" : "";
}

// ---- เลือกกลุ่มและ broker ----
function initTeamForm() {
  const form = $("#team-form"), input = $("#team"), sel = $("#broker"), err = $("#team-error");
  for (const [k, b] of Object.entries(BROKERS)) sel.append(el("option", { value: k, text: b.name }));
  input.value = S.team || "";
  sel.value = S.broker;
  form.addEventListener("submit", (e) => {
    e.preventDefault();
    const team = input.value.trim().toLowerCase();
    if (!TEAM_RE.test(team)) {
      err.textContent = "ใส่เลขกลุ่มแบบ team05 (team01 ถึง team99)";
      input.setAttribute("aria-invalid", "true");
      input.focus();
      return;
    }
    err.textContent = "";
    input.removeAttribute("aria-invalid");
    save("team", team);
    save("broker", sel.value);
    // เปลี่ยนกลุ่ม = โหลดหน้าใหม่ ทุกอย่างเริ่มสะอาด (ประวัติแยกตามกลุ่มอยู่แล้ว)
    const url = "?team=" + team + (sel.value !== DEFAULT_BROKER ? "&broker=" + sel.value : "");
    if (team !== S.team) location.assign(url);
    else { S.broker = sel.value; setUrl(url); mq.connect(BROKERS[S.broker].url, S.team); }
  });
}

function setUrl(url) {
  try { window.history.replaceState(null, "", url); } catch (e) { /* file:// บางเบราว์เซอร์ไม่ยอม */ }
}

// ---- PWA : service worker (เฉพาะ https / localhost) + ปุ่มติดตั้งบน Android ----
function initPwa() {
  if ("serviceWorker" in navigator && window.isSecureContext) {
    navigator.serviceWorker.register("./sw.js").catch(() => { /* ไม่มี SW ก็ใช้งานได้ปกติ */ });
  }
  let deferred = null;
  const btn = $("#install-btn");
  window.addEventListener("beforeinstallprompt", (e) => {
    e.preventDefault();
    deferred = e;
    btn.hidden = false;
  });
  btn.addEventListener("click", async () => {
    if (!deferred) return;
    deferred.prompt();
    await deferred.userChoice;
    deferred = null;
    btn.hidden = true;
  });
}

// ---- เริ่ม ----
function start() {
  const params = new URLSearchParams(location.search);
  const fromUrl = (params.get("team") || "").trim().toLowerCase();
  const saved = load("team", "");
  S.team = TEAM_RE.test(fromUrl) ? fromUrl : TEAM_RE.test(saved) ? saved : null;
  const b = params.get("broker") || load("broker", DEFAULT_BROKER);
  S.broker = b in BROKERS ? b : DEFAULT_BROKER;
  initTeamForm();
  initControls();
  initAlertsPanel();
  initPwa();
  const tab = load("tab", "overview");
  selectTab($$(".tabbar [data-tab]").some((t) => t.dataset.tab === tab) ? tab : "overview");
  if (fromUrl && !TEAM_RE.test(fromUrl)) $("#team-error").textContent = "เลขกลุ่มในลิงก์ไม่ถูกต้อง: " + short(fromUrl, 12);

  if (!S.team) {
    records = new History("none");
    initLogPanel();
    setStatus("idle", STATUS.idle, "");
    for (const btn of $$("[data-needs-online]")) btn.disabled = true;
    $("#team").focus();
    setInterval(tick, 1000);
    return;
  }
  document.title = "ฟาร์ม " + S.team;
  $("#team-label").textContent = S.team;
  save("team", S.team);
  records = new History(S.team);
  initLogPanel();
  charts.loadHistory(records.items);
  for (const btn of $$("[data-needs-online]")) btn.disabled = true;
  setUrl("?team=" + S.team + (S.broker !== DEFAULT_BROKER ? "&broker=" + S.broker : ""));
  mq.connect(BROKERS[S.broker].url, S.team);
  setInterval(tick, 1000);
  setInterval(() => charts.refreshAll(), 5000);
  tick();
}

start();

// ให้เครื่องมือทดสอบ (และนักพัฒนาใน DevTools) ดูสถานะได้: window.__farm
window.__farm = { S, mq, alerts, get records() { return records; }, charts, map, expects };
