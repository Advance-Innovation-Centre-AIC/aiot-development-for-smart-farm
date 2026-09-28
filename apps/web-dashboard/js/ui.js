// ui.js - ชิ้นส่วนหน้าจอ: ไอคอน การ์ด บันทึกเหตุการณ์ ป้ายสถานะ แท็บ ธีม และป้ายแจ้งสั้น (toast)
//
// กฎเหล็ก: ข้อมูลที่มาจาก broker ใส่หน้าจอด้วย textContent เท่านั้น ห้าม innerHTML
// (broker สาธารณะ ใครก็ส่งข้อความเข้าหัวข้อเราได้ ถ้าใช้ innerHTML เขาฝังโค้ดมารันในหน้าเราได้)

import { CARDS, STALE_S } from "./config.js";

// ---- ตัวช่วยสร้าง element ----
export function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k === "class") node.className = v;
    else if (k === "text") node.textContent = v;
    else if (k.startsWith("on")) node.addEventListener(k.slice(2), v);
    else node.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat()) {
    if (c === null || c === undefined || c === false) continue;
    node.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return node;
}

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

// ---- ไอคอน (วาดเอง เส้น 24x24) : เป็นค่าคงที่ในโค้ด ไม่ใช่ข้อมูลจาก broker จึงสร้างด้วย DOMParser ได้ ----
const ICONS = {
  home: '<path d="M3 11l9-7 9 7"/><path d="M5 10v10h14V10"/><path d="M10 20v-5h4v5"/>',
  chart: '<path d="M4 4v16h16"/><path d="M7 15l4-5 3 3 5-7"/>',
  sliders: '<path d="M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1"/><circle cx="15" cy="6" r="2"/><circle cx="9" cy="12" r="2"/><circle cx="17" cy="18" r="2"/>',
  bell: '<path d="M6 16v-5a6 6 0 0 1 12 0v5l2 2H4z"/><path d="M10 21h4"/>',
  list: '<path d="M9 6h11M9 12h11M9 18h11"/><path d="M4.5 6h.01M4.5 12h.01M4.5 18h.01"/>',
  moon: '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2.5M12 19.5V22M2 12h2.5M19.5 12H22M4.9 4.9l1.8 1.8M17.3 17.3l1.8 1.8M4.9 19.1l1.8-1.8M17.3 6.7l1.8-1.8"/>',
  thermo: '<path d="M10 4.5a2 2 0 0 1 4 0v9.3a4 4 0 1 1-4 0z"/><path d="M12 10v6"/>',
  drop: '<path d="M12 3.5s6 6.3 6 10.7a6 6 0 0 1-12 0C6 9.8 12 3.5 12 3.5z"/>',
  soil: '<path d="M12 19v-7"/><path d="M12 12.5C12 9 9.8 7 6 7c0 3.4 2.3 5.5 6 5.5z"/><path d="M12 10.5c0-3.4 2.2-5.5 6-5.5 0 3.4-2.2 5.5-6 5.5z"/><path d="M4 20h16"/>',
  tank: '<path d="M6 5h12v14a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1z"/><path d="M6 12c2 1.3 4 1.3 6 0s4-1.3 6 0"/>',
  gauge: '<path d="M4 17a8 8 0 1 1 16 0"/><path d="M12 17l3.5-4.5"/>',
  tilt: '<path d="M6.5 11h11l-1.7 9H8.2z"/><path d="M12 11V6"/><path d="M12 8c-1.6-2-3.6-2.4-5-1.5"/>',
  pump: '<circle cx="12" cy="12" r="8"/><path d="M12 12V6.5M12 12l4.8 2.8M12 12l-4.8 2.8"/><circle cx="12" cy="12" r="1.2"/>',
  auto: '<path d="M20 12a8 8 0 1 1-2.4-5.7"/><path d="M20 4v4h-4"/><path d="M12 8v4l2.5 2"/>',
  node: '<path d="M12 21v-8"/><circle cx="12" cy="11" r="2"/><path d="M8.6 7.6a4.8 4.8 0 0 1 6.8 0M6.1 5.1a8.3 8.3 0 0 1 11.8 0"/>',
  chip: '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9.5 2.5V6M14.5 2.5V6M9.5 18v3.5M14.5 18v3.5M2.5 9.5H6M2.5 14.5H6M18 9.5h3.5M18 14.5h3.5"/><path d="M10 10h4v4h-4z"/>',
  plc: '<rect x="4" y="4" width="16" height="16" rx="2"/><path d="M8 8h3v3H8z"/><path d="M14 8h2M14 11h2M8 15h8"/>',
  download: '<path d="M12 4v11M7 10l5 5 5-5"/><path d="M5 20h14"/>',
  trash: '<path d="M4 7h16M9 7V4h6v3M6.5 7l1 13h9l1-13"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  wifi: '<path d="M4 9.5a11.3 11.3 0 0 1 16 0M7 12.8a7 7 0 0 1 10 0M10 16a2.8 2.8 0 0 1 4 0"/><path d="M12 19.5h.01"/>',
  install: '<rect x="7" y="2.5" width="10" height="19" rx="2"/><path d="M12 8v6M9.5 11.5 12 14l2.5-2.5"/>',
  speaker: '<path d="M4 9.5h3.5L12 6v12l-4.5-3.5H4z"/><path d="M15.5 9a4 4 0 0 1 0 6M18 6.5a7.5 7.5 0 0 1 0 11"/>',
};

