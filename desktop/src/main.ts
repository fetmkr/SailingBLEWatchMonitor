// Sail Analyzer — 기록을 받아 타임라인으로 본다.
//
// 지금 되는 것
//   - 파일 열기 (.HLG) → 검사 → 타임라인
//   - 보드에 붙어 목록 보기 / 받기 (Range 이어받기)
//
// 설계는 ../../TRANSFER.md 에 있다.

import { open } from "@tauri-apps/plugin-dialog";
import { readFile } from "@tauri-apps/plugin-fs";
import { fetch as tfetch } from "@tauri-apps/plugin-http";
import * as hlog from "./hlog";
import * as lib from "./library";
import * as vid from "./video";
import { convertFileSrc } from "@tauri-apps/api/core";
import * as tl from "./timeline";
import "./styles.css";

// ── 상태 ────────────────────────────────────────────────────────────────

let session: hlog.Session | null = null;
let series: tl.Series[] = [];
/** 화면에 보이는 마킹. 파일에서 온 것 + 코치가 더한 것 */
let marks: tl.Mark[] = [];
/** 파일에서 읽은 마킹의 시각. 감추기·메모의 열쇠로 쓴다 */
let fileMarks: number[] = [];
let view: tl.View = { from: 0, to: 1 };
let fullSpan: tl.View = { from: 0, to: 1 };
/** 마우스가 있는 시각. 마우스를 떼면 사라진다 */
let cursorMs: number | null = null;
/**
 * 눌러서 고정한 시각.
 *
 * 영상과 맞춰 보려면 한 자리에 박아 두고 봐야 한다. 마우스를 뗄 때마다
 * 풀리면 화면을 볼 수가 없다. 눌러서 박고, Esc 로 푼다.
 */
let pinMs: number | null = null;

// 보관함
let library: lib.Library = { version: 1, entries: [] };
let openId: string | null = null;       // 지금 보고 있는 세션
let query = "";
/** 왼쪽에 무엇을 보여줄지. 보관함이 기본이다 — 코치는 대개 이미 받은 걸 본다 */
let pane: "library" | "board" = "library";

// 영상
let sync: vid.Sync = { offsetMs: 0, guessed: false, fileTime: null };
let videoOn = false;
/**
 * 영상과 데이터가 물려 있나.
 *
 * ★ `여기 맞춤` 을 누르기 전에는 **따로 움직여야 한다.**
 *
 *   맞추는 방법은 이렇다. 영상을 태킹하는 장면에 두고, 타임라인에서 힐이
 *   넘어가는 자리에 커서를 두고, 그때 `여기 맞춤` 을 누른다.
 *   그런데 둘이 붙어 다니면 하나를 옮길 때 다른 하나도 따라가서 애초에
 *   맞출 수가 없다.
 *
 * 파일이 적어 둔 시각으로 짐작은 해 두지만 그건 짐작일 뿐이다. 사람이
 * 눌러서 확인해야 물린다.
 */
let linked = false;
/** 타임라인이 영상을 움직이는 중인가. 서로 밀지 않게 한 쪽만 몰게 한다 */
let seeking = false;
export const _seeking = () => seeking;

/**
 * 시각을 어디부터 세나.
 *
 *   session  세션이 시작한 때부터 (영상이 없으면 이것뿐이다)
 *   video    영상 0초부터 — 타임라인 숫자와 영상 재생 시간이 같아진다
 *
 * 영상을 맞춘 뒤에는 video 로 넘어간다. 서로 다른 숫자를 보면서 맞추려면
 * 머릿속으로 계속 빼야 한다.
 */
let timeOrigin: "session" | "video" = "session";
const originMs = () => (timeOrigin === "video" && videoOn ? sync.offsetMs : 0);

const $ = (id: string) => document.getElementById(id)!;
const canvas = () => $("plot") as HTMLCanvasElement;

/**
 * 진행률.
 *
 * 보드에서 목록을 받는 데도 시간이 걸린다 — 파일마다 머리글을 읽기 때문이다.
 * 90 MB 를 내려받으면 더 오래 걸린다. 아무 표시가 없으면 사람은 앱이 멈춘
 * 줄 안다.
 *
 *   pct 가 null 이면 "얼마나 걸릴지 모름" — 막대가 좌우로 오간다
 */
function setProgress(text: string | null, pct: number | null = null) {
  const box = $("prog");
  if (text === null) {
    box.className = "";
    $("progText").textContent = "";   // 끝났는데 글자가 남아 있으면 안 된다
    return;
  }
  box.className = pct === null ? "on busy" : "on";
  $("progText").textContent = text;
  if (pct !== null) $("progBar").style.setProperty("--p", `${Math.round(pct * 100)}%`);
}

function setStatus(text: string, kind: "" | "bad" | "good" = "") {
  const el = $("status");
  el.textContent = text;
  el.className = kind;
}

// ── 파일 열기 ───────────────────────────────────────────────────────────

async function openFile() {
  const path = await open({
    multiple: false,
    filters: [{ name: "기록 파일", extensions: ["HLG", "hlg"] }],
  });
  if (!path || typeof path !== "string") return;
  setStatus("읽는 중…");
  const bytes = await readFile(path);
  await intake(new Uint8Array(bytes), path.split("/").pop() ?? path);
}

function loadBytes(buf: Uint8Array, name: string): hlog.Session | null {
  const t0 = performance.now();
  let s: hlog.Session;
  try {
    s = hlog.parse(buf);
  } catch (e) {
    setStatus(`읽을 수 없습니다 — ${e}`, "bad");
    return null;
  }
  const ms = performance.now() - t0;

  session = s;
  buildSeries(s);
  rebuildMarks();
  renderHeader(s, name, ms, buf.length);
  fitAll();

  // 세션을 바꾸면 영상 싱크를 다시 짐작한다.
  //
  // 싱크는 "영상 0초가 이 세션의 몇 초냐" 다. 세션마다 시작한 시각이 다르니
  // 앞 세션에서 맞춘 값은 새 세션에서 뜻이 없다. 그대로 두면 엉뚱한 자리를
  // 가리키는데 사람은 맞는 줄 안다.
  if (videoOn) {
    const first = s.imu[0]?.ms ?? s.nav[0]?.ms ?? 0;
    sync = vid.guessOffset(sync.fileTime, s.header.utcStart, first);
    renderVbar();
    if (!sync.guessed) {
      setStatus("세션이 바뀌어서 영상 싱크를 다시 맞춰야 합니다.", "bad");
    }
  }

  redraw();
  return s;
}

/** 읽고, 검사하고, 보관함에 넣는다. TRANSFER.md §4 순서 그대로. */
async function intake(buf: Uint8Array, name: string) {
  const s = loadBytes(buf, name);
  if (!s) return;
  const c = hlog.check(s);
  try {
    const r = await lib.put(library, buf, s.header, c.ok, c.problems);
    library = r.lib;
    openId = r.entry.id;
    rebuildMarks();
    renderSide();
    renderDetails();
  } catch (e) {
    // 보관함에 못 넣어도 화면은 이미 그려져 있다. 보는 건 막지 않는다.
    setStatus(`보관함에 못 넣었습니다 — ${e}`, "bad");
  }
}

// ── 값 묶음 만들기 ──────────────────────────────────────────────────────

