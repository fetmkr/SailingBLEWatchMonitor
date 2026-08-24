// 타임라인 그리기.
//
// 20분 세션이면 IMU 가 12만 점이다. 8시간이면 288만 점이다. 그걸 그대로
// 캔버스에 그리면 못 쓴다. 그래서 **화면 가로 픽셀 수만큼만 줄여서** 그린다.
//
//   가로 1200 픽셀  →  구간 1200개
//   각 구간마다 최소·최대만 남긴다  →  세로줄 하나
//
// 최소·최대를 남기는 게 중요하다. 평균만 남기면 파도로 튄 봉우리가 사라진다.
// 그 봉우리가 우리가 100 Hz 로 기록한 이유다.

export interface Series {
  /** 이름을 남길 때 쓰는 열쇠. 화면에는 안 나온다 ("SOG", "HEEL") */
  code: string;
  /** 사람이 고칠 수 있는 이름. 왼쪽 칸에 크게 나온다 */
  name: string;
  unit: string;
  color: string;
  /** x = 세션 시작으로부터 밀리초 */
  xs: Float64Array;
  ys: Float32Array;
  /** 세로축을 0 을 가운데 두고 그릴지 (힐·자이로처럼 부호가 있는 값) */
  zeroCentered?: boolean;
}

export interface View {
  /** 보이는 구간 (ms) */
  from: number;
  to: number;
}

interface Band {
  min: Float32Array;
  max: Float32Array;
  has: Uint8Array;
}

/** 한 구간(픽셀)마다 최소·최대를 뽑는다. 봉우리를 잃지 않으려는 것이다. */
function bucketize(s: Series, view: View, cols: number): Band {
  const min = new Float32Array(cols).fill(Infinity);
  const max = new Float32Array(cols).fill(-Infinity);
  const has = new Uint8Array(cols);
  const span = view.to - view.from;
  if (span <= 0) return { min, max, has };

  // xs 는 오름차순이라 이분탐색으로 시작점을 찾는다.
  let lo = 0, hi = s.xs.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (s.xs[mid] < view.from) lo = mid + 1;
    else hi = mid;
  }

  for (let i = lo; i < s.xs.length; i++) {
    const x = s.xs[i];
    if (x > view.to) break;
    const c = Math.min(cols - 1, Math.floor(((x - view.from) / span) * cols));
    const y = s.ys[i];
    if (!Number.isFinite(y)) continue;      // 값 없음은 안 그린다
    if (y < min[c]) min[c] = y;
    if (y > max[c]) max[c] = y;
    has[c] = 1;
  }
  return { min, max, has };
}

function niceRange(lo: number, hi: number, zeroCentered?: boolean): [number, number] {
  if (!Number.isFinite(lo) || !Number.isFinite(hi)) return [0, 1];
  if (zeroCentered) {
    const m = Math.max(Math.abs(lo), Math.abs(hi)) || 1;
    return [-m * 1.1, m * 1.1];
  }
  if (hi - lo < 1e-9) return [lo - 0.5, hi + 0.5];
  const pad = (hi - lo) * 0.08;
  return [lo - pad, hi + pad];
}

export function formatDuration(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return h > 0
    ? `${h}:${String(m).padStart(2, "0")}:${String(sec).padStart(2, "0")}`
    : `${m}:${String(sec).padStart(2, "0")}`;
}

/**
 * 기준점에서 얼마나 떨어졌나. Saleae 처럼 단위를 같이 적는다.
 *
 *   +250 ms   +2.5 초   +1분 30초
 *
 * 단위를 안 적으면 "0:02" 가 2초인지 2분인지 헷갈린다. 크게 당겨서 볼수록
 * 그렇다.
 */
export function formatOffsetTick(ms: number): string {
  const sign = ms < 0 ? "−" : "+";
  const a = Math.abs(ms);
  if (a < 1000) return `${sign}${Math.round(a)} ms`;
  if (a < 10000) return `${sign}${(a / 1000).toFixed(a % 1000 ? 1 : 0)} 초`;
  if (a < 60000) return `${sign}${Math.round(a / 1000)} 초`;
  const m = Math.floor(a / 60000);
  const s = Math.round((a % 60000) / 1000);
  return s ? `${sign}${m}분 ${s}초` : `${sign}${m}분`;
}

