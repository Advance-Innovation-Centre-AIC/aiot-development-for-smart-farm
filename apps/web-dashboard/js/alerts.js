// alerts.js - กฎเตือนฝั่งแอป (แก้ได้ในหน้าเว็บ) -> แจ้งเตือนเครื่อง (Notification API) + เสียง + สั่น
//
// กฎหนึ่งข้อ = { name, src, key, op, val, cool, level, on }
//   src: telemetry | event | plc/state | field/<ชื่อ> | silence (เงียบเกิน val วิ, key = หัวข้อที่เฝ้า)
//   ค่าต่อเนื่อง (telemetry, field, plc/state) เตือนเฉพาะ "ตอนเพิ่งเข้าเงื่อนไข" แล้วรอจนหลุดเงื่อนไขก่อนเตือนใหม่
//   เหตุการณ์ (event) เตือนทุกใบที่ตรง · ทุกกฎเว้นอย่างน้อย cool วินาทีก่อนเตือนซ้ำ ค่าที่แกว่งไปมาจะไม่เตือนรัว
// ข้อจำกัดที่ต้องรู้:
//   - แจ้งเตือนเครื่องใช้ได้เฉพาะหน้าเว็บที่เปิดผ่าน https หรือ localhost และต้องกดอนุญาตก่อน
//   - เตือนได้เฉพาะตอนหน้านี้ยังเปิดอยู่ ปิดแอปแล้วอยากให้มือถือเด้ง ใช้ apps/notify/ (ntfy)
//   - navigator.vibrate ไม่มีบน iPhone · เสียงต้องกดเปิดหนึ่งครั้ง (เบราว์เซอร์ห้ามเล่นเสียงเองก่อนคนแตะจอ)

import { DEFAULT_RULES, SOUND_VOLUME } from "./config.js";
import { load, save } from "./storage.js";
import { el, icon, $, short } from "./ui.js";

const OPS = ["<", "<=", ">", ">=", "==", "!="];
const SRCS = ["telemetry", "event", "plc/state", "field/soil", "field/tank", "ai", "silence"];
const LEVELS = { info: "แจ้งให้รู้", warn: "ระวัง", crit: "ด่วน" };
const LEVEL_PRIORITY = { info: 0, warn: 1, crit: 2 };

let nextId = 1;
const withId = (r) => ({ on: true, cool: 60, level: "warn", ...r, id: "r" + nextId++ });

export function compare(a, op, b) {
  const bothNum = typeof a === "number" && b !== "" && Number.isFinite(Number(b));
  if (bothNum) {
    const x = Number(b);
    switch (op) {
      case "<": return a < x;
      case "<=": return a <= x;
      case ">": return a > x;
      case ">=": return a >= x;
      case "==": return a === x;
      case "!=": return a !== x;
    }
  }
  if (a === undefined || a === null) return false;
  if (op === "==") return String(a) === String(b);
  if (op === "!=") return String(a) !== String(b);
  return false;
}

export class Alerts {
  // onFire(rule, text) ถูกเรียกทุกครั้งที่กฎเตือน (app.js เอาไปลงบันทึก)
  constructor({ onFire, onChange }) {
    this.onFire = onFire;
    this.onChange = onChange;
    const saved = load("rules", null);
    this.rules = (Array.isArray(saved) && saved.length ? saved : DEFAULT_RULES).map(withId);
    this.prefs = { notify: true, sound: true, vibrate: true, ...load("alert-prefs", {}) };
    this.state = {};           // id -> { active, last }
    this.audio = null;
    this.fired = 0;
  }

  persist() {
    save("rules", this.rules.map(({ id, ...r }) => r));
    save("alert-prefs", this.prefs);
  }

  // คีย์ที่มีกฎกำลังเตือนอยู่ (ให้การ์ดเปลี่ยนเป็นสีเตือน)
  hotKeys() {
    const keys = new Set();
    for (const r of this.rules) if (r.on && r.src === "telemetry" && this.state[r.id] && this.state[r.id].active) keys.add(r.key);
    return keys;
  }

  // ข้อความใหม่หนึ่งใบ: src = "telemetry" / "event" / "plc/state" / "field/soil" ...
  check(src, data, now = Date.now()) {
    for (const r of this.rules) {
      if (!r.on || r.src !== src || !(r.key in data)) continue;
      let hit = compare(data[r.key], r.op, r.val);
      const st = this.state[r.id] || (this.state[r.id] = { active: false, last: 0 });
      if (r.win > 1) {                 // เตือนเมื่อจริงอย่างน้อย need ใน win ข้อความล่าสุด (กันป้ายที่สลับไปมา)
        st.hist = (st.hist || []).concat(hit).slice(-r.win);
        hit = st.hist.filter(Boolean).length >= (r.need || r.win);
      }
      if (src === "event") {
        if (hit) this.maybeFire(r, st, data[r.key], now);
      } else {
        if (hit && !st.active) this.maybeFire(r, st, data[r.key], now);
        st.active = hit;
      }
    }
  }

