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
  /**
   * 이 값이 물리적으로 가질 수 있는 범위. 축이 여기를 넘지 않는다.
   *
   * ★ 없으면 **있을 수 없는 눈금**이 찍힌다. 속도 칸에 `-0.3 kn`,
   *   방위 칸에 `389 deg` 와 `-28.8 deg` 가 나왔다 (2026-08-31).
   *   축에 위아래로 8% 여백을 주는데, 0~360 이면 그 여백이 28.8 이다.
   *
   * 속도는 `[0]` (아래만 막는다), 방위는 `[0, 360]` (양쪽 다 막는다).
   */
  limit?: [number] | [number, number];
  /** 접어 두었나. 접힌 줄은 이름만 남기고 자리를 안 쓴다 */
  collapsed?: boolean;
  /**
   * 같은 자리에 갈아 끼울 수 있는 다른 값.
   *
   * 원본 옆에 줄을 하나 더 만드는 대신 **한 자리에서 바꿔 본다.** 줄이
   * 반으로 줄고, 두 줄을 눈으로 맞춰볼 필요가 없다. 이름 칸의 작은 단추를
   * 누르면 바뀐다.
   *   SOG·COG  → 위치로 계산한 값 (cal)
   *   HDG·힐·트림 → 축과 기울기를 보정한 값 (comp)
   */
  alt?: { ys: Float32Array; name: string; tag: string };
  /** 지금 갈아 끼운 값을 보고 있나 */
  altOn?: boolean;
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

  const ys = ysOf(s);
  for (let i = lo; i < s.xs.length; i++) {
    const x = s.xs[i];
    if (x > view.to) break;
    const c = Math.min(cols - 1, Math.floor(((x - view.from) / span) * cols));
    const y = ys[i];
    if (!Number.isFinite(y)) continue;      // 값 없음은 안 그린다
    if (y < min[c]) min[c] = y;
    if (y > max[c]) max[c] = y;
    has[c] = 1;
  }
  return { min, max, has };
}

/** 지금 그려야 할 값. 단추를 눌렀으면 갈아 끼운 쪽이다. */
export function ysOf(s: Series): Float32Array {
  return s.altOn && s.alt ? s.alt.ys : s.ys;
}
/** 지금 보여줘야 할 이름. */
function nameOf(s: Series): string {
  return s.altOn && s.alt ? s.alt.name : s.name;
}