/** 눈금 간격을 사람이 읽기 좋은 값으로 (1, 2, 5, 10, 15, 30초 …) */
function tickStep(spanMs: number, want: number): number {
  const steps = [
    100, 200, 500, 1000, 2000, 5000, 10000, 15000, 30000,
    60000, 120000, 300000, 600000, 900000, 1800000, 3600000,
  ];
  const target = spanMs / want;
  for (const s of steps) if (s >= target) return s;
  return 3600000;
}

export interface DrawOpts {
  canvas: HTMLCanvasElement;
  series: Series[];
  view: View;
  /** 마킹이 찍힌 시각 (ms) */
  marks: number[];
  /** 마우스가 있는 시각. 옅게 그린다. null 이면 안 그린다 */
  cursorMs: number | null;
  /** 눌러서 고정한 시각. 진하게 그린다 */
  pinMs: number | null;
  /** 전체 구간. 아래 띠를 그린다 */
  full?: View;
}

/**
 * 왼쪽 이름 칸. Saleae 처럼 값 그림 바깥에 둔다.
 *
 * 이름을 그림 안에 얹으면 값과 겹쳐서 둘 다 읽기 어렵다. 밖으로 빼면
 * 이름은 이름대로, 값은 값대로 읽힌다. 여기를 누르면 이름을 고친다.
 */
let LABEL_W = 148;
/** 세로축 숫자가 들어갈 폭 */
const AXIS_NUM_W = 62;
const axisW = () => LABEL_W + AXIS_NUM_W;

/** 이름 칸 너비. 경계를 끌어서 바꾼다 (이름이 길면 넘치기 때문이다) */
export function labelWidth(): number { return LABEL_W; }
export function setLabelWidth(px: number): void {
  LABEL_W = Math.min(360, Math.max(70, Math.round(px)));
}
/** 마우스가 이름 칸 경계에 있나 (끌어서 넓히는 자리) */
export function onLabelEdge(canvas: HTMLCanvasElement, clientX: number): boolean {
  const x = clientX - canvas.getBoundingClientRect().left;
  return Math.abs(x - LABEL_W) <= 4;
}

/** 칸을 넘치면 말줄임으로 자른다. 넘쳐서 잘리면 뭐가 잘렸는지 모른다. */
function fitText(g: CanvasRenderingContext2D, text: string, max: number): string {
  if (g.measureText(text).width <= max) return text;
  let lo = 0, hi = text.length;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (g.measureText(text.slice(0, mid) + "…").width <= max) lo = mid;
    else hi = mid - 1;
  }
  return text.slice(0, lo) + "…";
}
/** 시간 축. Saleae 처럼 **위**에 둔다 — 눈이 먼저 가는 자리다 */
const TIME_H = 26;
const GAP = 8;
/** 맨 아래 스크롤 막대. 전체 어디쯤인지만 알려 주면 된다 */
const OVER_H = 10;