  // ทุก 1 วิ: กฎ "เงียบเกิน" · lastSeen = { telemetry: ms, "plc/state": ms, ... }
  tick(lastSeen, now = Date.now()) {
    for (const r of this.rules) {
      if (!r.on || r.src !== "silence") continue;
      const seen = lastSeen[r.key];
      const st = this.state[r.id] || (this.state[r.id] = { active: false, last: 0 });
      const quiet = seen ? Math.round((now - seen) / 1000) : 0;
      const hit = !!seen && quiet > Number(r.val);
      if (hit && !st.active) this.maybeFire(r, st, quiet + " วิ", now);
      st.active = hit;
    }
  }

  maybeFire(r, st, value, now) {
    if (now - st.last < Math.max(0, Number(r.cool) || 0) * 1000) return;
    st.last = now;
    this.fired += 1;
    const text = r.name + " · " + (r.src === "silence" ? r.key + " เงียบ " + value : r.key + " = " + short(value, 20));
    this.onFire(r, text);
    this.notify(r, text);
    if (this.prefs.sound) this.beep(r.level);
    if (this.prefs.vibrate && "vibrate" in navigator) {
      try { navigator.vibrate(r.level === "crit" ? [300, 120, 300, 120, 300] : [200, 100, 200]); } catch (e) { /* ไม่รองรับ */ }
    }
  }

  // ---- แจ้งเตือนเครื่อง ----
  permission() {
    if (!("Notification" in window)) return "unsupported";
    if (!window.isSecureContext) return "insecure";
    return Notification.permission;          // default | granted | denied
  }

  async askPermission() {
    if (this.permission() !== "default") return this.permission();
    try { await Notification.requestPermission(); } catch (e) { /* Safari รุ่นเก่า */ }
    return this.permission();
  }

  async notify(r, text) {
    if (!this.prefs.notify || this.permission() !== "granted") return;
    const opts = { body: text, tag: "farm-" + r.id, icon: "icons/icon-192.png", badge: "icons/icon-192.png",
      renotify: true, requireInteraction: r.level === "crit" };
    try {
      // Android Chrome ไม่ยอมให้ new Notification() ต้องแจ้งผ่าน service worker
      const reg = "serviceWorker" in navigator ? await navigator.serviceWorker.getRegistration() : null;
      if (reg) await reg.showNotification("ฟาร์ม: " + LEVELS[r.level], opts);
      else new Notification("ฟาร์ม: " + LEVELS[r.level], opts);
    } catch (e) { /* ไม่เป็นไร ยังมีป้ายบนจอและเสียง */ }
  }

