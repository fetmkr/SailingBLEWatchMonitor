// 방위 식 교차 검증 — 보드(C++)가 낸 벡터를 앱 식(heading.ts)에 넣어 맞춰 본다.
//   tools/verify.sh 가 esbuild 로 묶어 node 로 돌린다.
import { readFileSync } from "node:fs";
import { flatHeadingDeg, tiltHeadingDeg } from "../src/heading";

const path = process.argv[2];
if (!path) { console.error("벡터 파일을 주세요"); process.exit(2); }

const TOL = 2e-3;   // 도. 보드는 float32, 앱은 double
const diff = (a: number, b: number) => { const d = Math.abs(a - b) % 360; return d > 180 ? 360 - d : d; };
const same = (board: number, app: number) =>
  board < 0 ? Number.isNaN(app) : (!Number.isNaN(app) && diff(board, app) <= TOL);

let n = 0, bad = 0, invalid = 0;
for (const line of readFileSync(path, "utf8").split("\n")) {
  if (!line || line.startsWith("#")) continue;
  const v = line.trim().split(/\s+/).map(Number);
  const c = { axisA: v[0], axisB: v[1], signA: v[2], signB: v[3], offDeg: v[4], declDeg: v[5] };
  const acc = [v[6], v[7], v[8]], mag = [v[9], v[10], v[11]];
  const t = tiltHeadingDeg(acc, mag, c), f = flatHeadingDeg(mag, c);
  const visible = tiltHeadingDeg(acc, mag, c, false);
  if (v[12] < 0) invalid++;
  if (!same(v[12], t) || !same(v[13], f) || (v.length > 14 && !same(v[14], visible))) {
    if (bad < 5) console.error(`어긋남: ${line}  → 앱 tilt ${t} flat ${f}`);
    bad++;
  }
  n++;
}
console.log(`방위 벡터 ${n}줄 (보드가 '없음' 낸 줄 ${invalid}) — 어긋남 ${bad}`);
process.exit(bad === 0 && n > 0 ? 0 : 1);