function buildSeries(s: hlog.Session) {
  const t0 = s.imu.length ? s.imu[0].ms : s.nav.length ? s.nav[0].ms : 0;

  const navX = new Float64Array(s.nav.length);
  const sog = new Float32Array(s.nav.length);
  const cog = new Float32Array(s.nav.length);
  const sv = new Float32Array(s.nav.length);
  fileMarks = [];
  for (let i = 0; i < s.nav.length; i++) {
    const r = s.nav[i];
    navX[i] = r.ms - t0;
    // 값이 없으면 NaN 으로 둔다. 0 을 넣으면 정박과 구별이 안 된다.
    sog[i] = r.sogKn ?? NaN;
    cog[i] = r.cogDeg ?? NaN;
    sv[i] = r.numSv;
    if (r.event & 0x01) fileMarks.push(r.ms - t0);
  }

  // 힐·피치를 어느 축에서 봤나. 머리글에 적혀 있다 (hlog.h 의 표).
  const hAxis = s.header.heelAxis, pAxis = s.header.pitchAxis;
  const hSign = s.header.heelSign ? -1 : 1;
  const pSign = s.header.pitchSign ? -1 : 1;
  const hOff = s.header.heelOff, pOff = s.header.pitchOff;

  const imuX = new Float64Array(s.imu.length);
  const heel = new Float32Array(s.imu.length);
  const pitch = new Float32Array(s.imu.length);
  const gz = new Float32Array(s.imu.length);
  const az = new Float32Array(s.imu.length);
  for (let i = 0; i < s.imu.length; i++) {
    const r = s.imu[i];
    imuX[i] = r.ms - t0;
    // 자세는 가속도 원본에서 뽑는다. 어느 축이 힐인지는 머리글에 적혀 있다.
    // 우리는 실제로 힐 축을 X → Y 로, 부호도 한 번 뒤집었다. 머리글을 안 보고
    // 고정된 축으로 읽으면 그 전후 파일에서 값이 틀린다.
    const [ax, ay, az_] = r.acc;
    const a = [ax, ay, az_];
    const mag = Math.hypot(ax, ay, az_) || 1;
    heel[i] = (Math.asin(clamp((hSign * a[hAxis]) / mag)) * 180) / Math.PI - hOff;
    pitch[i] = (Math.asin(clamp((pSign * a[pAxis]) / mag)) * 180) / Math.PI - pOff;
    gz[i] = r.gyr[2];
    az[i] = az_;
  }

  // 이름은 영어가 기본이다. 클래스도 대회도 영어로 돌아가고, 코치가 다른
  // 분석 도구와 나란히 볼 때 말이 맞아야 한다. 누르면 고칠 수 있다.
  series = [
    { code: "SOG",   name: n("SOG", "Speed Over Ground"), unit: "kn",
      color: "#4ea1ff", xs: navX, ys: sog },
    { code: "HEEL",  name: n("HEEL", "Heel"), unit: "deg",
      color: "#ff7a59", xs: imuX, ys: heel, zeroCentered: true },
    { code: "TRIM",  name: n("TRIM", "Trim"), unit: "deg",
      color: "#ffc857", xs: imuX, ys: pitch, zeroCentered: true },
    { code: "YAW",   name: n("YAW", "Yaw Rate"), unit: "deg/s",
      color: "#9d7bff", xs: imuX, ys: gz, zeroCentered: true },
    { code: "HEAVE", name: n("HEAVE", "Vertical Accel"), unit: "g",
      color: "#5ad19a", xs: imuX, ys: az },
    { code: "COG",   name: n("COG", "Course Over Ground"), unit: "deg",
      color: "#77d4e8", xs: navX, ys: cog },
    { code: "SAT",   name: n("SAT", "Satellites"), unit: "count",
      color: "#8a8a8a", xs: navX, ys: sv },
  ];
}

const clamp = (v: number) => (v > 1 ? 1 : v < -1 ? -1 : v);

// ── 마킹 ────────────────────────────────────────────────────────────────
//
// 배에서 찍힌 것은 파일 안에 있고 못 고친다. 코치가 붙이는 메모, 감춘 것,
// 나중에 더한 것은 보관함(library.json)에만 둔다. **원본은 안 건드린다.**
function entryNow(): lib.Entry | undefined {
  return library.entries.find((e) => e.id === openId);
}

function rebuildMarks() {
  const e = entryNow();
  const notes = e?.markNotes ?? {};
  const hidden = new Set(e?.markHidden ?? []);
  const added = e?.markAdded ?? [];

  marks = [
    ...fileMarks.filter((m) => !hidden.has(m))
      .map((m): tl.Mark => ({ ms: m, note: notes[String(m)] ?? "", from: "file" })),
    ...added.map((a): tl.Mark => ({
      ms: a.ms, note: notes[String(a.ms)] ?? a.note, from: "user",
    })),
  ].sort((a, b) => a.ms - b.ms);
}

function saveMarks(patch: Partial<lib.Entry>) {
  if (!openId) return;
  library = lib.update(library, openId, patch);
  rebuildMarks();
  redraw();
}

function addMarkAt(ms: number) {
  const e = entryNow();
  if (!e) { setStatus("보관함에 없는 파일이라 표식을 못 답니다.", "bad"); return; }
  const added = [...(e.markAdded ?? []), { ms, note: "" }];
  saveMarks({ markAdded: added });
  setStatus(`${tl.formatDuration(ms - originMs())} 에 표식을 달았습니다.`);
}

function removeMark(i: number) {
  const m = marks[i];
  const e = entryNow();
  if (!m || !e) return;
  if (m.from === "file") {
    // 배에서 찍힌 것은 지우지 않고 감춘다. 원본에는 그대로 남아 있다.
    saveMarks({ markHidden: [...(e.markHidden ?? []), m.ms] });
    setStatus("배에서 찍힌 표식이라 화면에서만 감췄습니다. 파일에는 남아 있습니다.");
  } else {
    saveMarks({ markAdded: (e.markAdded ?? []).filter((a) => a.ms !== m.ms) });
    setStatus("표식을 지웠습니다.");
  }
}

function noteMark(i: number, note: string) {
  const m = marks[i];
  const e = entryNow();
  if (!m || !e) return;
  const notes = { ...(e.markNotes ?? {}) };
  const v = note.trim();
  if (v) notes[String(m.ms)] = v; else delete notes[String(m.ms)];
  saveMarks({ markNotes: notes });
}

// ── 줄 이름 ─────────────────────────────────────────────────────────────
//
// 이름은 사람마다 다르게 부른다. 어떤 코치는 "Trim", 어떤 코치는 "Pitch" 다.
// 고쳐 쓸 수 있게 하고 남긴다.
const LABELW_KEY = "labelWidth.v1";
{
  const w = Number(localStorage.getItem(LABELW_KEY));
  if (w > 0) tl.setLabelWidth(w);
}

const NAMES_KEY = "seriesNames.v1";
let names: Record<string, string> = {};
try { names = JSON.parse(localStorage.getItem(NAMES_KEY) ?? "{}"); } catch { /* 처음 */ }
const n = (code: string, def: string) => names[code] ?? def;

function renameSeries(code: string, value: string) {
  const v = value.trim();
  if (v) names[code] = v; else delete names[code];
  localStorage.setItem(NAMES_KEY, JSON.stringify(names));
  const s = series.find((x) => x.code === code);
  if (s) s.name = v || s.code;
  redraw();
}

// ── 머리글 표시 ─────────────────────────────────────────────────────────

function renderHeader(s: hlog.Session, name: string, parseMs: number, bytes: number) {
  const h = s.header;
  const c = hlog.check(s);

  const when = h.utcStart
    ? new Date(h.utcStart * 1000).toLocaleString()
    : "위성을 못 잡은 세션";

  $("meta").innerHTML = `
    <div class="row"><b>${name}</b> <span class="dim">${(bytes / 1048576).toFixed(2)} MB · ${parseMs.toFixed(0)} ms 만에 읽음</span></div>
    <div class="row">세션 ${h.session} · 모듈 ${h.module} · ${when}</div>
    <div class="row dim">
      NAV ${s.nav.length.toLocaleString()}줄 (${c.navHz?.toFixed(2) ?? "?"} Hz) ·
      IMU ${s.imu.length.toLocaleString()}줄 (${c.imuHz?.toFixed(2) ?? "?"} Hz) ·
      등간격 ${c.evenPct?.toFixed(2) ?? "?"}%
    </div>
    <div class="row dim">
      IMU ${h.imuType === 0 ? "BNO085" : "MPU-9250"} ·
      자세 ${h.quatSrc === 0 ? "융합값" : "없음 (가속·자이로에서 뽑음)"} ·
      GNSS 동역학모델 ${h.gnssDyn}, ${h.gnssHz} Hz ·
      속도 ${h.sogSrc === 0 ? "도플러 원본" : "★다듬은 값★"}
    </div>`;

  if (c.ok) {
    setStatus("✅ 깨끗합니다", "good");
  } else {
    setStatus("❌ " + c.problems.join(" / "), "bad");
  }
}

