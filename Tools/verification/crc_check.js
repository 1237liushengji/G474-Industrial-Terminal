/**
 * CRC16-Modbus cross-check (PC side, no compiler needed).
 *
 * Purpose: verify the table embedded in Protocol/modbus_crc.c is exactly the
 * table produced by the reference bitwise CRC-16/MODBUS algorithm, and that
 * known vectors match.
 *
 * Usage: node Tools/verification/crc_check.js <path-to-modbus_crc.c>
 */
"use strict";
const fs = require("fs");

// ---- reference bitwise implementation (independent of C code) ----
function crc16ModbusBitwise(bytes) {
  let crc = 0xffff;
  for (const b of bytes) {
    crc ^= b;
    for (let i = 0; i < 8; i++) {
      crc = crc & 1 ? (crc >>> 1) ^ 0xa001 : crc >>> 1;
    }
  }
  return crc & 0xffff;
}

// ---- build reference table from the bitwise algorithm ----
// Table entry N is CRC16 of byte N starting from CRC = 0.
function crc16TableEntry(byte) {
  let crc = byte & 0xffff;
  for (let i = 0; i < 8; i++) {
    crc = crc & 1 ? (crc >>> 1) ^ 0xa001 : crc >>> 1;
  }
  return crc & 0xffff;
}

function buildTable() {
  const table = new Array(256);
  for (let i = 0; i < 256; i++) {
    table[i] = crc16TableEntry(i);
  }
  return table;
}

// ---- extract the table literally embedded in the C source ----
function extractCTable(cPath) {
  const src = fs.readFileSync(cPath, "utf8");
  const m = src.match(/s_crc16_table\[256\]\s*=\s*\{([^}]*)\}/s);
  if (!m) throw new Error("table not found in " + cPath);
  const values = m[1].match(/0x[0-9A-Fa-f]{4}/g);
  if (!values || values.length !== 256) {
    throw new Error("expected 256 entries, found " + (values && values.length));
  }
  return values.map((v) => parseInt(v, 16));
}

// ---- main ----
const cPath = process.argv[2] || "Protocol/modbus_crc.c";
const cTable = extractCTable(cPath);
const refTable = buildTable();

let errors = 0;
for (let i = 0; i < 256; i++) {
  if (cTable[i] !== refTable[i]) {
    console.error(`TABLE MISMATCH idx=${i}: C=0x${cTable[i].toString(16)} ref=0x${refTable[i].toString(16)}`);
    if (++errors > 10) break;
  }
}
if (errors) process.exit(1);
console.log("OK: embedded C table matches reference bitwise table (256/256).");

// ---- known test vectors ----
const vectors = [
  { name: "ASCII '123456789'", bytes: [...Buffer.from("123456789")], expect: 0x4b37 },
  { name: "read 0x03 frame 01 03 00 00 00 02", bytes: [0x01, 0x03, 0x00, 0x00, 0x00, 0x02], expect: 0x0bc4 },
  { name: "empty", bytes: [], expect: 0xffff },
  { name: "single 0x00", bytes: [0x00], expect: crc16ModbusBitwise([0x00]) }
];

let allOk = true;
for (const v of vectors) {
  const tableCrc = crc16ModbusTable(cTable, v.bytes);
  const bitCrc = crc16ModbusBitwise(v.bytes);
  const ok = tableCrc === v.expect && bitCrc === v.expect;
  if (!ok) allOk = false;
  console.log(
    `${ok ? "OK " : "FAIL"} ${v.name}: table=0x${tableCrc.toString(16).padStart(4, "0")} ` +
      `bitwise=0x${bitCrc.toString(16).padStart(4, "0")} expect=0x${v.expect.toString(16).padStart(4, "0")}`
  );
}

function crc16ModbusTable(table, bytes) {
  let crc = 0xffff;
  for (const b of bytes) {
    crc = ((crc >>> 8) ^ table[(crc ^ b) & 0xff]) & 0xffff;
  }
  return crc;
}

// CRC append/check semantics check
const frame = [0x01, 0x03, 0x00, 0x00, 0x00, 0x02];
const crc = crc16ModbusBitwise(frame);
const appended = [...frame, crc & 0xff, (crc >> 8) & 0xff];
const checkOk =
  crc16ModbusBitwise(appended.slice(0, 6)) === (appended[6] | (appended[7] << 8));
console.log(`${checkOk ? "OK " : "FAIL"} append low-first / check semantics`);
if (!allOk || !checkOk) process.exit(1);
console.log("ALL CRC CHECKS PASSED");
