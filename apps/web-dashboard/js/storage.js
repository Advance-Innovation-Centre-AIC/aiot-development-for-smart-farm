// storage.js - จดประวัติลงเบราว์เซอร์ (localStorage) และส่งออกเป็นไฟล์ CSV ที่เปิดใน Excel ได้
//
// ทำไมต้องจดเอง: บอร์ดส่งแบบไม่ retain (สัญญาข้อ 5.1) แอปที่เปิดทีหลังเห็นแค่ใบถัดไป
// ข้อจำกัด: localStorage อยู่ในเบราว์เซอร์เครื่องนี้เท่านั้น ไม่แชร์ข้ามเครื่อง และอาจถูกล้าง (โหมดส่วนตัว / ล้างข้อมูลเว็บ)
//           ทุกครั้งที่อ่าน/เขียนจึงห่อด้วย try/catch แอปต้องทำงานต่อได้แม้เก็บไม่ได้

import { HISTORY_MAX } from "./config.js";

const PREFIX = "sf-dash:";

export function load(name, fallback) {
  try {
    const text = localStorage.getItem(PREFIX + name);
    return text === null ? fallback : JSON.parse(text);
  } catch (e) {
    return fallback;
  }
}

export function save(name, value) {
  try {
    localStorage.setItem(PREFIX + name, JSON.stringify(value));
    return true;
  } catch (e) {
    return false;          // เต็ม หรือเบราว์เซอร์ไม่ให้เก็บ
  }
}

export function remove(name) {
  try { localStorage.removeItem(PREFIX + name); } catch (e) { /* ไม่เป็นไร */ }
}

// คอลัมน์ของ CSV : รวมคีย์จากทุกหัวข้อในสัญญา + คอลัมน์ json เก็บข้อความเต็ม
const CSV_COLUMNS = ["time", "kind", "topic", "n", "temp_c", "rh", "hpa", "az", "soil", "light", "tank",
  "pump", "auto", "by", "sim", "node", "value", "unit", "left_s", "why", "event", "crop", "level",
  "alert", "cmd", "on", "sec", "text", "t_hi", "json"];

// ประวัติของกลุ่มหนึ่งกลุ่ม: รายการ { t: เวลา ms, k: ชนิด, s: หัวข้อย่อย, d: ข้อมูล }
export class History {
  constructor(team) {
    this.team = team;
    this.key = "history:" + team;
    this.items = load(this.key, []);
    if (!Array.isArray(this.items)) this.items = [];
    this.timer = null;
  }

  add(kind, sub, data) {
    const item = { t: Date.now(), k: kind, s: sub, d: data };
    this.items.push(item);
    if (this.items.length > HISTORY_MAX) this.items.splice(0, this.items.length - HISTORY_MAX);
    this.saveSoon();
    return item;
  }

  // เขียนลงดิสก์ทุก 2 วิอย่างมาก ไม่ใช่ทุกข้อความ (เบราว์เซอร์มือถือจะได้ไม่หน่วง)
  saveSoon() {
    if (this.timer) return;
    this.timer = setTimeout(() => {
      this.timer = null;
      if (!save(this.key, this.items)) {
        this.items.splice(0, Math.floor(this.items.length / 2));   // เต็ม: ทิ้งครึ่งเก่าแล้วลองใหม่
        save(this.key, this.items);
      }
    }, 2000);
  }

  clear() {
    this.items = [];
    remove(this.key);
  }

  toCSV() {
    const rows = [CSV_COLUMNS.join(",")];
    for (const it of this.items) {
      const d = it.d && typeof it.d === "object" ? it.d : {};
      const row = CSV_COLUMNS.map((col) => {
        if (col === "time") return csvCell(localTime(it.t));
        if (col === "kind") return csvCell(it.k);
        if (col === "topic") return csvCell(it.s);
        if (col === "json") return csvCell(JSON.stringify(d));
        return csvCell(d[col]);
      });
      rows.push(row.join(","));
    }
    // BOM (﻿) นำหน้า ให้ Excel อ่านภาษาไทยเป็น UTF-8 ไม่เพี้ยน · \r\n ตามมาตรฐาน CSV
    return "﻿" + rows.join("\r\n") + "\r\n";
  }

  download() {
    const blob = new Blob([this.toCSV()], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    const stamp = localTime(Date.now()).replace(/[: ]/g, "-");
    a.href = url;
    a.download = "farm_" + this.team + "_" + stamp + ".csv";
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 5000);
    return a.download;
  }
}

// ข้อความจาก broker สาธารณะเป็นของใครก็ได้: ข้อความที่ขึ้นต้นด้วย = + - @ จะถูก Excel ตีความเป็นสูตร
// จึงเติม ' ข้างหน้า (ตัวเลขจริง เช่น -9.8 ไม่โดน เพราะเป็น number ไม่ใช่ข้อความ)
function csvCell(v) {
  if (v === undefined || v === null) return "";
  if (typeof v === "number" || typeof v === "boolean") return String(v);
  let s = String(v).slice(0, 500);
  if (/^[=+\-@\t\r]/.test(s)) s = "'" + s;
  return /[",\r\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
}

export function localTime(ms) {
  const d = new Date(ms);
  const p = (x) => String(x).padStart(2, "0");
  return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + " " +
    p(d.getHours()) + ":" + p(d.getMinutes()) + ":" + p(d.getSeconds());
}