// ── 보기 조작 ───────────────────────────────────────────────────────────

function fitAll() {
  let lo = Infinity, hi = -Infinity;
  for (const s of series) {
    if (!s.xs.length) continue;
    lo = Math.min(lo, s.xs[0]);
    hi = Math.max(hi, s.xs[s.xs.length - 1]);
  }
  if (!Number.isFinite(lo)) { lo = 0; hi = 1; }
  fullSpan = { from: lo, to: Math.max(hi, lo + 1) };
  view = { ...fullSpan };

  // ★ 파란 선을 처음부터 둔다.
  //
  //   전에는 데이터 창을 한 번 눌러야 생겼다. 그래서 영상만 만지던 사람은
  //   파란 선을 본 적이 없고, 싱크를 눌러도 "맞출 자리가 없다"고 거절당했다.
  //   거절당했으니 안 물리고, 안 물렸으니 영상이 돌아도 데이터가 안 움직였다.
  //
  //   늘 어딘가는 가리키고 있어야 한다. 처음엔 세션 첫머리다.
  if (pinMs === null || pinMs < fullSpan.from || pinMs > fullSpan.to) {
    pinMs = fullSpan.from;
  }
}

function clampView() {
  const min = 200;                                  // 0.2초보다 더는 못 당긴다
  let span = view.to - view.from;
  if (span < min) { view.to = view.from + min; span = min; }
  const full = fullSpan.to - fullSpan.from;
  if (span > full) { view = { ...fullSpan }; return; }
  if (view.from < fullSpan.from) { view.from = fullSpan.from; view.to = view.from + span; }
  if (view.to > fullSpan.to) { view.to = fullSpan.to; view.from = view.to - span; }
}

function redraw() {
  const c = canvas();
  // 꺼진 칸은 크기가 0 이라 그릴 것도 없다
  if (c.clientWidth < 2 || c.clientHeight < 2) return;
  tl.draw({ canvas: c, series, view, marks, cursorMs, pinMs, full: fullSpan,
            originMs: originMs() });
  const span = view.to - view.from;
  // 0.2초를 보고 있는데 "0:00 보는 중" 이라고 하면 아무 말도 안 하는 셈이다.
  const spanText = span < 10000
    ? tl.formatOffsetTick(span).replace(/^[+−]/, "")
    : tl.formatDuration(span);
  const o = originMs();
  $("range").textContent = session
    ? `${tl.formatDuration(view.from - o)} ~ ${tl.formatDuration(view.to - o)}` +
      `  (${spanText} 보는 중)` +
      (timeOrigin === "video" ? "  · 영상 기준" : "")
    : "";
  renderReadout();
}

/** 커서가 놓인 시각의 값을 숫자로 보여준다. 그래프만으로는 못 읽는다. */
function renderReadout() {
  // 고정한 자리가 있으면 그걸 보여준다. 마우스를 떼도 숫자가 남아야 한다.
  const at = pinMs !== null ? pinMs : cursorMs;
  if (!session || at === null) { $("readout").textContent = ""; return; }
  const parts: string[] = [
    (pinMs !== null ? "📍 " : "") + tl.formatDuration(at - originMs()),
  ];
  for (const s of series) {
    const i = nearest(s.xs, at);
    if (i < 0) continue;
    const v = s.ys[i];
    parts.push(
      `<b style="color:${s.color}">■</b> ${s.code} ` +
      (Number.isFinite(v) ? `${v.toFixed(2)}${s.unit}` : "—")
    );
  }
  // 가까운 마킹이 있으면 알려 준다. 주황 점선이 뭔지 물어볼 일이 없게.
  const near = marks.findIndex((m) => Math.abs(m.ms - at) < (view.to - view.from) * 0.01);
  if (near >= 0) {
    parts.splice(1, 0,
      `<b style="color:#f0a020">⚑ ${near + 1}${marks[near].note ? " " + esc(marks[near].note) : ""}</b>`);
  }
  $("readout").innerHTML = parts.join("　");
}

function nearest(xs: Float64Array, x: number): number {
  if (!xs.length) return -1;
  let lo = 0, hi = xs.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (xs[mid] < x) lo = mid + 1;
    else hi = mid;
  }
  if (lo > 0 && Math.abs(xs[lo - 1] - x) < Math.abs(xs[lo] - x)) return lo - 1;
  return lo;
}

// ── 보드에서 받기 ───────────────────────────────────────────────────────

interface FileInfo {
  name: string; size: number; ok: boolean; closed?: boolean;
  session?: number; module?: string; utc_start?: number;
  duration_s?: number; nav_rows?: number; imu_rows?: number;
  dropped?: number; fixed?: boolean;
}

// 앱에서는 Tauri 의 http 를 쓴다. 브라우저에서는 그게 없으니 보통 fetch 로
// 넘어간다 — 만드는 동안 보드 없이 화면을 시험할 수 있게 하려는 것이다.
// (앱에서 Tauri 것을 쓰는 이유는 브라우저의 출처 제한을 안 받기 때문이다)
const inApp = typeof (globalThis as any).__TAURI_INTERNALS__ !== "undefined";
const netFetch: typeof fetch = inApp ? (tfetch as unknown as typeof fetch) : fetch;

function boardUrl(): string {
  const v = ($("host") as HTMLInputElement).value.trim();
  return v.startsWith("http") ? v.replace(/\/$/, "") : `http://${v || "192.168.4.1"}`;
}

async function listBoard() {
  setStatus("보드에 물어보는 중…");
  // 보드가 파일마다 머리글을 읽어야 해서 몇 초 걸린다. 그동안 표시를 둔다.
  setProgress("보드에서 파일 목록을 받는 중…");
  try {
    const r = await netFetch(`${boardUrl()}/api/files`, { method: "GET" });
    const j = (await r.json()) as { ok: boolean; files: FileInfo[] };
    boardFiles = j.files ?? [];
    renderSide();
    setProgress(null);
    setStatus(`파일 ${j.files?.length ?? 0}개 — 받을 것을 고르세요`, "good");
  } catch (e) {
    setProgress(null);
    const why = e instanceof TypeError
      ? "주소가 맞는지, 보드 WiFi 에 붙어 있는지 보세요"
      : String(e);
    setStatus(`보드에 못 붙었습니다 — ${why}`, "bad");
  }
}

let boardFiles: FileInfo[] = [];

// ── 왼쪽 칸 ─────────────────────────────────────────────────────────────
//
// 보관함이 기본이다. 코치는 대개 이미 받아 둔 걸 본다. 보드에서 받는 건
// 훈련이 끝난 직후 한 번뿐이다.
function renderSide() {
  ($("tabLib") as HTMLElement).className = pane === "library" ? "tab on" : "tab";
  ($("tabBoard") as HTMLElement).className = pane === "board" ? "tab on" : "tab";
  ($("boardBar") as HTMLElement).style.display = pane === "board" ? "flex" : "none";
  ($("libBar") as HTMLElement).style.display = pane === "library" ? "flex" : "none";
  if (pane === "board") renderFileList(boardFiles);
  else renderLibrary();
}

const dayText = (t: number) =>
  new Date(t * 1000).toLocaleDateString("ko-KR", { month: "long", day: "numeric", weekday: "short" });
const timeText = (t: number) =>
  new Date(t * 1000).toLocaleTimeString("ko-KR", { hour: "2-digit", minute: "2-digit" });