export function draw(o: DrawOpts): void {
  const { canvas, series, view } = o;
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth;
  const cssH = canvas.clientHeight;
  if (canvas.width !== Math.round(cssW * dpr) || canvas.height !== Math.round(cssH * dpr)) {
    canvas.width = Math.round(cssW * dpr);
    canvas.height = Math.round(cssH * dpr);
  }
  const g = canvas.getContext("2d")!;
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, cssW, cssH);

  const plotW = Math.max(1, cssW - axisW() - 8);
  const rows = series.length || 1;
  const bodyTop = TIME_H;                       // 시간 축이 위에 있다
  const bodyH = cssH - TIME_H - OVER_H;
  const rowH = Math.max(24, (bodyH - GAP * (rows - 1)) / rows);
  const cols = Math.max(1, Math.floor(plotW));
  const span = Math.max(1, view.to - view.from);
  const xOf = (ms: number) => axisW() + ((ms - view.from) / span) * plotW;

  const css = getComputedStyle(document.documentElement);
  const ink = css.getPropertyValue("--ink").trim() || "#e6e6e6";
  const dim = css.getPropertyValue("--dim").trim() || "#8a8a8a";
  const grid = css.getPropertyValue("--grid").trim() || "#2a2a2a";

  // ── 시간 축 (위) ────────────────────────────────────────────────────
  //
  // Saleae 방식이다. 맨 왼쪽에 기준 시각을 굵게 적고, 그 뒤로는 거기서
  // 얼마나 떨어졌는지를 **단위와 함께** 적는다.
  //
  //   3:04    +10 초   +20 초   +30 초
  //
  // 단위가 없으면 크게 당겼을 때 "0:02" 가 2초인지 2분인지 헷갈린다.
  g.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
  g.textBaseline = "top";

  // 왼쪽 이름 칸 바탕
  g.fillStyle = "#181b21";
  g.fillRect(0, 0, LABEL_W, cssH);

  // 축 바탕
  g.fillStyle = "#181b21";
  g.fillRect(0, 0, cssW, TIME_H);
  g.strokeStyle = grid;
  g.lineWidth = 1;
  g.beginPath();
  g.moveTo(0, TIME_H - 0.5);
  g.lineTo(cssW, TIME_H - 0.5);
  g.stroke();

  const base = view.from;
  const step = tickStep(span, Math.max(3, Math.floor(plotW / 120)));
  const first = Math.ceil(base / step) * step;

  for (let t = first; t <= view.to; t += step) {
    const x = Math.round(xOf(t)) + 0.5;
    g.strokeStyle = grid;
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(x, bodyTop);
    g.lineTo(x, bodyTop + bodyH);
    g.stroke();
    // 축 안의 짧은 눈금
    g.beginPath();
    g.moveTo(x, TIME_H - 6);
    g.lineTo(x, TIME_H - 1);
    g.stroke();

    // 맨 왼쪽 눈금은 기준 시각과 겹친다. 기준이 이미 "0 부터" 라고 말한다.
    if (x > axisW() + 46) {
      g.fillStyle = dim;
      g.textAlign = "center";
      g.fillText(formatOffsetTick(t - base), x, 6);
    }
  }

  // 기준 시각. 맨 왼쪽에 굵게.
  g.fillStyle = ink;
  g.textAlign = "left";
  g.font = "bold 11px ui-monospace, SFMono-Regular, Menlo, monospace";
  g.fillText(formatDuration(base), LABEL_W + 4, 6);
  g.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";

  // ── 마킹 ────────────────────────────────────────────────────────────
  for (const m of o.marks) {
    if (m < view.from || m > view.to) continue;
    const x = Math.round(xOf(m)) + 0.5;
    g.strokeStyle = "#f0a020";
    g.lineWidth = 1.5;
    g.setLineDash([4, 3]);
    g.beginPath();
    g.moveTo(x, bodyTop);
    g.lineTo(x, bodyTop + bodyH);
    g.stroke();
    g.setLineDash([]);
  }

  // ── 값들 ────────────────────────────────────────────────────────────
  series.forEach((s, r) => {
    const top = bodyTop + r * (rowH + GAP);
    const bot = top + rowH;
    const band = bucketize(s, view, cols);

    let lo = Infinity, hi = -Infinity;
    for (let c = 0; c < cols; c++) {
      if (!band.has[c]) continue;
      if (band.min[c] < lo) lo = band.min[c];
      if (band.max[c] > hi) hi = band.max[c];
    }
    const [rlo, rhi] = niceRange(lo, hi, s.zeroCentered);
    const yOf = (v: number) => bot - ((v - rlo) / (rhi - rlo)) * rowH;

    // 0 선
    if (rlo < 0 && rhi > 0) {
      const y = Math.round(yOf(0)) + 0.5;
      g.strokeStyle = grid;
      g.beginPath();
      g.moveTo(axisW(), y);
      g.lineTo(cssW, y);
      g.stroke();
    }

    // 화면에 들어온 점이 몇 개인지 센다.
    let n = 0;
    for (let c = 0; c < cols; c++) if (band.has[c]) n++;

    g.strokeStyle = s.color;
    g.lineWidth = 1;

    if (n * 3 < cols) {
      // ── 아주 당겨서 점이 성길 때 ──
      //
      // 픽셀마다 세로줄을 그으면 점 하나가 점 하나로만 보여서 흐름을 못 읽는다.
      // 이럴 때는 실제 점을 이어 긋고 동그라미를 찍는다. 어디가 실제로 잰
      // 자리인지 눈에 보여야 한다 — 사이는 그냥 이어 놓은 선일 뿐이다.
      let first = true;
      g.beginPath();
      for (let i = 0; i < s.xs.length; i++) {
        const x = s.xs[i];
        if (x < view.from) continue;
        if (x > view.to) break;
        const y = s.ys[i];
        if (!Number.isFinite(y)) { first = true; continue; }
        const px = xOf(x), py = yOf(y);
        if (first) { g.moveTo(px, py); first = false; } else { g.lineTo(px, py); }
      }
      g.stroke();

      g.fillStyle = s.color;
      for (let i = 0; i < s.xs.length; i++) {
        const x = s.xs[i];
        if (x < view.from) continue;
        if (x > view.to) break;
        const y = s.ys[i];
        if (!Number.isFinite(y)) continue;
        g.beginPath();
        g.arc(xOf(x), yOf(y), 2, 0, Math.PI * 2);
        g.fill();
      }
    } else {
      // ── 보통 때 ── 픽셀마다 최소~최대를 세로줄로. 봉우리가 살아 있다.
      g.beginPath();
      for (let c = 0; c < cols; c++) {
        if (!band.has[c]) continue;
        const x = axisW() + c + 0.5;
        const y1 = yOf(band.max[c]);
        const y2 = yOf(band.min[c]);
        g.moveTo(x, y1);
        g.lineTo(x, Math.max(y2, y1 + 0.6));
      }
      g.stroke();
    }

    // ── 세로축 눈금 ──
    //
    // 단위는 **여기** 붙인다. 이름 칸에 한 줄 더 쓰면 세 줄이 되어 어지럽다.
    // Saleae 가 "5 V" 라고 축에 적는 것과 같다.
    g.fillStyle = dim;
    g.textAlign = "right";
    g.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
    g.fillText(`${rhi.toFixed(rhi >= 100 ? 0 : 1)} ${s.unit}`, axisW() - 6, top);
    g.fillText(`${rlo.toFixed(rlo <= -100 ? 0 : 1)} ${s.unit}`, axisW() - 6, bot - 12);

    // ── 왼쪽 이름 칸 — 한 줄이면 된다 ──
    //
    // 처음에 짧은 표시(SOG)와 이름(Speed Over Ground)을 나란히 뒀는데,
    // 같은 말을 두 번 쓰는 셈이었다. Saleae 의 D0 는 하드웨어 채널 번호라
    // 이름과 다른 것이지만 우리는 그게 아니다.
    //
    // 색은 이름 앞의 작은 네모로 준다. 그림의 선 색과 이어진다.
    const midY = top + rowH / 2;
    g.fillStyle = s.color;
    g.fillRect(10, midY - 9, 8, 8);

    g.fillStyle = ink;
    g.font = "13px -apple-system, system-ui, sans-serif";
    g.textAlign = "left";
    // 칸을 넘치면 잘라 준다. 그냥 넘치면 옆 칸 숫자와 겹쳐서 둘 다 못 읽는다.
    g.fillText(fitText(g, s.name, LABEL_W - 32), 24, midY - 9);

    // 줄 나눔선
    g.strokeStyle = grid;
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(0, Math.round(bot + GAP / 2) + 0.5);
    g.lineTo(cssW, Math.round(bot + GAP / 2) + 0.5);
    g.stroke();

    // 이름 칸과 그림 사이 경계
    g.beginPath();
    g.moveTo(LABEL_W + 0.5, top - GAP / 2);
    g.lineTo(LABEL_W + 0.5, bot + GAP / 2);
    g.stroke();
    g.beginPath();
    g.moveTo(axisW() + 0.5, top);
    g.lineTo(axisW() + 0.5, bot);
    g.stroke();
  });

  // ── 커서 ────────────────────────────────────────────────────────────
  //
  // 두 가지를 그린다.
  //   따라다니는 커서   마우스를 옮기면 같이 움직인다. 옅게
  //   고정한 커서       눌러서 박아 둔 자리. 진하게. 마우스를 떼도 남는다
  //
  // 영상과 맞춰 보려면 한 자리에 박아 두고 봐야 한다. 마우스를 뗄 때마다
  // 풀리면 화면을 볼 수가 없다.
  if (o.cursorMs !== null && o.cursorMs >= view.from && o.cursorMs <= view.to) {
    const x = Math.round(xOf(o.cursorMs)) + 0.5;
    g.strokeStyle = ink;
    g.globalAlpha = 0.35;
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(x, bodyTop);
    g.lineTo(x, bodyTop + bodyH);
    g.stroke();
    g.globalAlpha = 1;
  }

  if (o.pinMs !== null && o.pinMs >= view.from && o.pinMs <= view.to) {
    const x = Math.round(xOf(o.pinMs)) + 0.5;
    g.strokeStyle = "#4ea1ff";
    g.lineWidth = 1.5;
    g.beginPath();
    g.moveTo(x, bodyTop - TIME_H + 2);
    g.lineTo(x, bodyTop + bodyH);
    g.stroke();
    // 시간 축 안에 손잡이. 어느 것이 고정인지 한눈에 갈린다.
    g.fillStyle = "#4ea1ff";
    g.beginPath();
    g.moveTo(x - 5, 2);
    g.lineTo(x + 5, 2);
    g.lineTo(x, 11);
    g.closePath();
    g.fill();
  }

  // ── 맨 아래 스크롤 막대 ─────────────────────────────────────────────
  //
  // Saleae 처럼 파형은 다시 안 그린다. **손잡이 길이가 곧 배율**이다 —
  // 크게 당길수록 짧아지고, 전체를 보면 꽉 찬다. 그것만 알면 된다.
  // 같은 속도 그림을 아래에 또 깔면 눈만 어지럽다.
  if (o.full && o.full.to > o.full.from) {
    const oy = cssH - OVER_H + 3;
    const oh = OVER_H - 6;
    const fSpan = o.full.to - o.full.from;
    const fx = (ms: number) => axisW() + ((ms - o.full!.from) / fSpan) * plotW;

    g.fillStyle = "#1b1e24";
    g.fillRect(axisW(), oy, plotW, oh);

    const wx = fx(view.from);
    const ww = Math.max(14, fx(view.to) - wx);   // 너무 짧으면 잡을 수가 없다
    g.fillStyle = "#4a5160";
    g.beginPath();
    const rr = oh / 2;
    g.roundRect(Math.min(wx, axisW() + plotW - ww), oy, ww, oh, rr);
    g.fill();
  }
}

