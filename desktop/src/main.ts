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
import * as tl from "./timeline";
import "./styles.css";

// ── 상태 ────────────────────────────────────────────────────────────────

let session: hlog.Session | null = null;
let series: tl.Series[] = [];
let marks: number[] = [];
let view: tl.View = { from: 0, to: 1 };
let fullSpan: tl.View = { from: 0, to: 1 };
let cursorMs: number | null = null;

const $ = (id: string) => document.getElementById(id)!;
const canvas = () => $("plot") as HTMLCanvasElement;

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
  loadBytes(new Uint8Array(bytes), path.split("/").pop() ?? path);
}

function loadBytes(buf: Uint8Array, name: string) {
  const t0 = performance.now();
  let s: hlog.Session;
  try {
    s = hlog.parse(buf);
  } catch (e) {
    setStatus(`읽을 수 없습니다 — ${e}`, "bad");
    return;
  }
  const ms = performance.now() - t0;

  session = s;
  buildSeries(s);
  renderHeader(s, name, ms, buf.length);
  fitAll();
  redraw();
}

// ── 값 묶음 만들기 ──────────────────────────────────────────────────────

function buildSeries(s: hlog.Session) {
  const t0 = s.imu.length ? s.imu[0].ms : s.nav.length ? s.nav[0].ms : 0;

  const navX = new Float64Array(s.nav.length);
  const sog = new Float32Array(s.nav.length);
  const cog = new Float32Array(s.nav.length);
  const sv = new Float32Array(s.nav.length);
  marks = [];
  for (let i = 0; i < s.nav.length; i++) {
    const r = s.nav[i];
    navX[i] = r.ms - t0;
    // 값이 없으면 NaN 으로 둔다. 0 을 넣으면 정박과 구별이 안 된다.
    sog[i] = r.sogKn ?? NaN;
    cog[i] = r.cogDeg ?? NaN;
    sv[i] = r.numSv;
    if (r.event & 0x01) marks.push(r.ms - t0);
  }

  const imuX = new Float64Array(s.imu.length);
  const heel = new Float32Array(s.imu.length);
  const pitch = new Float32Array(s.imu.length);
  const gz = new Float32Array(s.imu.length);
  const az = new Float32Array(s.imu.length);
  for (let i = 0; i < s.imu.length; i++) {
    const r = s.imu[i];
    imuX[i] = r.ms - t0;
    // 자세는 후처리로 뽑는다. 지금 보드는 쿼터니언을 안 준다 (quat_src=1).
    // 가속도 한 축에서 바로 구하는 방법은 펌웨어와 같다 (main.cpp 의 "힐과 피치").
    const [ax, ay, az_] = r.acc;
    const mag = Math.hypot(ax, ay, az_) || 1;
    heel[i] = (Math.asin(clamp(-ay / mag)) * 180) / Math.PI;
    pitch[i] = (Math.asin(clamp(az_ / mag)) * 180) / Math.PI;
    gz[i] = r.gyr[2];
    az[i] = az_;
  }

  series = [
    { name: "속도 SOG", unit: "kn", color: "#4ea1ff", xs: navX, ys: sog },
    { name: "힐 HEEL", unit: "°", color: "#ff7a59", xs: imuX, ys: heel, zeroCentered: true },
    { name: "트림 PITCH", unit: "°", color: "#ffc857", xs: imuX, ys: pitch, zeroCentered: true },
    { name: "돌아가는 속도 (yaw)", unit: "°/s", color: "#9d7bff", xs: imuX, ys: gz, zeroCentered: true },
    { name: "상하 가속 (파도)", unit: "g", color: "#5ad19a", xs: imuX, ys: az },
    { name: "침로 COG", unit: "°", color: "#77d4e8", xs: navX, ys: cog },
    { name: "위성 수", unit: "개", color: "#8a8a8a", xs: navX, ys: sv },
  ];
  void cog; void sv;
}

const clamp = (v: number) => (v > 1 ? 1 : v < -1 ? -1 : v);

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
  tl.draw({ canvas: canvas(), series, view, marks, cursorMs, full: fullSpan });
  const span = view.to - view.from;
  $("range").textContent = session
    ? `${tl.formatDuration(view.from)} ~ ${tl.formatDuration(view.to)}  (${tl.formatDuration(span)} 보는 중)`
    : "";
  renderReadout();
}

