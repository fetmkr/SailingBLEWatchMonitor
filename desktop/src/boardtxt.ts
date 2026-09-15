/**
 * 같은 세션의 TXT 사본에서 **보드가 그때 보여준 방위**를 읽는다.
 *
 * 왜 필요한가 (2026-09-15)
 *   2026-09-15 전 펌웨어의 HLG 에는 보드가 보여준 방위도, 계산에 쓴 설정(축·부호·장착각)도 없다.
 *   그래서 앱이 같은 공식을 갖고도 방위를 못 그렸다. TXT 에는 보드가 계산한 방위가 10초마다 있다.
 *
 * 하는 일
 *   1. TXT 줄마다 보드 방위를 꺼낸다 → 화면에 "보드 기록 HDG" 로 그대로 보여준다.
 *   2. 그 값을 기준으로 당시 설정을 되찾는다 → HLG 자력으로 촘촘히 재계산. "추정 설정으로 재계산" 표시.
 *      ★ COG 는 안 쓴다. COG 에 맞추면 요트의 실제 HDG−COG 차이(leeway·조류)까지 지워진다.
 *
 * TXT 줄을 HLG 어느 줄에 붙이나 — 세션 46 으로 셋을 대 보고 골랐다 (당시 설정 +Y +Z 155.6°).
 *     TXT 끝의 NAV 줄 번호        보드 방위와 차이 중앙 0.49° · 1° 안 65.4%   ← 씀
 *     첫 NAV 시각 + 경과 초        중앙 1.57° · 1° 안 35.5%
 *     첫 IMU 시각 + 경과 초        중앙 1.60° · 1° 안 34.7%
 *   경과 초는 정수로 잘려 있고 기록 시작 시각(gStartedMs)이 HLG 에 없어서 최대 1초 어긋난다.
 *   ★ HLG 에서 NAV 줄이 빠지면(CRC 틀림) 번호가 밀린다. 그래서 구간마다 차이를 따로 보여준다 —
 *     밀렸으면 뒤쪽 구간 차이가 커진다.
 */

import type { Session } from "./hlog";
import * as heading from "./heading";

export interface TxtRow {
  /** 기록 시작부터 초 (TXT 맨 앞 시각) */
  sec: number;
  /** 그 순간까지 HLG 에 넣은 NAV 줄 수 (TXT 끝 "NAV%u") */
  navCount: number;
  /** 보드가 보여준 방위. "방위---" 면 null */
  hdgDeg: number | null;
}

export interface BoardTxt {
  session: number | null;
  /** TXT 방위를 만든 보드 식. heading.FORMULA_FLAT / FORMULA_TILT */
  formula: number;
  /** 식을 어떻게 알았나 — 화면에 그대로 */
  formulaWhy: string;
  rows: TxtRow[];
}

const ROW = /^(\d\d):(\d\d):(\d\d)\s.*?방위\s*(---|\d+).*?\|\s*NAV(\d+)/;

export function parseTxt(text: string): BoardTxt {
  const sess = text.match(/^#.*세션\s+(\d+)/m);
  // 2026-09-15 펌웨어부터 TXT 머리에 "# 방위(화면·BLE·TXT): 기울기 보정…" 줄을 쓴다 (main.cpp buildHeadingNote).
  // 그 전 펌웨어는 화면·BLE·TXT 모두 평평 식이었다 (main.cpp boatHeadingDeg 주석 "2026-09-15 까지 … 평평 식").
  const tilt = /^# 방위\(화면·BLE·TXT\): 기울기 보정/m.test(text);
  const rows: TxtRow[] = [];
  for (const line of text.split("\n")) {
    const m = line.match(ROW);
    if (!m) continue;
    rows.push({
      sec: +m[1] * 3600 + +m[2] * 60 + +m[3],
      navCount: +m[5],
      hdgDeg: m[4] === "---" ? null : +m[4],
    });
  }
  return {
    session: sess ? +sess[1] : null,
    formula: tilt ? heading.FORMULA_TILT : heading.FORMULA_FLAT,
    formulaWhy: tilt
      ? "TXT 머리에 적힌 식: 기울기 보정"
      : "TXT 머리에 방위 식 줄이 없음 → 2026-09-15 전 펌웨어 → 평평 식 atan2 (편각 없음)",
    rows,
  };
}

/** TXT 방위를 HLG 시각에 붙인 점들. xs 는 세션 시작(t0)부터 ms. */
export interface BoardPoints {
  xs: Float64Array;
  ys: Float32Array;
  /** 붙은 NAV 줄 번호 (0부터) */
  nav: Int32Array;
  /** HLG 에 그 번호 줄이 없어서 못 붙인 TXT 줄 */
  outOfRange: number;
  /** 보드가 "방위---" 로 적은 줄 */
  noHdg: number;
}

export function placeRows(t: BoardTxt, s: Session, t0: number): BoardPoints {
  const xs: number[] = [], ys: number[] = [], nav: number[] = [];
  let outOfRange = 0, noHdg = 0;
  for (const r of t.rows) {
    if (r.hdgDeg === null) { noHdg++; continue; }
    const i = r.navCount - 1;
    if (i < 0 || i >= s.nav.length) { outOfRange++; continue; }
    xs.push(s.nav[i].ms - t0);
    ys.push(r.hdgDeg);
    nav.push(i);
  }
  return {
    xs: Float64Array.from(xs), ys: Float32Array.from(ys), nav: Int32Array.from(nav),
    outOfRange, noHdg,
  };
}

const wrap180 = (x: number) => ((x % 360) + 540) % 360 - 180;

export interface Segment { fromMs: number; toMs: number; n: number; medianDeg: number; p90Deg: number }

export interface Fit {
  cfg: heading.HeadingCfg;
  formula: number;
  /** 대 본 점 수 */
  n: number;
  medianDeg: number;
  p90Deg: number;
  segments: Segment[];
  /** 같은 방위를 내는 다른 축·부호 설정 수 (평평 식은 네 가지가 늘 같은 값을 낸다) */
  sameOutput: number;
  /** 그다음으로 잘 맞은 다른 설정의 차이 중앙값 — 이게 가까우면 설정을 가를 수 없다 */
  runnerUpMedianDeg: number;
}

function quantile(sorted: number[], p: number): number {
  return sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] : NaN;
}