/** 전체 구간 띠의 세로 범위. 마우스가 거기 있는지 보려고 쓴다. */
export function overviewBand(canvas: HTMLCanvasElement): [number, number] {
  return [canvas.clientHeight - OVER_H, canvas.clientHeight];
}

/** 전체 구간 띠에서 x → 시각(ms) */
export function msAtOverviewX(
  canvas: HTMLCanvasElement, full: View, clientX: number,
): number {
  const rect = canvas.getBoundingClientRect();
  const plotW = Math.max(1, rect.width - axisW() - 8);
  const x = clientX - rect.left - axisW();
  return full.from + (x / plotW) * (full.to - full.from);
}

/** 화면 x 좌표 → 시각(ms) */
export function msAtX(canvas: HTMLCanvasElement, view: View, clientX: number): number {
  const rect = canvas.getBoundingClientRect();
  const plotW = Math.max(1, rect.width - axisW() - 8);
  const x = clientX - rect.left - axisW();
  return view.from + (x / plotW) * (view.to - view.from);
}

export const plotLeft = () => axisW();

/** 이름 칸에서 마우스가 몇 번째 줄에 있나. 밖이면 -1 */
export function rowAtY(
  canvas: HTMLCanvasElement, count: number, clientX: number, clientY: number,
): number {
  const r = canvas.getBoundingClientRect();
  const x = clientX - r.left;
  if (x < 0 || x > LABEL_W) return -1;
  const y = clientY - r.top - TIME_H;
  const bodyH = r.height - TIME_H - OVER_H;
  const rowH = Math.max(24, (bodyH - GAP * (count - 1)) / count);
  const i = Math.floor(y / (rowH + GAP));
  return i >= 0 && i < count && y >= 0 ? i : -1;
}

/** 그 줄의 이름을 고칠 입력칸을 놓을 자리 (캔버스 기준 픽셀) */
export function labelBox(
  canvas: HTMLCanvasElement, count: number, row: number,
): { left: number; top: number; width: number } {
  const r = canvas.getBoundingClientRect();
  const bodyH = r.height - TIME_H - OVER_H;
  const rowH = Math.max(24, (bodyH - GAP * (count - 1)) / count);
  const top = TIME_H + row * (rowH + GAP) + rowH / 2 - 15;
  return { left: 20, top, width: LABEL_W - 26 };   // 색 네모 옆에 놓는다
}