  // ---- เสียง : Web Audio สร้างเสียงเอง ไม่ต้องมีไฟล์เสียง ----
  unlockAudio() {
    try {
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!this.audio && Ctx) this.audio = new Ctx();
      if (this.audio && this.audio.state === "suspended") this.audio.resume();
    } catch (e) { this.audio = null; }
  }

  beep(level = "info") {
    if (!this.audio || this.audio.state !== "running") return;
    const count = 1 + LEVEL_PRIORITY[level];
    const freq = level === "crit" ? 1046 : level === "warn" ? 880 : 660;
    const t0 = this.audio.currentTime;
    for (let i = 0; i < count; i++) {
      const osc = this.audio.createOscillator(), gain = this.audio.createGain();
      osc.type = "sine";
      osc.frequency.value = freq;
      const s = t0 + i * 0.22;
      gain.gain.setValueAtTime(0.0001, s);
      gain.gain.exponentialRampToValueAtTime(SOUND_VOLUME, s + 0.02);
      gain.gain.exponentialRampToValueAtTime(0.0001, s + 0.16);
      osc.connect(gain).connect(this.audio.destination);
      osc.start(s);
      osc.stop(s + 0.18);
    }
  }

  test() {
    this.unlockAudio();
    this.maybeFire({ id: "test", name: "ทดสอบแจ้งเตือน", src: "event", key: "test", level: "warn", cool: 0 },
      { active: false, last: 0 }, "ok", Date.now());
  }

  // ---- ตัวแก้กฎในหน้าเว็บ ----
  renderEditor(box) {
    box.replaceChildren();
    for (const r of this.rules) box.append(this.ruleRow(r));
    const empty = $("#rules-empty");
    if (empty) empty.hidden = this.rules.length > 0;
  }

  // หนึ่งกฎ = สวิตช์เปิด/ปิด + บรรทัดสรุป (แตะเพื่อแก้รายละเอียด)
  ruleRow(r) {
    const cond = el("small", { class: "rule-cond" });
    const title = el("b", { class: "rule-name" });
    const paint = () => {
      title.textContent = r.name;
      cond.textContent = r.src === "silence" ? r.key + " เงียบเกิน " + r.val + " วิ" :
        r.src + " · " + r.key + " " + r.op + " " + r.val + (r.win > 1 ? " · " + (r.need || r.win) + " ใน " + r.win + " ใบล่าสุด" : "");
      cond.textContent += " · เว้น " + r.cool + " วิ · " + LEVELS[r.level];
      row.dataset.level = r.level;
    };
    const changed = () => { paint(); this.persist(); this.onChange && this.onChange(); };
    const field = (label, input) => el("label", { class: "rule-field" }, el("span", { text: label }), input);
    const on = el("input", { type: "checkbox", "aria-label": "เปิดใช้กฎ " + r.name });
    on.checked = !!r.on;
    on.addEventListener("change", () => { r.on = on.checked; changed(); });
    const name = el("input", { type: "text", value: r.name, maxlength: "40" });
    name.addEventListener("change", () => { r.name = name.value.trim().slice(0, 40) || "กฎไม่มีชื่อ"; changed(); });
    const src = el("select", {}, SRCS.map((s) => el("option", { value: s, text: s, selected: s === r.src })));
    if (!SRCS.includes(r.src)) src.append(el("option", { value: r.src, text: r.src, selected: true }));
    src.addEventListener("change", () => { r.src = src.value; delete this.state[r.id]; changed(); });
    const key = el("input", { type: "text", value: r.key, maxlength: "24", list: "known-keys", spellcheck: "false" });
    key.addEventListener("change", () => { r.key = key.value.trim().slice(0, 24); delete this.state[r.id]; changed(); });
    const op = el("select", {}, OPS.map((o) => el("option", { value: o, text: o, selected: o === r.op })));
    op.addEventListener("change", () => { r.op = op.value; delete this.state[r.id]; changed(); });
    const val = el("input", { type: "text", value: String(r.val), maxlength: "24", spellcheck: "false" });
    val.addEventListener("change", () => {
      const t = val.value.trim().slice(0, 24);
      r.val = t !== "" && Number.isFinite(Number(t)) ? Number(t) : t;
      delete this.state[r.id];
      changed();
    });
    const cool = el("input", { type: "number", value: String(r.cool), min: "0", max: "3600", step: "1", inputmode: "numeric" });
    cool.addEventListener("change", () => { r.cool = Math.max(0, Math.min(3600, Math.round(Number(cool.value) || 0))); cool.value = r.cool; changed(); });
    const level = el("select", {}, Object.entries(LEVELS).map(([k, v]) => el("option", { value: k, text: v, selected: k === r.level })));
    level.addEventListener("change", () => { r.level = level.value; changed(); });
    const del = el("button", { type: "button", class: "btn btn-ghost btn-sm" }, icon("trash"), "ลบกฎนี้");
    del.addEventListener("click", () => {
      const box = row.closest("#rules");
      this.rules = this.rules.filter((x) => x !== r);
      this.persist();
      this.renderEditor(box);
    });
    const row = el("div", { class: "rule" },
      el("label", { class: "switch" }, on, el("span", { class: "slider" })),
      el("details", {},
        el("summary", {}, title, cond),
        el("div", { class: "rule-body" },
          field("ชื่อกฎ", name),
          el("div", { class: "rule-grid" },
            field("ฟังหัวข้อ", src), field("คีย์", key), field("เงื่อนไข", op), field("ค่า", val),
            field("เว้น (วิ)", cool), field("ระดับ", level)),
          del)));
    paint();
    return row;
  }

  addRule(box) {
    this.rules.push(withId({ name: "กฎใหม่", src: "telemetry", key: "soil", op: "<", val: 30, cool: 60, level: "warn" }));
    this.persist();
    this.renderEditor(box);
    const last = box.lastElementChild && box.lastElementChild.querySelector("details");
    if (last) last.open = true;
  }

  reset(box) {
    this.rules = DEFAULT_RULES.map(withId);
    this.state = {};
    this.persist();
    this.renderEditor(box);
  }
}
