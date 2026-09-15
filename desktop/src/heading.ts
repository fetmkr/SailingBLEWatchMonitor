/**
 * 배의 방위 — 보드와 **같은 식**. 원본은 firmware-rak/include/heading_tilt.h 다.
 *
 * ★ 2026-09-15 전에는 앱이 보드와 다른 식(atan2(magY, magX))을 쓰고, 자력계 치우침을
 *   스스로 구해 "좋아지면" 뺐다. 세션 46 에서 COG 대비 흩어짐이 49.5° 였다.
 *   사용자: "과학적인 데이터 분석용 앱인데 맘대로 공식을 적용해?"
 *
 * 규칙
 *   - 식과 입력(축·부호·오프셋·편각)은 HLG 머리글에서 읽는다. 앱이 고르지 않는다.
 *   - 머리글에 설정이 없는 옛 파일은 방위를 안 그린다. 짐작으로 채우지 않는다.
 *   - 두 구현이 같은지는 tools/verify.sh 가 벡터로 맞춰 본다 (desktop/tools/heading_check.ts).
 *
 * 못 구하면 NaN (보드는 -1).
 */

export interface HeadingCfg {
  axisA: number;   // atan2 첫 인자 축 (0=X 1=Y 2=Z, 자력계 좌표)
  axisB: number;
  signA: number;   // +1 / -1
  signB: number;
  offDeg: number;  // 장착 오프셋
  declDeg: number; // 자기 편각 (동편 +)
}

export const FORMULA_NONE = 0;
export const FORMULA_FLAT = 1;
export const FORMULA_TILT = 2;
export const FORMULA_TILT_VISIBLE = 3;

function wrap360(deg: number): number {
  if (!Number.isFinite(deg)) return NaN;
  let r = deg % 360;
  if (r < 0) r += 360;
  if (r >= 360) r -= 360;
  return r;
}

function downSign(c: HeadingCfg): number {
  if (c.axisA > 2 || c.axisB > 2 || c.axisA === c.axisB) return 0;
  const f = c.axisB, r = c.axisA;
  const even = (f === 0 && r === 1) || (f === 1 && r === 2) || (f === 2 && r === 0);
  return (even ? 1 : -1) * c.signB * -c.signA;
}

function toFRD(v: readonly number[], c: HeadingCfg): [number, number, number] | null {
  const ds = downSign(c);
  if (ds === 0) return null;
  return [v[c.axisB] * c.signB, -v[c.axisA] * c.signA, v[3 - c.axisA - c.axisB] * ds];
}

/** 가속(가속도계 좌표, g)으로 힐·피치 (라디안). 1 g ±0.15 밖이면 null. */
function gravityRollPitch(acc: readonly number[], c: HeadingCfg, requireGravity = true): [number, number] | null {
  const a = [acc[1], acc[0], -acc[2]];           // 자력 좌표로 (자력 X=가속 Y, Y=가속 X, Z=−가속 Z)
  const frd = toFRD(a, c);
  if (!frd) return null;
  const gf = -frd[0], gr = -frd[1], gd = -frd[2];
  const gn = Math.hypot(gf, gr, gd);
  if (!Number.isFinite(gn) || gn <= 0) return null;
  if (requireGravity && Math.abs(gn - 1) > 0.15) return null;
  return [Math.atan2(gr, gd), Math.atan2(-gf, Math.hypot(gr, gd))];
}

/** 기울기 보정 방위 (도). 식 번호 2. */
export function tiltHeadingDeg(acc: readonly number[], mag: readonly number[], c: HeadingCfg, requireGravity = true): number {
  const rp = gravityRollPitch(acc, c, requireGravity);
  if (!rp) return NaN;
  const m = toFRD(mag, c);
  if (!m) return NaN;
  const [roll, pitch] = rp;
  const cr = Math.cos(roll), sr = Math.sin(roll);
  const ct = Math.cos(pitch), st = Math.sin(pitch);
  const ty = cr * m[1] - sr * m[2];
  const tz = sr * m[1] + cr * m[2];
  const hx = ct * m[0] + st * tz;
  return wrap360(Math.atan2(-ty, hx) * 180 / Math.PI + c.offDeg + c.declDeg);
}

/** 평평 방위 (도). 식 번호 1 — 보드 진단용 식. */
export function flatHeadingDeg(mag: readonly number[], c: HeadingCfg): number {
  if (c.axisA > 2 || c.axisB > 2 || c.axisA === c.axisB) return NaN;
  return wrap360(Math.atan2(mag[c.axisA] * c.signA, mag[c.axisB] * c.signB) * 180 / Math.PI +
                 c.offDeg + c.declDeg);
}

/** 사람이 읽는 식 설명. 화면에 그대로 보여준다 — 무엇으로 계산했는지 숨기지 않는다. */
export function describe(h: {
  hdgFormula: number; hdgAxisA: number; hdgAxisB: number; hdgSignA: number; hdgSignB: number;
  hdgOffDeg: number; hdgDeclDeg: number; magHardIron: [number, number, number];
}): string {
  if (h.hdgFormula === FORMULA_NONE) {
    return "방위: 이 파일에는 보드의 방위 설정이 없습니다 (2026-09-15 전 펌웨어). 설정을 짐작해서 그리지 않습니다 — 같은 세션 TXT 를 붙이면 보드 기록 HDG 와 추정 설정으로 재계산을 봅니다.";
  }
  const ax = (a: number, s: number) => `${s < 0 ? "−" : "+"}${"XYZ"[a] ?? "?"}`;
  const sgn = (v: number) => `${v >= 0 ? "+" : "−"}${Math.abs(v).toFixed(1)}`;
  const kind = h.hdgFormula === FORMULA_TILT_VISIBLE
    ? "기울기 보정 (INSLIB ahrs_mag_detilt · 운동 가속에도 계산 · 1 g ±0.15 밖이면 보드 OLED에 ?)"
    : h.hdgFormula === FORMULA_TILT
    ? "기울기 보정 (INSLIB ahrs_mag_detilt · 중력은 그때 가속도 · 1 g ±0.15 밖이면 없음)"
    : h.hdgFormula === FORMULA_FLAT ? "평평 atan2 (기울기 보정 없음)" : `알 수 없는 식 번호 ${h.hdgFormula}`;
  const hi = h.magHardIron.map((v) => v.toFixed(1)).join(", ");
  return `방위: 보드와 같은 식 — ${kind} · 축 atan2(${ax(h.hdgAxisA, h.hdgSignA)}, ${ax(h.hdgAxisB, h.hdgSignB)}) · ` +
         `장착 오프셋 ${sgn(h.hdgOffDeg)}° · 편각 ${sgn(h.hdgDeclDeg)}° · 기록된 자력은 하드아이언 (${hi}) µT 를 뺀 값`;
}