function renderLibrary() {
  const box = $("files");
  const hits = lib.search(library.entries, query);
  if (!hits.length) {
    box.innerHTML = library.entries.length
      ? "<div class='dim pad'>찾는 게 없습니다.</div>"
      : "<div class='dim pad'>아직 받은 훈련이 없습니다.<br>보드 칸에서 받아오세요.</div>";
    return;
  }

  box.innerHTML = lib.byDay(hits).map(({ items }) => {
    const head = `<div class="day">${dayText(items[0].utcStart || items[0].fetchedAt)}
        <span class="dim">${items.length}척</span></div>`;
    const rows = items.map((e) => {
      const when = e.utcStart ? timeText(e.utcStart) : "—";
      const dur = tl.formatDuration(e.durationS * 1000);
      const bad = !e.verified || e.dropped ? "<span class='bad'>●</span>" : "";
      const who = e.sailor || e.title || `세션 ${e.session}`;
      const sub = [e.boatClass, e.venue, e.windKn && `${e.windKn}kn`]
        .filter(Boolean).join(" · ");
      return `<div class="file ${e.id === openId ? "on" : ""}" data-id="${e.id}">
        <div>${e.starred ? "★ " : ""}<b>${esc(who)}</b> ${bad}</div>
        <div class="dim">${when} · ${dur} · ${esc(sub) || e.module.slice(-5)}</div>
      </div>`;
    }).join("");
    return head + rows;
  }).join("");

  box.querySelectorAll<HTMLElement>(".file").forEach((el) => {
    el.onclick = () => openEntry(el.dataset.id!);
  });
}

const esc = (v: string) =>
  v.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]!));

async function openEntry(id: string) {
  const e = library.entries.find((x) => x.id === id);
  if (!e) return;
  if (!(await lib.hasFile(e))) {
    setStatus("보관함에 파일이 없습니다. 다시 받아야 합니다.", "bad");
    return;
  }
  setStatus("읽는 중…");
  openId = id;
  loadBytes(await lib.readEntry(e), e.title || e.sailor || e.file);
  rebuildMarks();
  renderSide();
  renderDetails();
}

// ── 훈련 정보 적기 ──────────────────────────────────────────────────────
//
// 모듈 MAC 만으로는 누가 탔는지 모른다. 조건도 모른다. 코치가 적는다.
// **원본 파일은 안 건드린다.** 적은 것은 library.json 에만 들어간다.
const FIELDS: [keyof lib.Entry, string, string][] = [
  ["title", "제목", "화요일 오전 상승풍"],
  ["sailor", "선수", "이름"],
  ["boatClass", "클래스", "ILCA / 470 / 49er / iQFoil"],
  ["venue", "장소", "왕산 마리나"],
  ["group", "훈련 묶음", "같은 날 같이 나간 배끼리 같은 값"],
  ["windKn", "풍속", "8~12"],
  ["windDir", "풍향", "북서"],
  ["waves", "파도", "잔잔 / 1m 너울"],
  ["rig", "리그 세팅", "바텐 텐션 …"],
  ["notes", "메모", ""],
];

function renderDetails() {
  const box = $("details");
  const e = library.entries.find((x) => x.id === openId);
  if (!e) { box.innerHTML = ""; return; }

  box.innerHTML = `
    <div class="drow">
      <button id="star" class="${e.starred ? "on" : ""}">${e.starred ? "★" : "☆"}</button>
      <span class="dim">${e.module} · 세션 ${e.session} · ${(e.bytes / 1048576).toFixed(2)} MB</span>
      ${e.verified ? "<span class='good'>검사 통과</span>" : "<span class='bad'>검사 실패</span>"}
    </div>
    ${FIELDS.map(([k, label, ph]) => `
      <label class="drow"><span>${label}</span>
        <input data-k="${k}" value="${esc(String(e[k] ?? ""))}" placeholder="${ph}" />
      </label>`).join("")}`;

  box.querySelectorAll<HTMLInputElement>("input").forEach((inp) => {
    // 칠 때마다 저장한다. 저장 버튼을 따로 두면 잊고 닫는다.
    inp.onchange = () => {
      library = lib.update(library, e.id, { [inp.dataset.k!]: inp.value });
      renderSide();
    };
  });
  ($("star") as HTMLButtonElement).onclick = () => {
    library = lib.update(library, e.id, { starred: !e.starred });
    renderDetails();
    renderSide();
  };
}

function renderFileList(files: FileInfo[]) {
  const box = $("files");
  if (!files.length) {
    box.innerHTML = "<div class='dim pad'>보드에서 목록을 받아오세요.</div>";
    return;
  }

  const have = new Set(library.entries.map((e) => e.id));
  box.innerHTML = files
    .map((f) => {
      const when = f.utc_start
        ? new Date(f.utc_start * 1000).toLocaleString()
        : "<span class='dim'>위성 못 잡음</span>";
      const dur = f.duration_s ? tl.formatDuration(f.duration_s * 1000) : "?";
      const bad = f.dropped ? `<span class='bad'>버림 ${f.dropped}</span>` : "";
      const notClosed = f.closed === false ? "<span class='bad'>안 닫힘</span>" : "";
      // 이미 받은 것은 흐리게. 같은 걸 두 번 받을 이유가 없다.
      const mine = f.module && f.session !== undefined
        && have.has(lib.entryId(f.module, f.session));
      return `<div class="file ${mine ? "got" : ""}" data-name="${f.name}" data-size="${f.size}">
        <div><b>${f.name}</b> <span class="dim">${(f.size / 1048576).toFixed(2)} MB</span>
          ${mine ? "<span class='good'>받음</span>" : ""}</div>
        <div class="dim">세션 ${f.session ?? "?"} · ${when} · ${dur} ${bad} ${notClosed}</div>
        <button class="get">${mine ? "다시 받기" : "받기"}</button>
      </div>`;
    })
    .join("");

  // 줄을 누르는 것만으로는 안 받는다. 90 MB 를 실수로 받으면 안 된다.
  box.querySelectorAll<HTMLElement>(".file .get").forEach((btn) => {
    btn.onclick = (e) => {
      e.stopPropagation();
      const el = (btn as HTMLElement).closest<HTMLElement>(".file")!;
      fetchFile(el.dataset.name!, Number(el.dataset.size) || undefined);
    };
  });
}

const MB = (n: number) => (n / 1048576).toFixed(2);

let fetching = false;

async function fetchFile(name: string, size?: number) {
  if (fetching) { setStatus("이미 하나 받는 중입니다.", "bad"); return; }
  fetching = true;
  document.querySelectorAll<HTMLButtonElement>(".file .get")
    .forEach((b) => { b.disabled = true; });
  setStatus(`${name} 받는 중…`);
  setProgress(size ? `${name}  0 / ${MB(size)} MB` : `${name} 받는 중…`, size ? 0 : null);
  const t0 = performance.now();
  try {
    const r = await netFetch(`${boardUrl()}/file/${name}`, { method: "GET" });
    if (!r.ok) {
      setProgress(null);
      setStatus(`받기 실패 — HTTP ${r.status}`, "bad");
      return;
    }

    const total = Number(r.headers.get("content-length")) || size || 0;
    let buf: Uint8Array;

    // 조각조각 받으면서 얼마나 왔는지 보여준다. 90 MB 를 아무 말 없이
    // 기다리게 두면 안 된다.
    if (r.body) {
      const reader = r.body.getReader();
      const chunks: Uint8Array[] = [];
      let got = 0;
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        got += value.length;
        setProgress(
          `${name}  ${MB(got)} / ${total ? MB(total) + " MB" : "?"}` +
          `  ·  ${(got / 1024 / ((performance.now() - t0) / 1000)).toFixed(0)} KB/초`,
          total ? got / total : null,
        );
      }
      buf = new Uint8Array(got);
      let at = 0;
      for (const ch of chunks) { buf.set(ch, at); at += ch.length; }
    } else {
      buf = new Uint8Array(await r.arrayBuffer());
    }

    const sec = (performance.now() - t0) / 1000;
    setProgress(`${name} 검사하는 중…`);
    await intake(buf, name);
    setProgress(null);
    setStatus(
      `${name} 받았습니다 — ${MB(buf.length)} MB, ${sec.toFixed(1)}초 ` +
      `(${(buf.length / 1024 / sec).toFixed(0)} KB/초)`, "good");
  } catch (e) {
    setProgress(null);
    setStatus(`받기 실패 — ${e}`, "bad");
  } finally {
    fetching = false;
    document.querySelectorAll<HTMLButtonElement>(".file .get")
      .forEach((b) => { b.disabled = false; });
  }
}