/**
 * 보드 방위에 맞는 설정을 찾는다. 축 둘 × 부호 넷 = 24가지, 장착각은 원형 평균 하나.
 *
 * ★ 평평 식만 한다. 옛 TXT 는 평평 식이었다. 평평 식에서는 설정 네 가지가 똑같은 방위를 내서
 *   (축을 바꾸고 부호를 뒤집으면 장착각이 90° 씩 옮겨 갈 뿐) 그중 부호가 + 인 것을 고른다.
 *   그 네 가지는 기울기 보정 식에서는 서로 다른 값을 낸다 — 그래서 되찾은 설정으로 기울기 보정은 안 한다.
 * ★ 편각은 0. 당시 펌웨어(3d05d30)에 편각 설정이 없었다.
 */
export function fitFlat(s: Session, p: BoardPoints, segments = 6): Fit | null {
  const idx: number[] = [];
  for (let k = 0; k < p.nav.length; k++) {
    const m = s.nav[p.nav[k]].mag;
    if (m[0] || m[1] || m[2]) idx.push(k);      // 자력 0 은 보드가 새 표본이 없어 비운 줄
  }
  if (idx.length < 30) return null;

  type Cand = { cfg: heading.HeadingCfg; off: number; resid: number[]; median: number };
  const cands: Cand[] = [];
  for (let a = 0; a < 3; a++) for (let b = 0; b < 3; b++) {
    if (a === b) continue;
    for (const sa of [1, -1]) for (const sb of [1, -1]) {
      const cfg = { axisA: a, axisB: b, signA: sa, signB: sb, offDeg: 0, declDeg: 0 };
      const d = idx.map((k) => wrap180(p.ys[k] - heading.flatHeadingDeg(s.nav[p.nav[k]].mag, cfg)));
      let C = 0, S = 0;
      for (const x of d) { C += Math.cos(x * Math.PI / 180); S += Math.sin(x * Math.PI / 180); }
      const off = Math.atan2(S, C) * 180 / Math.PI;
      const resid = d.map((x) => Math.abs(wrap180(x - off)));
      const sorted = [...resid].sort((u, v) => u - v);
      cands.push({ cfg: { ...cfg, offDeg: off }, off, resid, median: quantile(sorted, 0.5) });
    }
  }
  // 차이가 같은 것끼리는 부호 + 를 먼저, 그다음 축 번호가 작은 것
  cands.sort((u, v) =>
    u.median - v.median ||
    (v.cfg.signA + v.cfg.signB) - (u.cfg.signA + u.cfg.signB) ||
    u.cfg.axisA - v.cfg.axisA || u.cfg.axisB - v.cfg.axisB);
  const best = cands[0];
  const same = cands.filter((c) => Math.abs(c.median - best.median) < 0.01).length;
  const runnerUp = cands.find((c) => c.median - best.median >= 0.01);

  const all = [...best.resid].sort((u, v) => u - v);
  const t0 = p.xs[idx[0]], t1 = p.xs[idx[idx.length - 1]];
  const span = Math.max(1, t1 - t0);
  const segs: Segment[] = [];
  for (let g = 0; g < segments; g++) {
    const from = t0 + span * g / segments, to = t0 + span * (g + 1) / segments;
    const r: number[] = [];
    idx.forEach((k, j) => {
      const x = p.xs[k];
      if (x >= from && (x < to || (g === segments - 1 && x <= to))) r.push(best.resid[j]);
    });
    r.sort((u, v) => u - v);
    segs.push({ fromMs: from, toMs: to, n: r.length, medianDeg: quantile(r, 0.5), p90Deg: quantile(r, 0.9) });
  }
  return {
    cfg: best.cfg,
    formula: heading.FORMULA_FLAT,
    n: idx.length,
    medianDeg: quantile(all, 0.5),
    p90Deg: quantile(all, 0.9),
    segments: segs,
    sameOutput: same,
    runnerUpMedianDeg: runnerUp ? runnerUp.median : NaN,
  };
}