export function icon(name, cls = "icon") {
  const svg = new DOMParser().parseFromString(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
    'stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
    (ICONS[name] || ICONS.node) + "</svg>", "image/svg+xml").documentElement;
  svg.setAttribute("class", cls);
  return document.importNode(svg, true);
}

// <i data-icon="home"></i> ใน index.html -> ไอคอนจริง
export function hydrateIcons(root = document) {
  for (const i of $$("i[data-icon]", root)) i.replaceWith(icon(i.dataset.icon, i.className || "icon"));
}

// ---- เวลา ----
export function hhmmss(ms) {
  return new Date(ms).toLocaleTimeString("th-TH", { hour12: false });
}
export function ago(ms, now = Date.now()) {
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 3) return "เมื่อกี้";
  if (s < 60) return s + " วิที่แล้ว";
  if (s < 3600) return Math.floor(s / 60) + " นาทีที่แล้ว";
  return Math.floor(s / 3600) + " ชม.ที่แล้ว";
}

// ---- ตัวเลข: รับเฉพาะ number จริงที่อยู่ในช่วง (ค่านอกช่วง = ขยะจากใครก็ไม่รู้) ----
export function num(v, min = -Infinity, max = Infinity) {
  return typeof v === "number" && Number.isFinite(v) && v >= min && v <= max ? v : null;
}
export function short(v, n = 40) {
  return String(v).slice(0, n);
}

// ---- ป้ายสถานะการเชื่อมต่อ ----
export function setStatus(code, text, detail) {
  const badge = $("#status");
  badge.dataset.state = code;
  $("#status-text").textContent = text;
  badge.title = detail || text;
}

// ---- toast : ป้ายแจ้งสั้น ๆ มุมจอ หายเองใน 5 วิ ----
export function toast(text, level = "info") {
  const box = $("#toasts");
  const t = el("div", { class: "toast", "data-level": level, role: "status", text: short(text, 140) });
  box.prepend(t);
  while (box.children.length > 3) box.lastElementChild.remove();
  setTimeout(() => t.classList.add("out"), 4500);
  setTimeout(() => t.remove(), 5000);
}

// ---- การ์ดค่าจากบอร์ด (หนึ่งการ์ดต่อหนึ่งคีย์ใน CARDS) ----
export class Cards {
  constructor(box) {
    this.box = box;
    this.nodes = {};
    this.empty = el("p", { class: "empty", text: "รอข้อมูลจากบอร์ด... (บอร์ดส่ง telemetry ทุก 5 วินาที)" });
    box.append(this.empty);
  }

  // tel = { key: {v, t} } · sim = คีย์ที่เป็นค่าจำลองจากลูกบิด · hot = คีย์ที่กฎเตือนกำลังทำงาน
  render(tel, sim, hot, now = Date.now()) {
    for (const c of CARDS) {
      const got = tel[c.key];
      if (!got) continue;
      const node = this.nodes[c.key] || this.make(c);
      const stale = now - got.t > STALE_S * 1000;
      node.card.dataset.state = hot.has(c.key) ? "alert" : stale ? "stale" : "live";
      node.sim.hidden = !sim.has(c.key);
      if (got.v === null) {
        node.num.textContent = "—";
        node.unit.textContent = "";
      } else if (c.kind === "onoff") {
        node.num.textContent = got.v ? c.on : c.off;
        node.unit.textContent = "";
        node.card.dataset.on = got.v ? "1" : "0";
      } else {
        node.num.textContent = got.v.toFixed(c.digits);
        node.unit.textContent = c.unit;
      }
      if (node.bar) node.bar.style.width = (got.v === null ? 0 : Math.max(0, Math.min(100, got.v))) + "%";
      node.foot.textContent = (stale ? "ค่าเก่า · " : "") + ago(got.t, now);
    }
    this.empty.hidden = Object.keys(this.nodes).length > 0;
  }