// ── 칸 켜고 끄기 ────────────────────────────────────────────────────────
//
// 네 칸이 다 켜져 있으면 각자 좁다. 영상만 크게 보거나 데이터만 크게 보고
// 싶을 때가 많다. 그래서 각각 끈다. 끈 칸의 자리는 남은 칸이 나눠 쓴다.
//
// 마지막 하나까지 끄면 볼 게 없어진다. 그건 막는다.
type PaneKey = "lib" | "video" | "data" | "info";
const PANE_EL: Record<PaneKey, string> = {
  lib: "left", video: "videoPane", data: "dataPane", info: "right",
};
const LAYOUT_KEY = "layout.v1";

let layout: Record<PaneKey, boolean> = {
  lib: true, video: true, data: true, info: true,
};

const PANE_NAME: Record<PaneKey, string> = {
  lib: "보관함", video: "영상", data: "데이터", info: "정보",
};

/**
 * 칸마다 제 손잡이를 달아 준다.
 *
 * 위 띠의 단추만 있으면 "이 칸을 크게" 하려고 눈이 위로 갔다가 다시 내려와야
 * 한다. 크게 하고 싶은 칸을 보고 있는데 손잡이는 저 위에 있는 셈이다.
 * 그래서 칸 자체의 오른쪽 위에 붙인다.
 *
 *   ⤢  이 칸만 크게 (다시 누르면 넷 다)
 *   ✕  이 칸 끄기
 */
function mountPaneHandles() {
  (Object.keys(PANE_EL) as PaneKey[]).forEach((k) => {
    const el = $(PANE_EL[k]);
    if (el.querySelector(".handles")) return;
    el.classList.add("hasHandles");

    const box = document.createElement("div");
    box.className = "handles";

    const big = document.createElement("button");
    big.className = "big";
    big.textContent = "⤢";
    big.title = `${PANE_NAME[k]}만 크게 보기`;
    big.onclick = (e) => { e.stopPropagation(); only(k); };

    const shut = document.createElement("button");
    shut.textContent = "✕";
    shut.title = `${PANE_NAME[k]} 접기`;
    shut.onclick = (e) => { e.stopPropagation(); toggle(k); };

    box.append(big, shut);

    // 접혔을 때 남는 띠. 이름을 세로로 적어 두면 뭐가 접혔는지 보인다.
    const rail = document.createElement("button");
    rail.className = "rail";
    rail.innerHTML = `<span>▸</span><span class="railname">${PANE_NAME[k]}</span>`;
    rail.title = `${PANE_NAME[k]} 펼치기`;
    rail.onclick = () => toggle(k);

    el.append(box, rail);
  });
}

function applyLayout() {
  const alone = (Object.keys(layout) as PaneKey[]).filter((x) => layout[x]).length === 1;

  (Object.keys(PANE_EL) as PaneKey[]).forEach((k) => {
    const el = $(PANE_EL[k]);
    // ★ 접은 칸을 아주 없애지 않는다. 얇은 띠로 남긴다.
    //   완전히 사라지면 어디로 갔는지, 어떻게 되돌리는지 알 수가 없다.
    el.classList.toggle("mini", !layout[k]);

    const big = el.querySelector<HTMLButtonElement>(".handles .big");
    if (big) {
      big.textContent = alone ? "⤡" : "⤢";
      big.title = alone ? "넷 다 펼치기" : `${PANE_NAME[k]}만 크게 보기`;
    }
  });

  // 가운데 두 칸 중 하나가 접히면 나누개를 접고 남은 칸이 다 쓴다
  const solo = !(layout.video && layout.data);
  $("center").classList.toggle("solo", solo);
  if (solo) {
    $("videoPane").style.flex = "";
    $("dataPane").style.flex = "";
  }

  localStorage.setItem(LAYOUT_KEY, JSON.stringify(layout));
  redraw();
}

function toggle(k: PaneKey) {
  const on = (Object.keys(layout) as PaneKey[]).filter((x) => layout[x]);
  if (layout[k] && on.length === 1) {
    // 마지막 하나까지 접으면 볼 게 없어진다. 그건 막는다 —
    // 다만 띠가 남아 있으니 사람이 어떻게 되돌리는지는 보인다.
    setStatus("마지막 칸입니다. 다른 칸을 먼저 펼치세요.", "bad");
    return;
  }
  layout = { ...layout, [k]: !layout[k] };
  applyLayout();
}

/**
 * 이 칸만 크게. 나머지를 다 끈다.
 *
 * "영상만 크게 보고 싶다" 가 제일 흔한데, 하나씩 끄면 세 번을 눌러야 한다.
 * 한 번에 되게 한다. 이미 혼자면 넷 다 켜서 되돌린다.
 */
function only(k: PaneKey) {
  const on = (Object.keys(layout) as PaneKey[]).filter((x) => layout[x]);
  if (on.length === 1 && on[0] === k) {
    layout = { lib: true, video: true, data: true, info: true };
    setStatus("넷 다 켰습니다.");
  } else {
    layout = { lib: false, video: false, data: false, info: false, [k]: true };
    // 손잡이(⤢)는 한 번, 위 띠 단추는 두 번이다. 둘 다 맞는 말로 적는다.
    setStatus(`${PANE_NAME[k]}만 크게 봅니다. 옆의 얇은 띠를 누르면 다시 펼쳐집니다.`);
  }
  applyLayout();
}

function loadLayout() {
  mountPaneHandles();
  try {
    const j = localStorage.getItem(LAYOUT_KEY);
    if (j) {
      const l = JSON.parse(j);
      if (l && typeof l === "object") {
        layout = { ...layout, ...l };
        // 다 꺼진 상태로 저장돼 있으면 되돌린다
        if (!Object.values(layout).some(Boolean)) {
          layout = { lib: true, video: true, data: true, info: true };
        }
      }
    }
  } catch { /* 처음이면 없는 게 정상이다 */ }
  applyLayout();
}

// ── 영상 ────────────────────────────────────────────────────────────────
//
// 왼쪽 반은 영상, 오른쪽 반은 데이터다. 둘의 시각을 맞춰 놓으면 태킹하는
// 순간의 힐 값과 그때 화면이 같이 보인다.
//
// 고프로·DJI 가 적어 둔 시각은 틀린 경우가 잦아서 (시계를 안 맞췄거나
// 시간대를 지역 시각으로 적거나) **첫 짐작으로만** 쓰고 사람이 맞춘다.

const video = () => $("vid") as HTMLVideoElement;
/** 슬라이더를 사람이 끌고 있나. 그동안은 코드가 값을 안 건드린다 */
let sliderHeld = false;

async function openVideo() {
  const path = await open({
    multiple: false,
    filters: [{ name: "영상", extensions: ["mp4", "MP4", "mov", "MOV", "m4v"] }],
  });
  if (!path || typeof path !== "string") return;
  await useVideoUrl(convertFileSrc(path), path.split("/").pop() ?? path,
                    await readFile(path).then((b) => new Blob([new Uint8Array(b)])));
}

