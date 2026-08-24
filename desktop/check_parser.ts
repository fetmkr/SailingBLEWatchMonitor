// TS 파서가 파이썬 파서와 같은 답을 내는지 본다.
// 규격 사본이 둘(파이썬·TS)이라 어긋나면 여기서 잡힌다.
import { readFileSync } from "node:fs";
import * as hlog from "./src/hlog";

const path = process.argv[2];
const buf = new Uint8Array(readFileSync(path));
const t0 = performance.now();
const s = hlog.parse(buf);
const ms = performance.now() - t0;
const c = hlog.check(s);

console.log(`${path}  ${buf.length.toLocaleString()} 바이트  ${ms.toFixed(0)} ms`);
console.log(`  머리글 CRC   ${s.header.crcOk ? "맞음" : "★틀림"}`);
console.log(`  세션 ${s.header.session}  모듈 ${s.header.module}  닫힘 ${s.header.closed}`);
console.log(`  첫 fix UTC   ${s.header.utcStart}`);
console.log(`  NAV ${s.nav.length.toLocaleString()}  IMU ${s.imu.length.toLocaleString()}`);
console.log(`  재동기 ${s.resyncs}  못 읽은 바이트 ${s.lostBytes}`);
console.log(`  NAV ${c.navHz?.toFixed(2)} Hz   IMU ${c.imuHz?.toFixed(2)} Hz`);
console.log(`  IMU 등간격   ${c.evenPct?.toFixed(2)}%`);
// ★ Math.max(...arr) 는 28만 개에서 스택이 터진다. 돌면서 센다.
let mx = -Infinity, sum = 0, cnt = 0;
for (const r of s.nav) { if (r.sogKn === null) continue; if (r.sogKn > mx) mx = r.sogKn; sum += r.sogKn; cnt++; }
console.log(`  속도  최대 ${mx.toFixed(2)} kn  평균 ${(sum/cnt).toFixed(2)} kn`);
console.log(`  마킹  ${s.nav.filter(r => r.event & 1).length}회`);
console.log(c.ok ? "  ✅ 깨끗합니다" : "  ❌ " + c.problems.join(" / "));
