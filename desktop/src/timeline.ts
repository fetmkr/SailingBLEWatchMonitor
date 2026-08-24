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

const AXIS_W = 62;
const TIME_H = 22;
const GAP = 8;
/** 아래에 붙는 전체 구간 띠. 지금 어디를 보고 있는지 알려 준다. */
const OVER_H = 34;

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

  const plotW = Math.max(1, cssW - AXIS_W - 8);
  const rows = series.length || 1;
  const bodyH = cssH - TIME_H - OVER_H;
  const rowH = Math.max(24, (bodyH - GAP * (rows - 1)) / rows);
  const cols = Math.max(1, Math.floor(plotW));
  const span = Math.max(1, view.to - view.from);
  const xOf = (ms: number) => AXIS_W + ((ms - view.from) / span) * plotW;

  const css = getComputedStyle(document.documentElement);
  const ink = css.getPropertyValue("--ink").trim() || "#e6e6e6";
  const dim = css.getPropertyValue("--dim").trim() || "#8a8a8a";
  const grid = css.getPropertyValue("--grid").trim() || "#2a2a2a";

  // ── 세로 눈금 (시간) ────────────────────────────────────────────────
  const step = tickStep(span, Math.max(3, Math.floor(plotW / 110)));
  const first = Math.ceil(view.from / step) * step;
  g.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
  g.textBaseline = "top";
  for (let t = first; t <= view.to; t += step) {
    const x = Math.round(xOf(t)) + 0.5;
    g.strokeStyle = grid;
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(x, 0);
    g.lineTo(x, bodyH);
    g.stroke();
    g.fillStyle = dim;
    g.textAlign = "center";
    g.fillText(formatDuration(t), x, bodyH + 5);
  }

  // ── 마킹 ────────────────────────────────────────────────────────────
  for (const m of o.marks) {
    if (m < view.from || m > view.to) continue;
    const x = Math.round(xOf(m)) + 0.5;
    g.strokeStyle = "#f0a020";
    g.lineWidth = 1.5;
    g.setLineDash([4, 3]);
    g.beginPath();
    g.moveTo(x, 0);
    g.lineTo(x, bodyH);
    g.stroke();
    g.setLineDash([]);
  }

  // ── 값들 ────────────────────────────────────────────────────────────
  series.forEach((s, r) => {
    const top = r * (rowH + GAP);
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
      g.moveTo(AXIS_W, y);
      g.lineTo(cssW, y);
      g.stroke();
    }

    // 최소~최대를 세로줄로. 봉우리가 살아 있다.
    g.strokeStyle = s.color;
    g.lineWidth = 1;
    g.beginPath();
    for (let c = 0; c < cols; c++) {
      if (!band.has[c]) continue;
      const x = AXIS_W + c + 0.5;
      const y1 = yOf(band.max[c]);
      const y2 = yOf(band.min[c]);
      g.moveTo(x, y1);
      g.lineTo(x, Math.max(y2, y1 + 0.6));   // 한 점짜리도 보이게
    }
    g.stroke();

    // 이름과 눈금
    g.fillStyle = dim;
    g.textAlign = "right";
    g.fillText(rhi.toFixed(rhi >= 100 ? 0 : 1), AXIS_W - 6, top);
    g.fillText(rlo.toFixed(rlo <= -100 ? 0 : 1), AXIS_W - 6, bot - 12);
    g.fillStyle = s.color;
    g.textAlign = "left";
    g.fillText(`${s.name} (${s.unit})`, AXIS_W + 6, top + 2);

    g.strokeStyle = grid;
    g.beginPath();
    g.moveTo(AXIS_W + 0.5, top);
    g.lineTo(AXIS_W + 0.5, bot);
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
    g.moveTo(x, 0);
    g.lineTo(x, bodyH);
    g.stroke();
    g.globalAlpha = 1;
  }

  if (o.pinMs !== null && o.pinMs >= view.from && o.pinMs <= view.to) {
    const x = Math.round(xOf(o.pinMs)) + 0.5;
    g.strokeStyle = "#4ea1ff";
    g.lineWidth = 1.5;
    g.beginPath();
    g.moveTo(x, 0);
    g.lineTo(x, bodyH);
    g.stroke();
    // 위쪽에 작은 손잡이. 어느 것이 고정인지 한눈에 갈린다.
    g.fillStyle = "#4ea1ff";
    g.beginPath();
    g.moveTo(x - 5, 0);
    g.lineTo(x + 5, 0);
    g.lineTo(x, 8);
    g.closePath();
    g.fill();
  }

  // ── 전체 구간 띠 ────────────────────────────────────────────────────
  //
  // 크게 당겨서 보면 전체 어디쯤인지 알 수가 없다. Saleae 처럼 아래에
  // 전체를 깔고 지금 보는 구간을 밝게 표시한다. 여기를 끌어도 움직인다.
  if (o.full && o.full.to > o.full.from) {
    const oy = cssH - OVER_H + 4;
    const oh = OVER_H - 8;
    const fSpan = o.full.to - o.full.from;
    const fx = (ms: number) => AXIS_W + ((ms - o.full!.from) / fSpan) * plotW;

    g.fillStyle = "#101317";
    g.fillRect(AXIS_W, oy, plotW, oh);

    // 전체 속도 모양을 얇게 깔아 둔다. 어디가 빠른 구간인지 눈에 띈다.
    const s0 = series[0];
    if (s0) {
      const band = bucketize(s0, o.full, Math.floor(plotW));
      let lo = Infinity, hi = -Infinity;
      for (let c = 0; c < band.has.length; c++) {
        if (!band.has[c]) continue;
        if (band.min[c] < lo) lo = band.min[c];
        if (band.max[c] > hi) hi = band.max[c];
      }
      if (hi > lo) {
        g.strokeStyle = s0.color;
        g.globalAlpha = 0.55;
        g.beginPath();
        for (let c = 0; c < band.has.length; c++) {
          if (!band.has[c]) continue;
          const x = AXIS_W + c + 0.5;
          const y1 = oy + oh - ((band.max[c] - lo) / (hi - lo)) * oh;
          const y2 = oy + oh - ((band.min[c] - lo) / (hi - lo)) * oh;
          g.moveTo(x, y1);
          g.lineTo(x, Math.max(y2, y1 + 0.6));
        }
        g.stroke();
        g.globalAlpha = 1;
      }
    }

    // 지금 보는 구간
    const wx = fx(view.from);
    const ww = Math.max(3, fx(view.to) - wx);
    g.fillStyle = "rgba(255,255,255,0.13)";
    g.fillRect(wx, oy, ww, oh);
    g.strokeStyle = ink;
    g.lineWidth = 1;
    g.strokeRect(Math.round(wx) + 0.5, oy + 0.5, Math.round(ww), oh - 1);

    // 양쪽 손잡이 — 여기를 끌면 구간이 넓어지고 좁아진다
    g.fillStyle = ink;
    g.fillRect(Math.round(wx) - 1, oy + oh / 2 - 6, 3, 12);
    g.fillRect(Math.round(wx + ww) - 2, oy + oh / 2 - 6, 3, 12);
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
  const plotW = Math.max(1, rect.width - AXIS_W - 8);
  const x = clientX - rect.left - AXIS_W;
  return full.from + (x / plotW) * (full.to - full.from);
}

/** 화면 x 좌표 → 시각(ms) */
export function msAtX(canvas: HTMLCanvasElement, view: View, clientX: number): number {
  const rect = canvas.getBoundingClientRect();
  const plotW = Math.max(1, rect.width - AXIS_W - 8);
  const x = clientX - rect.left - AXIS_W;
  return view.from + (x / plotW) * (view.to - view.from);
}

export const PLOT_LEFT = AXIS_W;