async function useVideoUrl(url: string, name: string, blob: Blob | null) {
  const v = video();
  v.src = url;
  v.preload = "auto";
  v.load();
  v.classList.add("on");
  $("vdrop").classList.add("hide");
  videoOn = true;
  linked = false;             // 사람이 눌러 확인하기 전에는 따로 논다
  timeOrigin = "session";

  // 파일이 적어 둔 시각으로 첫 짐작을 만든다
  let ft: Date | null = null;
  if (blob) { try { ft = await vid.mp4CreationTime(blob); } catch { /* 못 읽어도 된다 */ } }
  const first = session?.imu[0]?.ms ?? session?.nav[0]?.ms ?? 0;
  sync = vid.guessOffset(ft, session?.header.utcStart ?? 0, first);

  setStatus(
    ft
      ? `${name} — 파일이 적어 둔 시각 ${ft.toLocaleString()}${sync.guessed ? " 로 맞춰 봤습니다" : ""}. 어긋나면 아래에서 맞추세요.`
      : `${name} — 파일에 시각이 없습니다. 아래에서 맞추세요.`,
  );
  renderVbar();
}

function renderVbar() {
  const v = video();
  const dur = Number.isFinite(v.duration) ? v.duration : 0;
  $("vtime").textContent =
    `${tl.formatDuration(v.currentTime * 1000)} / ${tl.formatDuration(dur * 1000)}`;

  const sb = $("syncBtn");
  sb.textContent = linked ? "싱크 ●" : "싱크";
  sb.className = linked ? "on" : "";

  const sl = $("vslider") as HTMLInputElement;
  sl.disabled = !videoOn || dur <= 0;
  // 사람이 끌고 있는 동안에는 건드리지 않는다. 손이 튕겨 나간다.
  if (!sliderHeld && dur > 0) sl.value = String(Math.round((v.currentTime / dur) * 1000));
  const off = $("voff");
  if (!videoOn) {
    off.textContent = "";
    off.className = "mono dim";
  } else if (linked) {
    off.textContent = `같이 움직임 · 영상 0초 = ${vid.formatOffset(sync.offsetMs)}`;
    off.className = "mono good";
  } else {
    // 안 물렸으면 그 사실을 먼저 말한다. 그래야 왜 따로 노는지 안다.
    off.textContent =
      "따로 움직임 — 같은 순간에 두고 [싱크]" +
      (sync.guessed ? `  (짐작 ${vid.formatOffset(sync.offsetMs)})` : "");
    off.className = "mono bad";
  }
  ($("play") as HTMLButtonElement).textContent = v.paused ? "▶" : "⏸";
}

/** 타임라인 커서 → 영상 위치 */
function seekVideoTo(sessionMs: number) {
  if (!videoOn || !linked) return;   // 맞추기 전에는 따로 논다
  const v = video();
  const t = vid.sessionToVideo(sync, sessionMs);
  if (t < 0 || t > (v.duration || Infinity)) return;
  if (Math.abs(v.currentTime - t) < 0.03) return;
  seeking = true;
  v.currentTime = t;
}

function nudge(ms: number) {
  sync = { ...sync, offsetMs: sync.offsetMs + ms, guessed: false };
  renderVbar();
  redraw();
}

/** 지금 영상 화면이 커서 자리라고 알려 준다. 눈으로 맞추는 길이다. */
/**
 * 싱크 단추 하나로 다 한다.
 *
 *   안 물렸을 때 누르면   지금 영상 화면과 타임라인 자리를 맞추고 물린다
 *   물렸을 때 누르면      푼다 (따로 움직인다)
 *
 * 전에는 `여기 맞춤` · `물림 풀기` · `시각 기준` 세 개였다. 하는 일이 결국
 * 하나라 셋으로 나눌 이유가 없었다.
 */
function toggleSync() {
  if (!videoOn) { setStatus("영상을 먼저 여세요.", "bad"); return; }

  if (linked) {
    linked = false;
    timeOrigin = "session";
    setStatus("싱크를 풀었습니다. 영상과 데이터가 따로 움직입니다.");
    renderVbar();
    redraw();
    return;
  }

  // 파란 선은 늘 있으므로 여기서 없을 일이 없다. 그래도 막아 둔다.
  const at = pinMs ?? cursorMs ?? view.from;
  sync = { ...sync, offsetMs: at - video().currentTime * 1000, guessed: false };
  linked = true;
  timeOrigin = "video";      // 이제 두 숫자가 같아진다
  setStatus("싱크 맞췄습니다. 같이 움직입니다.", "good");
  renderVbar();
  redraw();
}

// ── 붙이기 ──────────────────────────────────────────────────────────────

/** 시험용 데이터. 보드가 없어도 화면을 볼 수 있게 넣어 둔다. */
async function loadSample() {
  setStatus("시험용 데이터를 읽는 중…");
  const r = await fetch("/sample.HLG");
  await intake(new Uint8Array(await r.arrayBuffer()), "sample.HLG (시험용)");

  // 시험용 영상도 있으면 같이 연다. 만드는 동안 싱크를 눈으로 보려는 것이다.
  try {
    const vr = await fetch("/sample.mp4");
    if (vr.ok) {
      const blob = await vr.blob();
      await useVideoUrl(URL.createObjectURL(blob), "sample.mp4 (시험용)", blob);
    }
  } catch { /* 없으면 그만이다 */ }
}