/** 커서가 놓인 시각의 값을 숫자로 보여준다. 그래프만으로는 못 읽는다. */
function renderReadout() {
  if (!session || cursorMs === null) { $("readout").textContent = ""; return; }
  const parts: string[] = [tl.formatDuration(cursorMs)];
  for (const s of series) {
    const i = nearest(s.xs, cursorMs);
    if (i < 0) continue;
    const v = s.ys[i];
    parts.push(
      `<span style="color:${s.color}">${s.name}</span> ` +
      (Number.isFinite(v) ? `${v.toFixed(2)}${s.unit}` : "—")
    );
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

function boardUrl(): string {
  const v = ($("host") as HTMLInputElement).value.trim();
  return v.startsWith("http") ? v.replace(/\/$/, "") : `http://${v || "192.168.4.1"}`;
}

async function listBoard() {
  setStatus("보드에 물어보는 중…");
  try {
    const r = await tfetch(`${boardUrl()}/api/files`, { method: "GET" });
    const j = (await r.json()) as { ok: boolean; files: FileInfo[] };
    renderFileList(j.files ?? []);
    setStatus(`파일 ${j.files?.length ?? 0}개`, "good");
  } catch (e) {
    setStatus(`보드에 못 붙었습니다 — ${e}`, "bad");
  }
}

function renderFileList(files: FileInfo[]) {
  const box = $("files");
  if (!files.length) { box.innerHTML = "<div class='dim'>파일이 없습니다.</div>"; return; }

  box.innerHTML = files
    .map((f) => {
      const when = f.utc_start
        ? new Date(f.utc_start * 1000).toLocaleString()
        : "<span class='dim'>위성 못 잡음</span>";
      const dur = f.duration_s ? tl.formatDuration(f.duration_s * 1000) : "?";
      const bad = f.dropped ? `<span class='bad'>버림 ${f.dropped}</span>` : "";
      const notClosed = f.closed === false ? "<span class='bad'>안 닫힘</span>" : "";
      return `<div class="file" data-name="${f.name}">
        <div><b>${f.name}</b> <span class="dim">${(f.size / 1048576).toFixed(2)} MB</span></div>
        <div class="dim">세션 ${f.session ?? "?"} · ${when} · ${dur} ${bad} ${notClosed}</div>
      </div>`;
    })
    .join("");

  box.querySelectorAll<HTMLElement>(".file").forEach((el) => {
    el.onclick = () => fetchFile(el.dataset.name!);
  });
}

async function fetchFile(name: string) {
  setStatus(`${name} 받는 중…`);
  try {
    const r = await tfetch(`${boardUrl()}/file/${name}`, { method: "GET" });
    if (!r.ok) { setStatus(`받기 실패 — HTTP ${r.status}`, "bad"); return; }
    const buf = new Uint8Array(await r.arrayBuffer());
    loadBytes(buf, name);
  } catch (e) {
    setStatus(`받기 실패 — ${e}`, "bad");
  }
}

// ── 붙이기 ──────────────────────────────────────────────────────────────

/** 시험용 데이터. 보드가 없어도 화면을 볼 수 있게 넣어 둔다. */
async function loadSample() {
  setStatus("시험용 데이터를 읽는 중…");
  const r = await fetch("/sample.HLG");
  loadBytes(new Uint8Array(await r.arrayBuffer()), "sample.HLG (시험용)");
}

function wire() {
  $("open").onclick = openFile;
  $("sample").onclick = loadSample;
  $("list").onclick = listBoard;
  $("fit").onclick = () => { view = { ...fullSpan }; redraw(); };

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
  type Drag = { kind: "pan" | "over"; x: number; from: number };
  let drag: Drag | null = null;

  const inOverview = (e: PointerEvent) => {
    const [top] = tl.overviewBand(c);
    return e.clientY - c.getBoundingClientRect().top >= top;
  };

  c.addEventListener("pointerdown", (e) => {
    if (!session) return;
    c.setPointerCapture(e.pointerId);
    if (inOverview(e)) {
      drag = { kind: "over", x: e.clientX, from: view.from };
      jumpTo(tl.msAtOverviewX(c, fullSpan, e.clientX));
      redraw();
    } else {
      drag = { kind: "pan", x: e.clientX, from: view.from };
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
    if (drag?.kind === "over") {
      jumpTo(tl.msAtOverviewX(c, fullSpan, e.clientX));
      cursorMs = null;
    } else if (drag?.kind === "pan") {
      const rect = c.getBoundingClientRect();
      const plotW = rect.width - tl.PLOT_LEFT - 8;
      const span = view.to - view.from;
      view.from = drag.from + ((drag.x - e.clientX) / plotW) * span;
      view.to = view.from + span;
      clampView();
      cursorMs = tl.msAtX(c, view, e.clientX);
    } else {
      cursorMs = inOverview(e) ? null : tl.msAtX(c, view, e.clientX);
    }
    redraw();
  });

  const endDrag = () => { drag = null; };
  c.addEventListener("pointerup", endDrag);
  c.addEventListener("pointercancel", endDrag);
  c.addEventListener("pointerleave", () => { cursorMs = null; drag = null; redraw(); });

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
      default: return;
    }
    e.preventDefault();
    redraw();
  });

  addEventListener("resize", redraw);
  addEventListener("dragover", (e) => e.preventDefault());
}

wire();
redraw();
setStatus("파일을 열거나 보드에서 받으세요.");

// 만드는 중에는 시험용 데이터를 바로 띄운다. 배포판에서는 안 그런다.
if (import.meta.env.DEV) loadSample();