function niceRange(lo: number, hi: number, zeroCentered?: boolean,
                   limit?: [number] | [number, number]): [number, number] {
  if (!Number.isFinite(lo) || !Number.isFinite(hi)) return [0, 1];

  // 방위처럼 양쪽이 다 막힌 값은 **늘 그 범위를 통째로** 보여준다.
  // 데이터에 맞춰 좁히면 세션마다 축이 달라져서 두 세션을 눈으로 못 견준다.
  if (limit && limit.length === 2) return [limit[0], limit[1]];

  let out: [number, number];
  if (zeroCentered) {
    const m = Math.max(Math.abs(lo), Math.abs(hi)) || 1;
    out = [-m * 1.1, m * 1.1];
  } else if (hi - lo < 1e-9) {
    out = [lo - 0.5, hi + 0.5];
  } else {
    const pad = (hi - lo) * 0.08;
    out = [lo - pad, hi + pad];
  }

  // 아래만 막힌 값(속도처럼 음수가 없는 것). 여백이 0 밑으로 못 내려간다.
  if (limit && limit.length === 1 && out[0] < limit[0]) {
    out[0] = limit[0];
    if (out[1] - out[0] < 1e-9) out[1] = out[0] + 1;
  }
  return out;
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

/** 마킹 하나. 파일에서 온 것과 사람이 더한 것이 섞인다 */
export interface Mark {
  ms: number;
  note: string;
  /** file = 배에서 버튼으로 찍은 것, user = 사람이 나중에 더한 것 */
  from: "file" | "user";
}

export interface DrawOpts {
  canvas: HTMLCanvasElement;
  series: Series[];
  view: View;
  /** 마킹 */
  marks: Mark[];
  /** 마우스가 있는 시각. 옅게 그린다. null 이면 안 그린다 */
  cursorMs: number | null;
  /** 눌러서 고정한 시각. 진하게 그린다 */
  pinMs: number | null;
  /** 전체 구간. 아래 띠를 그린다 */
  full?: View;
  /**
   * 시각을 어디부터 세나 (ms).
   *
   * 영상을 맞춰 놓으면 영상 0초를 여기에 넣는다. 그러면 타임라인 숫자와
   * 영상 재생 시간이 같은 값이 된다 — 영상이 1:23 일 때 타임라인도 1:23 이다.
   * 서로 다른 숫자를 보면서 맞추려면 머릿속으로 계속 빼야 한다.
   */
  originMs?: number;
}

/**
 * 왼쪽 이름 칸. Saleae 처럼 값 그림 바깥에 둔다.
 *
 * 이름을 그림 안에 얹으면 값과 겹쳐서 둘 다 읽기 어렵다. 밖으로 빼면
 * 이름은 이름대로, 값은 값대로 읽힌다. 여기를 누르면 이름을 고친다.
 */
// 줄 이름이 들어가는 칸. 148 은 글자가 12px 이던 시절 값이다.
// 글자를 키우면 이름이 더 잘리므로 같이 넓힌다. 사람이 끌어서 바꾸면
// 그 값이 우선한다 (setLabelWidth).
let LABEL_W = 148;

/** 글자 크기에 맞춰 처음 폭을 잡았는가.
 *  사람이 손으로 끌어 놓았거나 저장된 값을 불러왔으면 건드리지 않는다. */
let widthsTuned = false;
/** 세로축 숫자가 들어갈 폭 */
let AXIS_NUM_W = 62;
const axisW = () => LABEL_W + AXIS_NUM_W;

/** 접힌 줄의 높이. 이름만 보이면 되니 이 정도면 된다. */
const MINI_ROW = 26;

/**
 * 펴 둔 줄의 최소 높이.
 *
 * 줄이 열여섯이면 칸에 다 우겨 넣었을 때 하나가 20픽셀도 안 된다. 파형이
 * 뭉개져서 볼 수가 없다. 그래서 **짜부라뜨리지 않고 넘치면 위아래로
 * 굴린다.**
 */
const MIN_ROW = 64;

/** 오른쪽 세로 굴림대 두께 */
const VBAR_W = 8;

/** 위아래로 얼마나 굴렸나 (px). 넘칠 때만 0 보다 커진다. */
let scrollY = 0;
/** 그릴 것의 전체 높이와 보이는 높이. 굴림대를 그리고 한계를 잡는 데 쓴다. */
let contentH = 0, viewH = 0;

export function maxScroll(): number { return Math.max(0, contentH - viewH); }
export function scrollTo(y: number) {
  scrollY = Math.min(maxScroll(), Math.max(0, y));
}
export function scrollBy(dy: number) { scrollTo(scrollY + dy); }
export function scrollPos(): number { return scrollY; }
/** 넘쳐서 굴릴 수 있나. 굴림대를 그릴지 정할 때 쓴다. */
export function canScroll(): boolean { return maxScroll() > 0.5; }

/** 접기 표시가 차지하는 폭. 이 안을 누르면 접힌다. */
const CHEV_W = 26;

/** 줄마다의 자리. draw 가 채우고 hit test 가 읽는다 — 늘 같은 값이어야 한다. */
let rowBoxes: { top: number; h: number }[] = [];
/** 값을 갈아 끼우는 작은 단추 자리. 그 줄에 갈아 낄 값이 없으면 null */
let altBoxes: ({ x: number; y: number; w: number; h: number } | null)[] = [];

/** 이름 칸 너비. 경계를 끌어서 바꾼다 (이름이 길면 넘치기 때문이다) */
export function labelWidth(): number { return LABEL_W; }
export function numWidth(): number { return AXIS_NUM_W; }
export function setNumWidth(px: number) {
  widthsTuned = true;   // 사람이 정한 값이 우선한다
  AXIS_NUM_W = Math.min(200, Math.max(40, Math.round(px)));
}
export function setLabelWidth(px: number): void {
  widthsTuned = true;   // 사람이 정한 값이 우선한다
  LABEL_W = Math.min(360, Math.max(70, Math.round(px)));
}
// 마지막으로 그린 자리들. 마우스가 그 위에 있는지 보려고 기억해 둔다.
let pillBox: { x: number; y: number; w: number; h: number } | null = null;
const flagBoxes: { i: number; x: number; y: number; w: number; h: number }[] = [];

/** 마우스가 파란 알약(고정 손잡이) 위에 있나 */
/** 손이 닿아야 하는 최소 크기. CSS 의 --tap 을 그대로 읽는다.
 *  맥은 28, 손가락 기기는 44 다. */
function tapSize(): number {
  const v = parseFloat(
    getComputedStyle(document.documentElement).getPropertyValue("--tap"));
  return Number.isFinite(v) ? v : 44;
}

export function onPinPill(canvas: HTMLCanvasElement, cx: number, cy: number): boolean {
  if (!pillBox) return false;
  const r = canvas.getBoundingClientRect();
  const x = cx - r.left, y = cy - r.top;

  // ★ 눈에 보이는 알약보다 잡는 자리를 넓게 잡는다.
  //   알약은 맥에서 가로 50 세로 17 밖에 안 된다. 애플이 말하는 최소가
  //   28(마우스)·44(손가락)인데 한참 작다. 잡으려다 빗나가면 그림 위를
  //   누른 것이 되어 커서가 그 자리로 튄다. 끌리는 게 아니라 튀는 것처럼
  //   보인다.
  //
  //   그려지는 모양은 그대로 두고 닿는 넓이만 키운다.
  const need = tapSize();
  const padX = Math.max(3, (need - pillBox.w) / 2);
  const padY = Math.max(3, (need - pillBox.h) / 2);
  return x >= pillBox.x - padX && x <= pillBox.x + pillBox.w + padX &&
         y >= pillBox.y - padY && y <= pillBox.y + pillBox.h + padY;
}

/** 마우스 밑에 깃발이 있으면 그 번호, 없으면 -1 */
export function flagAt(canvas: HTMLCanvasElement, cx: number, cy: number): number {
  const r = canvas.getBoundingClientRect();
  const x = cx - r.left, y = cy - r.top;
  for (const b of flagBoxes) {
    if (x >= b.x && x <= b.x + b.w && y >= b.y - 2 && y <= b.y + b.h + 2) return b.i;
  }
  return -1;
}

/** 그 깃발 옆에 입력칸을 놓을 자리 */
export function flagBox(i: number): { left: number; top: number } | null {
  const b = flagBoxes.find((f) => f.i === i);
  return b ? { left: b.x, top: b.y + 14 } : null;
}

/** 마우스가 위 시간 축에 있나 (거기를 끌면 화면이 밀린다) */
export function onTimeAxis(canvas: HTMLCanvasElement, clientY: number): boolean {
  return clientY - canvas.getBoundingClientRect().top < TIME_H;
}

/** 마우스가 이름 칸 경계에 있나 (끌어서 넓히는 자리) */
/** 숫자 칸과 그림 사이 경계에 있나. 잡아 끌면 숫자 칸이 넓어진다. */
export function onNumEdge(canvas: HTMLCanvasElement, clientX: number): boolean {
  const x = clientX - canvas.getBoundingClientRect().left;
  return Math.abs(x - axisW()) <= 4;
}

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

function tuneWidths(base: number) {
  if (widthsTuned) return;
  widthsTuned = true;
  const k = base / 12;                 // 148 과 62 는 12px 글자 기준이었다
  LABEL_W = Math.round(148 * k);
  AXIS_NUM_W = Math.round(62 * k);
}

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

  // 줄 높이. 접힌 줄은 이름만 남기고, 남는 자리는 펼친 줄들이 나눠 갖는다.
  // 그래서 하나만 펼쳐 두면 그 하나가 화면을 다 쓴다.
  //
  // ★ 다 우겨 넣지 않는다. 펴 둔 줄이 MIN_ROW 보다 얇아질 만큼 많으면
  //   짜부라뜨리는 대신 **위아래로 굴린다**. 파형이 뭉개지면 볼 이유가 없다.
  const shut = series.filter((x) => x.collapsed).length;
  const open = Math.max(1, series.length - shut);
  const freeH = bodyH - GAP * (rows - 1) - shut * MINI_ROW;
  const rowH = Math.max(MIN_ROW, freeH / open);

  viewH = bodyH;
  contentH = open * rowH + shut * MINI_ROW + GAP * Math.max(0, rows - 1);
  scrollTo(scrollY);                       // 줄이 줄어들면 굴린 만큼도 줄인다

  rowBoxes = [];
  altBoxes = [];
  {
    let y = bodyTop - scrollY;
    for (const sx of series) {
      const h = sx.collapsed ? MINI_ROW : rowH;
      rowBoxes.push({ top: y, h });
      altBoxes.push(null);
      y += h + GAP;
    }
    if (!series.length) { rowBoxes.push({ top: bodyTop, h: bodyH }); altBoxes.push(null); }
  }
  const cols = Math.max(1, Math.floor(plotW));
  const span = Math.max(1, view.to - view.from);
  const xOf = (ms: number) => axisW() + ((ms - view.from) / span) * plotW;

  // 색은 CSS 에서 읽는다. 어둡게·밝게를 바꾸면 여기가 같이 따라온다.
  // 캔버스는 CSS 가 안 먹으니 이렇게 손으로 가져와야 한다.
  const css = getComputedStyle(document.documentElement);
  const v = (name: string, fallback: string) =>
    css.getPropertyValue(name).trim() || fallback;
  const ink = v("--ink", "#e6e6e6");
  const dim = v("--dim", "#8a8a8a");
  const grid = v("--grid", "#2a2a2a");
  const band = v("--tl-band", "#181b21");     // 시간 축·아래 띠 바탕
  const plotBg = v("--tl-plot", "#0b0e12");   // 그림 바탕
  const panel = v("--panel", "#1b1e24");
  const thumb = v("--tl-thumb", "#4a5160");   // 아래 띠에서 지금 보는 자리
  const MARK = v("--mark", "#f0a020");

  // 글자 크기도 CSS 에서 읽는다. 색과 같은 이유다. 캔버스에는 CSS 가 안
  // 먹으니 손으로 가져와야 하는데, 그래도 기준은 styles.css 한 곳에 둔다.
  const fpx = (name: string, fallback: number) => {
    const n = parseFloat(css.getPropertyValue(name));
    return Number.isFinite(n) ? n : fallback;
  };
  const FS = fpx("--fs", 17);         // 줄 이름
  const FSM = fpx("--fs-sm", 16);
  const FXS = fpx("--fs-xs", 15);     // 눈금 숫자
  const FXXS = fpx("--fs-xxs", 13);   // 깃발 번호·메모
  const MONO = "ui-monospace, SFMono-Regular, Menlo, monospace";
  const SANS = "-apple-system, system-ui, sans-serif";
  tuneWidths(FSM);
  const PIN = v("--pin", "#4ea1ff");

  // ── 시간 축 (위) ────────────────────────────────────────────────────
  //
  // Saleae 방식이다. 맨 왼쪽에 기준 시각을 굵게 적고, 그 뒤로는 거기서
  // 얼마나 떨어졌는지를 **단위와 함께** 적는다.
  //
  //   3:04    +10 초   +20 초   +30 초
  //
  // 단위가 없으면 크게 당겼을 때 "0:02" 가 2초인지 2분인지 헷갈린다.
  g.font = `${FXS}px ${MONO}`;
  g.textBaseline = "top";

  // 왼쪽 이름 칸 바탕
  g.fillStyle = band;
  g.fillRect(0, 0, LABEL_W, cssH);

  // 축 바탕
  g.fillStyle = band;
  g.fillRect(0, 0, cssW, TIME_H);
  g.strokeStyle = grid;
  g.lineWidth = 1;
  g.beginPath();
  g.moveTo(0, TIME_H - 0.5);
  g.lineTo(cssW, TIME_H - 0.5);
  g.stroke();

  const origin = o.originMs ?? 0;
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
  g.font = `bold ${FXS}px ${MONO}`;
  g.fillText(formatDuration(base - origin), LABEL_W + 4, 6);
  g.font = `${FXS}px ${MONO}`;

  // ── 마킹 ────────────────────────────────────────────────────────────
  //
  // 선수가 배에서 버튼을 눌러 "이 순간" 이라고 찍어 둔 자리다. 주황 점선만
  // 그으면 뭔지 알 수가 없어서 위에 깃발과 번호를 붙인다.
  flagBoxes.length = 0;
  o.marks.forEach((mk, i) => {
    const m = mk.ms;
    if (m < view.from || m > view.to) return;
    const x = Math.round(xOf(m)) + 0.5;
    g.strokeStyle = MARK;
    g.lineWidth = 1.5;
    g.setLineDash([4, 3]);
    g.beginPath();
    g.moveTo(x, bodyTop);
    g.lineTo(x, bodyTop + bodyH);
    g.stroke();
    g.setLineDash([]);

    // 깃발 + 메모. 눌러서 고치거나 지울 수 있게 자리를 기억해 둔다.
    g.font = `${FXXS}px ${SANS}`;
    const note = mk.note.trim();
    const nw = note ? g.measureText(note).width + 6 : 0;
    const fw = 16 + nw;

    g.fillStyle = MARK;
    g.beginPath();
    g.moveTo(x, bodyTop);
    g.lineTo(x + fw, bodyTop);
    g.lineTo(x + fw + 5, bodyTop + 6);
    g.lineTo(x + fw, bodyTop + 12);
    g.lineTo(x, bodyTop + 12);
    g.closePath();
    g.fill();

    g.fillStyle = plotBg;
    g.textAlign = "left";
    g.textBaseline = "top";
    g.font = `bold ${FXXS}px ${MONO}`;
    g.fillText(String(i + 1), x + 3, bodyTop + 2);
    if (note) {
      g.font = `${FXXS}px ${SANS}`;
      g.fillText(note, x + 13, bodyTop + 1);
    }
    flagBoxes.push({ i, x, y: bodyTop, w: fw + 5, h: 12 });
    g.font = `${FXS}px ${MONO}`;
  });

  // ── 값들 ────────────────────────────────────────────────────────────
  //
  // 굴린 만큼 위로 밀려 있으니, 본문 밖으로 새지 않게 자르고 그린다.
  // 시간 축과 아래 띠를 침범하면 안 된다.
  g.save();
  g.beginPath();
  g.rect(0, bodyTop, cssW, bodyH);
  g.clip();

  series.forEach((s, r) => {
    // 화면 밖에 있는 줄은 그릴 것도 없다
    if (rowBoxes[r].top > bodyTop + bodyH || rowBoxes[r].top + rowBoxes[r].h < bodyTop) return;
    const top = rowBoxes[r].top;
    const rh = rowBoxes[r].h;
    const bot = top + rh;

    // 접힌 줄 — 이름과 접기 표시만. 그림도 눈금도 안 그린다.
    if (s.collapsed) {
      const my = top + rh / 2;
      chevron(g, 13, my, false, dim);
      g.fillStyle = s.color;
      g.fillRect(CHEV_W + 2, my - 4, 8, 8);
      g.fillStyle = dim;
      g.textAlign = "left";
      g.textBaseline = "middle";
      g.font = `${FSM}px ${SANS}`;
      g.fillText(fitText(g, nameOf(s), LABEL_W - CHEV_W - 20), CHEV_W + 15, my);
      g.textBaseline = "top";

      g.strokeStyle = grid;
      g.lineWidth = 1;
      g.beginPath();
      g.moveTo(0, Math.round(bot + GAP / 2) + 0.5);
      g.lineTo(cssW, Math.round(bot + GAP / 2) + 0.5);
      g.stroke();
      return;
    }

    const band = bucketize(s, view, cols);

    let lo = Infinity, hi = -Infinity;
    for (let c = 0; c < cols; c++) {
      if (!band.has[c]) continue;
      if (band.min[c] < lo) lo = band.min[c];
      if (band.max[c] > hi) hi = band.max[c];
    }
    const [rlo, rhi] = niceRange(lo, hi, s.zeroCentered, s.limit);
    const yOf = (v: number) => bot - ((v - rlo) / (rhi - rlo)) * rh;

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
        const y = ysOf(s)[i];
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
        const y = ysOf(s)[i];
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
    g.font = `${FXS}px ${MONO}`;
    g.fillText(`${rhi.toFixed(rhi >= 100 ? 0 : 1)} ${s.unit}`, axisW() - 6, top);
    g.fillText(`${rlo.toFixed(rlo <= -100 ? 0 : 1)} ${s.unit}`, axisW() - 6, bot - 12);

    // ── 왼쪽 이름 칸 — 한 줄이면 된다 ──
    //
    // 처음에 짧은 표시(SOG)와 이름(Speed Over Ground)을 나란히 뒀는데,
    // 같은 말을 두 번 쓰는 셈이었다. Saleae 의 D0 는 하드웨어 채널 번호라
    // 이름과 다른 것이지만 우리는 그게 아니다.
    //
    // 색은 이름 앞의 작은 네모로 준다. 그림의 선 색과 이어진다.
    const midY = top + rh / 2;
    // 접기 표시. 누르면 이 줄이 접힌다.
    chevron(g, 13, midY, true, dim);

    g.fillStyle = s.color;
    g.fillRect(CHEV_W + 2, midY - 5, 9, 9);

    g.fillStyle = ink;
    g.font = `${FS}px ${SANS}`;
    g.textAlign = "left";
    g.textBaseline = "middle";
    // 칸을 넘치면 잘라 준다. 그냥 넘치면 옆 칸 숫자와 겹쳐서 둘 다 못 읽는다.
    g.fillText(fitText(g, nameOf(s), LABEL_W - CHEV_W - 22 - (s.alt ? 44 : 0)),
               CHEV_W + 17, midY);

    // ── 값을 갈아 끼우는 단추 ──
    //
    // 줄을 하나 더 만드는 대신 **같은 자리에서 바꿔 본다.** 켜져 있으면
    // 색이 차고, 꺼져 있으면 테두리만 남는다. 지금 무엇을 보고 있는지가
    // 글자(cal / comp)로 남아 있어야 나중에 화면만 보고도 안다.
    if (s.alt) {
      const bw = 38, bh = 16;
      const bx = LABEL_W - bw - 6, by = Math.round(midY - bh / 2);
      g.beginPath();
      const rr = 4;
      g.moveTo(bx + rr, by);
      g.arcTo(bx + bw, by, bx + bw, by + bh, rr);
      g.arcTo(bx + bw, by + bh, bx, by + bh, rr);
      g.arcTo(bx, by + bh, bx, by, rr);
      g.arcTo(bx, by, bx + bw, by, rr);
      g.closePath();
      if (s.altOn) { g.fillStyle = s.color; g.fill(); }
      else { g.strokeStyle = dim; g.lineWidth = 1; g.stroke(); }
      g.fillStyle = s.altOn ? "#0b0e13" : dim;
      g.font = `10px ${SANS}`;
      g.textAlign = "center";
      g.fillText(s.alt.tag, bx + bw / 2, midY);
      g.textAlign = "left";
      g.font = `${FS}px ${SANS}`;
      altBoxes[r] = { x: bx, y: by, w: bw, h: bh };
    }
    g.textBaseline = "top";

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

  g.restore();                              // 자르기 끝

  // ── 오른쪽 세로 굴림대 ──────────────────────────────────────────────
  //
  // 넘칠 때만 나온다. 있어야 "밑에 더 있다" 는 것을 안다.
  if (contentH > bodyH + 0.5) {
    const trackX = cssW - VBAR_W;
    g.fillStyle = band;
    g.fillRect(trackX, bodyTop, VBAR_W, bodyH);

    const th = Math.max(24, (bodyH / contentH) * bodyH);
    const ty = bodyTop + (scrollY / (contentH - bodyH)) * (bodyH - th);
    g.fillStyle = thumb;
    g.beginPath();
    g.roundRect(trackX + 1.5, ty, VBAR_W - 3, th, (VBAR_W - 3) / 2);
    g.fill();
  }

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
    g.globalAlpha = 0.55;
    g.lineWidth = 1;
    g.setLineDash([3, 3]);
    g.beginPath();
    g.moveTo(x, bodyTop);
    g.lineTo(x, bodyTop + bodyH);
    g.stroke();
    g.setLineDash([]);
    g.globalAlpha = 1;
  }

  // ── 고정한 자리 ──
  //
  // 어두운 바탕에 얇은 선 하나면 눈에 안 들어온다. 굵게 긋고, 시간 축에
  // 시각을 적은 알약을 붙이고, 위아래에 손잡이를 둔다.
  // 안 그리면 잡히는 자리도 없애야 한다. 안 그러면 지난번에 그린 자리가
  // 그대로 남아, 아무것도 없는 곳을 눌렀는데 알약을 잡은 것이 된다.
  pillBox = null;
  if (o.pinMs !== null && o.pinMs >= view.from && o.pinMs <= view.to) {
    const x = Math.round(xOf(o.pinMs)) + 0.5;

    g.strokeStyle = PIN;
    g.lineWidth = 2;
    g.beginPath();
    g.moveTo(x, TIME_H);
    g.lineTo(x, bodyTop + bodyH);
    g.stroke();

    // 시간 축에 붙는 알약
    const label = formatDuration(o.pinMs - origin);
    g.font = `bold ${FXS}px ${MONO}`;
    const tw = g.measureText(label).width;
    // 알약은 글자를 담는 그릇이라 글자 크기를 따라간다. 17 로 박아 뒀더니
    // 글자를 키우자마자 넘쳤다.
    const ph = Math.round(FXS * 1.55);
    const pw = tw + 12;
    let px = x - pw / 2;
    px = Math.max(axisW(), Math.min(px, cssW - pw - 2));
    pillBox = { x: px, y: 4, w: pw, h: ph };
    g.fillStyle = PIN;
    g.beginPath();
    g.roundRect(px, 4, pw, ph, 4);
    g.fill();
    g.fillStyle = plotBg;
    g.textAlign = "center";
    g.textBaseline = "middle";
    g.fillText(label, px + pw / 2, 4 + ph / 2);

    // 아래쪽 손잡이
    g.fillStyle = PIN;
    g.beginPath();
    g.moveTo(x - 5, bodyTop + bodyH);
    g.lineTo(x + 5, bodyTop + bodyH);
    g.lineTo(x, bodyTop + bodyH - 8);
    g.closePath();
    g.fill();

    g.font = `${FXS}px ${MONO}`;
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

    g.fillStyle = panel;
    g.fillRect(axisW(), oy, plotW, oh);

    const wx = fx(view.from);
    const ww = Math.max(14, fx(view.to) - wx);   // 너무 짧으면 잡을 수가 없다
    g.fillStyle = thumb;
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
/**
 * 그 자리가 몇 번째 줄인가. -1 이면 이름 칸 밖이다.
 *
 * ★ 그릴 때 재어 둔 자리를 그대로 읽는다. 여기서 다시 셈하면 접힌 줄이
 *   생겼을 때 그림과 어긋난다.
 */
/**
 * 접기 표시를 직접 그린다.
 *
 * 글꼴의 ▾ 문자를 쓰다가 너무 작았다. 글꼴마다 크기가 제각각이라 키우기도
 * 어렵다. 삼각형은 그냥 그리는 편이 낫다.
 */
function chevron(
  g: CanvasRenderingContext2D, cx: number, cy: number, open: boolean, color: string,
) {
  const s = 5;                       // 반지름쯤
  g.fillStyle = color;
  g.beginPath();
  if (open) {                        // ▾ 아래를 가리킨다 = 펼쳐져 있다
    g.moveTo(cx - s, cy - s * 0.6);
    g.lineTo(cx + s, cy - s * 0.6);
    g.lineTo(cx, cy + s * 0.8);
  } else {                           // ▸ 오른쪽 = 접혀 있다
    g.moveTo(cx - s * 0.6, cy - s);
    g.lineTo(cx + s * 0.8, cy);
    g.lineTo(cx - s * 0.6, cy + s);
  }
  g.closePath();
  g.fill();
}

/**
 * 값 갈아끼우기 단추를 눌렀나. 누른 줄 번호, 아니면 -1.
 *
 * ★ 이름 칸 클릭(이름 고치기)보다 **먼저** 봐야 한다. 단추가 이름 칸 안에
 *   있어서, 순서를 바꾸면 단추를 눌러도 이름 고치기가 열린다.
 */
export function altAtY(
  canvas: HTMLCanvasElement, clientX: number, clientY: number,
): number {
  const r = canvas.getBoundingClientRect();
  const x = clientX - r.left, y = clientY - r.top;
  for (let i = 0; i < altBoxes.length; i++) {
    const b = altBoxes[i];
    if (!b) continue;
    if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return i;
  }
  return -1;
}

export function rowAtY(
  canvas: HTMLCanvasElement, _count: number, clientX: number, clientY: number,
): number {
  const r = canvas.getBoundingClientRect();
  const x = clientX - r.left;
  if (x < 0 || x > LABEL_W) return -1;
  const y = clientY - r.top;
  for (let i = 0; i < rowBoxes.length; i++) {
    const b = rowBoxes[i];
    if (y >= b.top && y <= b.top + b.h) return i;
  }
  return -1;
}

/** 오른쪽 세로 굴림대를 짚었나. */
export function onVBar(canvas: HTMLCanvasElement, clientX: number): boolean {
  if (!canScroll()) return false;
  const r = canvas.getBoundingClientRect();
  return clientX - r.left >= r.width - VBAR_W - 3;
}

/** 굴림대를 그 자리로 끌었다. 화면 y 를 굴린 양으로 바꾼다. */
export function scrollToBarY(canvas: HTMLCanvasElement, clientY: number) {
  const r = canvas.getBoundingClientRect();
  const bodyH = r.height - TIME_H - OVER_H;
  if (bodyH <= 0 || !canScroll()) return;
  const th = Math.max(24, (bodyH / contentH) * bodyH);
  const y = clientY - r.top - TIME_H - th / 2;
  const span = bodyH - th;
  scrollTo(span > 0 ? (y / span) * (contentH - bodyH) : 0);
}

/** 이름 칸 위인가. 여기서 휠을 굴리면 시간이 아니라 줄이 움직인다. */
export function onLabelColumn(canvas: HTMLCanvasElement, clientX: number): boolean {
  return clientX - canvas.getBoundingClientRect().left <= axisW();
}

/** 접기 표시(▾ ▸)를 짚었나. 이름 칸 맨 왼쪽 자리다. */
export function chevronAt(
  canvas: HTMLCanvasElement, clientX: number, clientY: number,
): number {
  const r = canvas.getBoundingClientRect();
  const x = clientX - r.left;
  if (x < 0 || x > CHEV_W) return -1;
  return rowAtY(canvas, 0, clientX, clientY);
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
