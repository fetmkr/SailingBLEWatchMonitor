// 영상 시각 읽기와 싱크 짐작을 시험한다.
// 브라우저 탭이 숨김이면 크롬이 영상을 안 읽어서 화면으로는 못 본다.
import { readFileSync } from "node:fs";
import * as vid from "./src/video";

const path = process.argv[2];
const buf = readFileSync(path);
const blob = new Blob([buf]);

const t = await vid.mp4CreationTime(blob);
console.log(`${path}  ${(buf.length / 1048576).toFixed(2)} MB`);
console.log(`  파일이 적어 둔 시각   ${t ? t.toISOString() : "★ 못 읽음"}`);

// 세션은 2026-08-23T15:46:40Z 에 시작했고 첫 레코드가 local_ms 1000 이다
const SESSION_UTC = 1787500000, FIRST_MS = 1000;
const s = vid.guessOffset(t, SESSION_UTC, FIRST_MS);
console.log(`  짐작한 싱크           ${vid.formatOffset(s.offsetMs)}  (짐작=${s.guessed})`);
console.log(`  영상 0초 → 세션 ${(s.offsetMs / 1000).toFixed(1)}초`);
console.log(`  세션 60초 → 영상 ${vid.sessionToVideo(s, 60000).toFixed(1)}초`);

// ── 시간대를 잘못 적은 경우와 다른 날 영상 ──
const cases: [string, number][] = [
  ["시각이 딱 맞음",            SESSION_UTC * 1000 + 20000],
  ["9시간 뒤로 적힘 (KST)",     SESSION_UTC * 1000 + 20000 + 9 * 3600_000],
  ["9시간 앞으로 적힘",         SESSION_UTC * 1000 + 20000 - 9 * 3600_000],
  ["이틀 뒤 영상 (시간대 아님)", SESSION_UTC * 1000 + 48 * 3600_000],
  ["1시간 3분 뒤 (진짜 나중)",   SESSION_UTC * 1000 + 63 * 60_000],
];
console.log("\n  ── 시간대를 잘못 적은 경우 ──");
for (const [name, ms] of cases) {
  const g = vid.guessOffset(new Date(ms), SESSION_UTC, FIRST_MS);
  console.log(`  ${name.padEnd(26)} → ${vid.formatOffset(g.offsetMs)}`);
}