  make(c) {
    const num = el("span", { class: "num", text: "—" });
    const unit = el("span", { class: "unit" });
    const sim = el("span", { class: "chip chip-sim", text: "จำลอง", title: "ค่านี้มาจากลูกบิดบนบอร์ด ไม่ใช่เซนเซอร์จริง" });
    const age = el("span");
    const foot = el("div", { class: "card-foot" }, age, sim);
    const barFill = c.bar ? el("span") : null;
    const card = el("article", { class: "card", "data-key": c.key },
      el("div", { class: "card-top" }, icon(c.icon), el("span", { class: "card-label", text: c.label })),
      el("div", { class: "card-value" }, num, unit),
      barFill ? el("div", { class: "bar", "aria-hidden": "true" }, barFill) : null,
      c.note ? el("div", { class: "card-note", text: c.note }) : null,
      foot);
    // เรียงการ์ดตามลำดับใน CARDS เสมอ ไม่ว่าคีย์ไหนมาก่อน
    const order = CARDS.findIndex((x) => x.key === c.key);
    const after = Object.values(this.nodes).find((n) => n.order > order);
    this.box.insertBefore(card, after ? after.card : null);
    return (this.nodes[c.key] = { card, num, unit, sim, foot: age, bar: barFill, order });
  }
}

// ---- บันทึกเหตุการณ์ ----
const KIND_TH = { event: "บอร์ด", out: "เราสั่ง", cmd: "คนอื่นสั่ง", plc: "PLC", alert: "เตือน", sys: "ระบบ" };
const FILTERS = { all: null, event: ["event", "plc"], cmd: ["out", "cmd"], alert: ["alert"], sys: ["sys"] };

export class EventLog {
  constructor(list, chips) {
    this.list = list;
    this.filter = "all";
    this.max = 200;
    for (const chip of chips) {
      chip.addEventListener("click", () => {
        this.filter = chip.dataset.filter;
        chips.forEach((c) => c.setAttribute("aria-pressed", c === chip ? "true" : "false"));
        this.applyFilter();
      });
    }
  }

  // entry = { t, kind, title, raw, level }
  add(entry) {
    const li = el("li", { class: "log-item", "data-kind": entry.kind, "data-level": entry.level || "info" },
      el("div", { class: "log-head" },
        el("time", { text: hhmmss(entry.t) }),
        el("span", { class: "chip chip-kind", text: KIND_TH[entry.kind] || entry.kind }),
        el("span", { class: "log-title", text: short(entry.title, 140) })),
      entry.raw ? el("code", { class: "log-raw", text: short(entry.raw, 200) }) : null);
    this.list.prepend(li);
    while (this.list.children.length > this.max) this.list.lastElementChild.remove();
    this.show(li);
    const empty = this.list.parentElement.querySelector(".empty");
    if (empty) empty.hidden = true;
  }

  show(li) {
    const keep = FILTERS[this.filter];
    li.hidden = !!keep && !keep.includes(li.dataset.kind);
  }

  applyFilter() {
    for (const li of this.list.children) this.show(li);
  }
}

// ---- แท็บ (จอแคบ) : จอกว้างแสดงทุกส่วนพร้อมกัน แท็บซ่อนด้วย CSS ----
export function initTabs(onChange) {
  const tabs = $$(".tabbar [data-tab]");
  const select = (name) => {
    for (const t of tabs) t.setAttribute("aria-selected", t.dataset.tab === name ? "true" : "false");
    for (const s of $$("[data-section]")) s.classList.toggle("active", s.dataset.section === name);
    document.body.dataset.tab = name;
    if (onChange) onChange(name);
  };
  tabs.forEach((t) => t.addEventListener("click", () => { select(t.dataset.tab); window.scrollTo(0, 0); }));
  return select;
}

// ---- ธีมสว่าง/มืด : ค่าเริ่มต้นตามเครื่อง กดสลับแล้วจำไว้ ----
export function initTheme(saved, onChange) {
  const root = document.documentElement;
  const btn = $("#theme-btn");
  const media = window.matchMedia("(prefers-color-scheme: dark)");
  const current = () => root.dataset.theme || (media.matches ? "dark" : "light");
  const paint = () => {
    const mode = current();
    btn.replaceChildren(icon(mode === "dark" ? "sun" : "moon"));
    btn.setAttribute("aria-label", mode === "dark" ? "เปลี่ยนเป็นธีมสว่าง" : "เปลี่ยนเป็นธีมมืด");
    const meta = $('meta[name="theme-color"]:not([media])');
    if (meta) meta.content = getComputedStyle(root).getPropertyValue("--bg").trim() || "#1f7a4d";
    if (onChange) onChange(mode);
  };
  if (saved === "light" || saved === "dark") root.dataset.theme = saved;
  btn.addEventListener("click", () => {
    root.dataset.theme = current() === "dark" ? "light" : "dark";
    paint();
  });
  media.addEventListener("change", () => { if (!root.dataset.theme) paint(); });
  paint();
  return current;
}
