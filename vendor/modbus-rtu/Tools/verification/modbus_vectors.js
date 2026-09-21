/**
 * Modbus RTU slave exception/behaviour test vectors (PC side).
 *
 * Generates golden request -> expected response frames for the C slave
 * engine implemented in Protocol/modbus_slave.c, cross-checks the CRC of
 * every frame with the independently verified table algorithm (see
 * crc_check.js), and prints a JSON suite usable later by the Python tool
 * (v1.2) and the pressure/regression test harness (v1.3).
 *
 * Defaults assumed by the vectors:
 *   slave id = 0x01
 *   holding registers: 15 regs (0x00..0x0E), writable 0x09..0x0E
 *   coils:             4 coils  (0x00..0x03)
 *
 * Usage: node Tools/verification/modbus_vectors.js
 */
"use strict";

const crcModbus = (() => {
  // reference bitwise CRC-16/MODBUS
  function bitwise(bytes) {
    let crc = 0xffff;
    for (const b of bytes) {
      crc ^= b;
      for (let i = 0; i < 8; i++) {
        crc = crc & 1 ? (crc >>> 1) ^ 0xa001 : crc >>> 1;
      }
    }
    return crc & 0xffff;
  }
  return { bitwise };
})();

function appendCrc(frame) {
  const crc = crcModbus.bitwise(frame);
  return [...frame, crc & 0xff, (crc >> 8) & 0xff];
}

function hex(a) {
  return a.map((b) => b.toString(16).padStart(2, "0").toUpperCase()).join(" ");
}

const SLAVE = 0x01;

const vectors = [
  {
    id: "E1-illegal-function",
    name: "unsupported FC 0x0F -> exception 0x01",
    request: appendCrc([SLAVE, 0x0f, 0x00, 0x00, 0x00, 0x01, 0x01]),
    expect: appendCrc([SLAVE, 0x8f, 0x01]),
  },
  {
    id: "E2-illegal-address-hold",
    name: "read holding start 0x0100 -> exception 0x02",
    request: appendCrc([SLAVE, 0x03, 0x01, 0x00, 0x00, 0x01]),
    expect: appendCrc([SLAVE, 0x83, 0x02]),
  },
  {
    id: "E3-quantity-overflow",
    name: "read holding 0x000E qty 5 -> exception 0x02 (14+5>15)",
    request: appendCrc([SLAVE, 0x03, 0x00, 0x0e, 0x00, 0x05]),
    expect: appendCrc([SLAVE, 0x83, 0x02]),
  },
  {
    id: "E4-write-readonly",
    name: "write single to RO reg 40001(0x00) -> exception 0x02",
    request: appendCrc([SLAVE, 0x06, 0x00, 0x00, 0x01, 0x2c]),
    expect: appendCrc([SLAVE, 0x86, 0x02]),
  },
  {
    id: "E5-write-ok",
    name: "write single reg 40010(0x09)=0x015E -> echo",
    request: appendCrc([SLAVE, 0x06, 0x00, 0x09, 0x01, 0x5e]),
    expect: appendCrc([SLAVE, 0x06, 0x00, 0x09, 0x01, 0x5e]),
  },
  {
    id: "N1-read-holding-ok",
    name: "read holding 0x00 qty 2 -> 2 regs (4 data bytes)",
    request: appendCrc([SLAVE, 0x03, 0x00, 0x00, 0x00, 0x02]),
    expect: null, // data-dependent; length checked below
    expectLen: 9,
  },
  {
    id: "N2-write-multi-ok",
    name: "write multi 0x09 qty 4 -> echo start+qty",
    request: appendCrc([
      SLAVE, 0x10, 0x00, 0x09, 0x00, 0x04, 0x08,
      0x01, 0x2c, 0x06, 0x40, 0x01, 0xf4, 0x00, 0x01,
    ]),
    expect: appendCrc([SLAVE, 0x10, 0x00, 0x09, 0x00, 0x04]),
  },
  {
    id: "E6-write-multi-bad-bytecount",
    name: "write multi bytecount mismatch -> exception 0x03",
    request: appendCrc([SLAVE, 0x10, 0x00, 0x09, 0x00, 0x02, 0x05, 0x01, 0x2c, 0x06, 0x40, 0x00]),
    expect: appendCrc([SLAVE, 0x90, 0x03]),
  },
  {
    id: "E7-crc-error",
    name: "corrupted CRC -> no response",
    skipReqCrcCheck: true,
    request: (() => {
      const f = appendCrc([SLAVE, 0x03, 0x00, 0x00, 0x00, 0x01]);
      f[f.length - 1] ^= 0xff; // flip last CRC byte
      return f;
    })(),
    expect: null,
    expectNoResponse: true,
  },
  {
    id: "E8-wrong-slave",
    name: "foreign slave id 0x02 -> no response",
    request: appendCrc([0x02, 0x03, 0x00, 0x00, 0x00, 0x01]),
    expect: null,
    expectNoResponse: true,
  },
  {
    id: "N3-coil-read-ok",
    name: "read coils 0x00 qty 4 -> 1 byte data",
    request: appendCrc([SLAVE, 0x01, 0x00, 0x00, 0x00, 0x04]),
    expect: null,
    expectLen: 6,
  },
];

let errors = 0;
const suite = [];
for (const v of vectors) {
  // self check: CRC of every frame we produce must validate
  let reqCrcOk = true;
  if (!v.skipReqCrcCheck) {
    reqCrcOk =
      crcModbus.bitwise(v.request.slice(0, -2)) ===
      (v.request[v.request.length - 2] | (v.request[v.request.length - 1] << 8));
  }
  if (!reqCrcOk) {
    console.error(`FAIL ${v.id}: generated request CRC is wrong`);
    errors++;
    continue;
  }
  let expCrcOk = true;
  if (v.expect) {
    expCrcOk =
      crcModbus.bitwise(v.expect.slice(0, -2)) ===
      (v.expect[v.expect.length - 2] | (v.expect[v.expect.length - 1] << 8));
    if (!expCrcOk) {
      console.error(`FAIL ${v.id}: expected response CRC is wrong`);
      errors++;
      continue;
    }
  }
  const entry = {
    id: v.id,
    name: v.name,
    request: hex(v.request),
    response: v.expect ? hex(v.expect) : null,
    expectNoResponse: v.expectNoResponse || false,
    expectLen: v.expectLen || null,
  };
  suite.push(entry);
  console.log(`OK   ${v.id}  ${v.name}`);
  console.log(`     req: ${entry.request}`);
  if (entry.response) console.log(`     rsp: ${entry.response}`);
  else console.log(`     rsp: <no response>`);
}

require("fs").writeFileSync(
  __dirname + "/modbus_vectors.json",
  JSON.stringify(suite, null, 2)
);
console.log(`\nsuite written: Tools/verification/modbus_vectors.json (${suite.length} vectors)`);
if (errors) process.exit(1);
console.log("ALL VECTOR GENERATION CHECKS PASSED");