function wire() {
  $("open").onclick = openFile;
  $("sample").onclick = loadSample;
  $("list").onclick = listBoard;
  $("fit").onclick = () => { view = { ...fullSpan }; redraw(); };
  $("openVideo").onclick = openVideo;

  // ── 영상 조작 ──────────────────────────────────────────────────────
  const v = video();
  $("play").onclick = () => { v.paused ? v.play() : v.pause(); renderVbar(); };

  // 영상 슬라이더. 타임라인 없이 영상만 훑을 때 쓴다.
  const sl = $("vslider") as HTMLInputElement;
  const slSeek = () => {
    const dur = Number.isFinite(v.duration) ? v.duration : 0;
    if (dur <= 0) {
      setStatus("영상을 아직 다 읽지 못했습니다.", "bad");
      return;
    }
    sliderHeld = true;          // 끄는 동안 코드가 값을 안 건드리게
    v.currentTime = (Number(sl.value) / 1000) * dur;
    // 물려 있을 때만 타임라인이 따라온다. 맞추기 전에는 영상만 움직인다.
    if (linked) pinMs = vid.videoToSession(sync, v.currentTime);
    renderVbar();
    redraw();
  };
  // input 은 끄는 내내, change 는 놓을 때 온다. 둘 다 받는다 —
  // 웹뷰마다 어느 쪽이 오는지가 다르다.
  sl.addEventListener("input", slSeek);
  sl.addEventListener("change", () => { slSeek(); sliderHeld = false; });
  addEventListener("pointerup", () => { sliderHeld = false; });
  addEventListener("keyup", () => { sliderHeld = false; });
  $("syncBtn").onclick = toggleSync;
  // 누른 부호대로 아래 숫자가 움직인다. 반대로 두면 사람이 헷갈린다.
  $("n10").onclick = () => nudge(-10000);
  $("n1").onclick = () => nudge(-1000);
  $("p1").onclick = () => nudge(1000);
  $("p10").onclick = () => nudge(10000);

  // ── 영상이 돌면 타임라인이 따라간다 ──────────────────────────────
  //
  // ★ requestVideoFrameCallback 만 쓰면 안 된다.
  //
  //   그건 **새 프레임이 나올 때만** 불린다. 그래서 영상을 한 번 세우면
  //   다시는 안 불리고, 그 뒤로는 재생해도 타임라인이 안 따라온다.
  //   (다음 부름을 콜백 안에서 걸어 두는 구조라 한 번 끊기면 영영 끊긴다)
  //
  // 그래서 재생이 시작될 때마다 다시 건다. timeupdate 도 함께 받는다 —
  // 초당 네 번쯤이라 성기지만, 프레임 콜백이 없는 경우의 안전망이다.
  let rvfcArmed = false;
  const follow = () => {
    rvfcArmed = false;
    syncFromVideo();
    if (!v.paused) armFollow();
  };
  function armFollow() {
    if (rvfcArmed) return;
    if ("requestVideoFrameCallback" in v) {
      rvfcArmed = true;
      (v as any).requestVideoFrameCallback(follow);
    }
  }
  /** 지금 영상 자리를 타임라인에 옮긴다 */
  function syncFromVideo() {
    if (linked) {
      pinMs = vid.videoToSession(sync, v.currentTime);
      cursorMs = pinMs;
      // 커서가 화면 밖으로 나가면 따라 밀어 준다
      if (pinMs < view.from || pinMs > view.to) {
        const span = view.to - view.from;
        view.from = pinMs - span * 0.3;
        view.to = view.from + span;
        clampView();
      }
    }
    renderVbar();
    redraw();
  }
  v.addEventListener("play", () => { armFollow(); renderVbar(); });
  v.addEventListener("timeupdate", syncFromVideo);
  armFollow();

  v.addEventListener("seeked", () => { seeking = false; syncFromVideo(); });
  v.addEventListener("pause", renderVbar);
  v.addEventListener("loadedmetadata", renderVbar);

  // 끌어다 놓기
  const drop = $("vdrop");
  const wrap = drop.parentElement!;
  ["dragenter", "dragover"].forEach((k) =>
    wrap.addEventListener(k, (e) => { e.preventDefault(); drop.classList.add("over"); }));
  ["dragleave", "drop"].forEach((k) =>
    wrap.addEventListener(k, () => drop.classList.remove("over")));
  wrap.addEventListener("drop", async (e) => {
    e.preventDefault();
    const f = (e as DragEvent).dataTransfer?.files?.[0];
    if (f) await useVideoUrl(URL.createObjectURL(f), f.name, f);
  });

  // 가운데 나누개
  const sp = $("split");
  let spDrag = false;
  sp.addEventListener("pointerdown", (e) => {
    spDrag = true; sp.setPointerCapture((e as PointerEvent).pointerId);
  });
  sp.addEventListener("pointermove", (e) => {
    if (!spDrag) return;
    const box = $("center").getBoundingClientRect();
    const f = Math.min(0.8, Math.max(0.2, ((e as PointerEvent).clientX - box.left) / box.width));
    $("videoPane").style.flex = `1 1 ${f * 100}%`;
    $("dataPane").style.flex = `1 1 ${(1 - f) * 100}%`;
    redraw();
  });
  sp.addEventListener("pointerup", () => { spDrag = false; });

  $("tabLib").onclick = () => { pane = "library"; renderSide(); };
  $("tabBoard").onclick = () => { pane = "board"; renderSide(); };
  ($("q") as HTMLInputElement).oninput = (e) => {
    query = (e.target as HTMLInputElement).value;
    renderLibrary();
  };

  const c = canvas();

  // ── 확대·축소와 밀기 — Saleae Logic 과 같은 손맛 ──────────────────────
  //
  //   휠(또는 트랙패드 위아래)  커서가 놓인 시각을 붙잡고 확대·축소
  //   트랙패드 좌우             그대로 밀기
  //   시프트 + 휠               밀기
  //   끌기                      밀기
  //   아래 띠를 클릭·끌기       그 자리로 건너뛰기
  //   화살표 ← →                밀기 (시프트를 누르면 크게)
  //   + −                       확대·축소
  //   F / 0                     전체 보기
  //
  // 확대할 때 **커서 아래 시각이 제자리에 있어야** 손맛이 난다. 가운데를
  // 기준으로 확대하면 보고 있던 곳이 화면 밖으로 달아난다.
  function zoomAt(atMs: number, factor: number) {
    view.from = atMs - (atMs - view.from) * factor;
    view.to = atMs + (view.to - atMs) * factor;
    clampView();
  }
  function panBy(ms: number) {
    view.from += ms;
    view.to += ms;
    clampView();
  }

  c.addEventListener("wheel", (e) => {
    if (!session) return;
    e.preventDefault();

    // 트랙패드 좌우 밀기. 맥에서 두 손가락으로 옆으로 쓸면 이게 온다.
    if (Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
      panBy(((view.to - view.from) * e.deltaX) / 400);
      redraw();
      return;
    }
    if (e.shiftKey) {
      panBy(((view.to - view.from) * e.deltaY) / 400);
    } else {
      zoomAt(tl.msAtX(c, view, e.clientX), Math.exp(e.deltaY / 260));
    }
    redraw();
  }, { passive: false });

  // 끌기 — 본 화면이면 밀기, 아래 띠면 그 자리로
  type Drag = { kind: "pan" | "over" | "scrub" | "pill"; x: number; from: number };
  let drag: Drag | null = null;

  const inOverview = (e: PointerEvent) => {
    const [top] = tl.overviewBand(c);
    return e.clientY - c.getBoundingClientRect().top >= top;
  };

  // ── 이름 고치기 ────────────────────────────────────────────────────
  //
  // 이름 칸을 누르면 그 자리에 입력칸을 띄운다. 캔버스 위에 얹는 것이라
  // 자리를 계산해서 놓아야 한다.
  let editing: HTMLInputElement | null = null;
  function closeEdit(commit: boolean) {
    if (!editing) return;
    const el = editing;
    editing = null;
    if (commit) renameSeries(el.dataset.code!, el.value);
    el.remove();
  }
  function editLabel(row: number) {
    closeEdit(true);
    const s = series[row];
    if (!s) return;
    const box = tl.labelBox(c, series.length, row);
    const inp = document.createElement("input");
    inp.className = "labelEdit";
    inp.value = s.name;
    inp.dataset.code = s.code;
    inp.style.left = `${box.left}px`;
    inp.style.top = `${box.top}px`;
    inp.style.width = `${box.width}px`;
    inp.onkeydown = (ev) => {
      if (ev.key === "Enter") { ev.preventDefault(); closeEdit(true); }
      if (ev.key === "Escape") { ev.preventDefault(); closeEdit(false); }
      ev.stopPropagation();      // 화살표·스페이스가 타임라인으로 새지 않게
    };
    inp.onblur = () => closeEdit(true);
    c.parentElement!.append(inp);
    editing = inp;
    inp.focus();
    inp.select();
  }

  // ── 깃발 고치기 ────────────────────────────────────────────────────
  //
  // 깃발을 누르면 그 옆에 작은 창이 뜬다. 메모를 적거나 지운다.
  let flagPanel: HTMLElement | null = null;
  function closeFlag() { flagPanel?.remove(); flagPanel = null; }

  function editFlag(i: number) {
    closeFlag();
    const m = marks[i];
    const box = tl.flagBox(i);
    if (!m || !box) return;

    const el = document.createElement("div");
    el.className = "flagPanel";
    el.style.left = `${box.left}px`;
    el.style.top = `${box.top}px`;
    el.innerHTML = `
      <div class="frow"><b>⚑ ${i + 1}</b>
        <span class="dim">${tl.formatDuration(m.ms - originMs())}</span>
        <span class="dim">${m.from === "file" ? "배에서 찍음" : "직접 단 것"}</span>
      </div>
      <input class="fnote" placeholder="메모 (예: 태킹 좋았음)" />
      <div class="frow"><button class="fdel">지우기</button><button class="fok">확인</button></div>`;
    c.parentElement!.append(el);
    flagPanel = el;

    const inp = el.querySelector<HTMLInputElement>(".fnote")!;
    inp.value = m.note;
    inp.onkeydown = (ev) => {
      ev.stopPropagation();
      if (ev.key === "Enter") { noteMark(i, inp.value); closeFlag(); }
      if (ev.key === "Escape") closeFlag();
    };
    el.querySelector<HTMLButtonElement>(".fok")!.onclick = () => {
      noteMark(i, inp.value); closeFlag();
    };
    el.querySelector<HTMLButtonElement>(".fdel")!.onclick = () => {
      removeMark(i); closeFlag();
    };
    inp.focus();
    inp.select();
  }

  // ── 끌면 훑는다 ────────────────────────────────────────────────────
  //
  // 그림 위를 끌면 고정 자리가 따라오고 영상도 같이 훑어진다. 영상 편집기
  // 에서 재생 머리를 끄는 것과 같다.
  //
  // 밀기(pan)는 그림 위에서 안 한다. 이미 길이 셋이나 있다.
  //   시프트 + 휠 / 트랙패드 좌우 / 아래 스크롤 막대 / 위 시간 축 끌기
  // 그림 위까지 밀기로 쓰면 정작 훑을 데가 없어진다.
  let moved = 0;
  // 이름 칸 경계를 끌면 넓어진다. 이름이 길면 넘치기 때문이다.
  let edgeDrag = false;

  c.addEventListener("pointerdown", (e) => {
    if (!session) return;
    if (tl.onLabelEdge(c, e.clientX)) {
      edgeDrag = true;
      c.setPointerCapture(e.pointerId);
      return;
    }
    // 깃발을 누르면 고치기 창
    const fi = tl.flagAt(c, e.clientX, e.clientY);
    if (fi >= 0) { editFlag(fi); return; }
    closeFlag();

    // 파란 알약을 잡으면 고정 자리를 끌어 옮긴다
    if (tl.onPinPill(c, e.clientX, e.clientY)) {
      drag = { kind: "pill", x: e.clientX, from: view.from };
      c.setPointerCapture(e.pointerId);
      c.style.cursor = "grabbing";
      return;
    }

    // 왼쪽 이름 칸이면 이름 고치기다. 그림을 건드리는 게 아니다.
    const row = tl.rowAtY(c, series.length, e.clientX, e.clientY);
    if (row >= 0) { editLabel(row); return; }
    closeEdit(true);
    c.setPointerCapture(e.pointerId);
    moved = 0;
    if (inOverview(e)) {
      drag = { kind: "over", x: e.clientX, from: view.from };
      c.style.cursor = "grabbing";      // 잡고 있는 동안은 쥔 손
      jumpTo(tl.msAtOverviewX(c, fullSpan, e.clientX));
      redraw();
    } else if (tl.onTimeAxis(c, e.clientY)) {
      // 위 시간 축을 끌면 화면이 밀린다
      drag = { kind: "pan", x: e.clientX, from: view.from };
      c.style.cursor = "grabbing";
    } else {
      // 그림 위를 끌면 훑는다
      drag = { kind: "scrub", x: e.clientX, from: view.from };
      pinMs = tl.msAtX(c, view, e.clientX);
      seekVideoTo(pinMs);
      redraw();
    }
  });

  function jumpTo(centerMs: number) {
    const span = view.to - view.from;
    view.from = centerMs - span / 2;
    view.to = view.from + span;
    clampView();
  }

  c.addEventListener("pointermove", (e) => {
    if (!session) return;
    if (edgeDrag) {
      tl.setLabelWidth(e.clientX - c.getBoundingClientRect().left);
      localStorage.setItem(LABELW_KEY, String(tl.labelWidth()));
      c.style.cursor = "col-resize";
      redraw();
      return;
    }
    if (drag?.kind === "over") {
      jumpTo(tl.msAtOverviewX(c, fullSpan, e.clientX));
      cursorMs = null;
      c.style.cursor = "grabbing";
    } else if (drag?.kind === "pill") {
      // 알약을 끌면 고정 자리가 따라온다. 영상도 (물려 있으면) 같이 간다.
      pinMs = tl.msAtX(c, view, e.clientX);
      cursorMs = pinMs;
      seekVideoTo(pinMs);
    } else if (drag?.kind === "scrub") {
      pinMs = tl.msAtX(c, view, e.clientX);
      cursorMs = pinMs;
      seekVideoTo(pinMs);
    } else if (drag?.kind === "pan") {
      moved = Math.max(moved, Math.abs(e.clientX - drag.x));
      const rect = c.getBoundingClientRect();
      const plotW = rect.width - tl.plotLeft() - 8;
      const span = view.to - view.from;
      view.from = drag.from + ((drag.x - e.clientX) / plotW) * span;
      view.to = view.from + span;
      clampView();
      cursorMs = tl.msAtX(c, view, e.clientX);
    } else {
      const over = inOverview(e);
      const edge = tl.onLabelEdge(c, e.clientX);
      const pill = tl.onPinPill(c, e.clientX, e.clientY);
      const flag = tl.flagAt(c, e.clientX, e.clientY) >= 0;
      const axis = !pill && tl.onTimeAxis(c, e.clientY);
      const onLabel = !edge && tl.rowAtY(c, series.length, e.clientX, e.clientY) >= 0;
      cursorMs = over || onLabel || edge || axis || pill || flag
        ? null : tl.msAtX(c, view, e.clientX);
      c.style.cursor = edge ? "col-resize"
        : pill ? "grab"                // 파란 알약은 잡아서 옮기는 손잡이
        : flag ? "pointer"             // 깃발은 눌러서 고치는 것
        : over || axis ? "grab"
        : onLabel ? "text"
        : "crosshair";
      // 물려 있고 아직 고정 안 했으면 마우스만 움직여도 영상이 따라온다.
      if (linked && pinMs === null && cursorMs !== null && video().paused) {
        seekVideoTo(cursorMs);
      }
    }
    redraw();
  });

  c.addEventListener("pointerup", (e) => {
    if (edgeDrag) { edgeDrag = false; return; }
    if (drag?.kind === "scrub") setStatus("이 자리에 고정했습니다. Esc 로 풉니다.");
    drag = null;
    c.style.cursor = inOverview(e) ? "grab"
      : tl.onTimeAxis(c, e.clientY) ? "grab" : "crosshair";
    redraw();
  });
  c.addEventListener("pointercancel", () => {
    drag = null; edgeDrag = false; c.style.cursor = "crosshair";
  });
  c.addEventListener("pointerleave", () => {
    cursorMs = null; drag = null; c.style.cursor = "crosshair"; redraw();
  });

  // 두 번 누르면 그 자리로 크게 당긴다
  c.addEventListener("dblclick", (e) => {
    if (!session || inOverview(e as unknown as PointerEvent)) return;
    zoomAt(tl.msAtX(c, view, e.clientX), 0.4);
    redraw();
  });

  addEventListener("keydown", (e) => {
    if (!session) return;
    const span = view.to - view.from;
    const step = span * (e.shiftKey ? 0.5 : 0.12);
    const mid = (view.from + view.to) / 2;
    switch (e.key) {
      case "ArrowLeft":  panBy(-step); break;
      case "ArrowRight": panBy(step); break;
      case "+": case "=": zoomAt(cursorMs ?? mid, 0.6); break;
      case "-": case "_": zoomAt(cursorMs ?? mid, 1 / 0.6); break;
      case "Home": panBy(fullSpan.from - view.from); break;
      case "End":  panBy(fullSpan.to - view.to); break;
      case "f": case "F": case "0": view = { ...fullSpan }; break;
      // 숫자만 누르면 켜고 끄기, 시프트를 같이 누르면 그 칸만 크게
      case "1": case "!": (e.shiftKey ? only : toggle)("lib"); break;
      case "2": case "@": (e.shiftKey ? only : toggle)("video"); break;
      case "3": case "#": (e.shiftKey ? only : toggle)("data"); break;
      case "4": case "$": (e.shiftKey ? only : toggle)("info"); break;
      case "m": case "M":
        if (pinMs !== null) addMarkAt(pinMs);
        break;
      case "Escape":
        // 선을 없애지는 않는다. 없으면 싱크를 걸 자리가 없어진다.
        pinMs = view.from;
        setStatus("고정을 화면 왼쪽 끝으로 옮겼습니다.");
        break;
      case " ": {
        const vv = video();
        if (videoOn) { vv.paused ? vv.play() : vv.pause(); renderVbar(); }
        break;
      }
      default: return;
    }
    e.preventDefault();
    redraw();
  });

  addEventListener("resize", redraw);
  addEventListener("dragover", (e) => e.preventDefault());
}

wire();
loadLayout();
redraw();
setStatus("파일을 열거나 보드에서 받으세요.");

lib.load().then((l) => { library = l; renderSide(); });

// 만드는 중에는 시험용 데이터를 바로 띄운다. 배포판에서는 안 그런다.
if (import.meta.env.DEV) loadSample();
