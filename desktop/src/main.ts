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
let marks: number[] = [];
let view: tl.View = { from: 0, to: 1 };
let fullSpan: tl.View = { from: 0, to: 1 };
let cursorMs: number | null = null;

// 보관함
let library: lib.Library = { version: 1, entries: [] };
let openId: string | null = null;       // 지금 보고 있는 세션
let query = "";
/** 왼쪽에 무엇을 보여줄지. 보관함이 기본이다 — 코치는 대개 이미 받은 걸 본다 */
let pane: "library" | "board" = "library";

// 영상
let sync: vid.Sync = { offsetMs: 0, guessed: false, fileTime: null };
let videoOn = false;
/** 타임라인이 영상을 움직이는 중인가. 서로 밀지 않게 한 쪽만 몰게 한다 */
let seeking = false;
export const _seeking = () => seeking;

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
  const c = canvas();
  // 꺼진 칸은 크기가 0 이라 그릴 것도 없다
  if (c.clientWidth < 2 || c.clientHeight < 2) return;
  tl.draw({ canvas: c, series, view, marks, cursorMs, full: fullSpan });
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
  try {
    const r = await netFetch(`${boardUrl()}/api/files`, { method: "GET" });
    const j = (await r.json()) as { ok: boolean; files: FileInfo[] };
    boardFiles = j.files ?? [];
    renderSide();
    setStatus(`파일 ${j.files?.length ?? 0}개`, "good");
  } catch (e) {
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
      return `<div class="file ${mine ? "got" : ""}" data-name="${f.name}">
        <div><b>${f.name}</b> <span class="dim">${(f.size / 1048576).toFixed(2)} MB</span>
          ${mine ? "<span class='good'>받음</span>" : ""}</div>
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
    const r = await netFetch(`${boardUrl()}/file/${name}`, { method: "GET" });
    if (!r.ok) { setStatus(`받기 실패 — HTTP ${r.status}`, "bad"); return; }
    const buf = new Uint8Array(await r.arrayBuffer());
    await intake(buf, name);
  } catch (e) {
    setStatus(`받기 실패 — ${e}`, "bad");
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
    big.textContent = "⤢";
    big.title = `${PANE_NAME[k]}만 크게 보기`;
    big.onclick = (e) => { e.stopPropagation(); only(k); };

    const shut = document.createElement("button");
    shut.textContent = "✕";
    shut.title = `${PANE_NAME[k]} 닫기`;
    shut.onclick = (e) => { e.stopPropagation(); toggle(k); };

    box.append(big, shut);
    el.append(box);
  });
}

function applyLayout() {
  (Object.keys(PANE_EL) as PaneKey[]).forEach((k) => {
    $(PANE_EL[k]).classList.toggle("off", !layout[k]);
    const btn = $("tgl" + k[0].toUpperCase() + k.slice(1));
    btn.className = layout[k] ? "pane on" : "pane";
    // 켜짐·꺼짐이 눈에 안 띄면 단추가 무슨 상태인지 알 수가 없다.
    btn.textContent = (layout[k] ? "◉ " : "○ ") + PANE_NAME[k];
    btn.title = layout[k]
      ? `${PANE_NAME[k]} 끄기 — 두 번 누르면 이 칸만 크게`
      : `${PANE_NAME[k]} 켜기 — 두 번 누르면 이 칸만 크게`;

    // 혼자 남았으면 ⤢ 가 "되돌리기" 다
    const big = $(PANE_EL[k]).querySelector<HTMLButtonElement>(".handles button");
    if (big) {
      const alone = (Object.keys(layout) as PaneKey[]).filter((x) => layout[x]).length === 1;
      big.textContent = alone ? "⤡" : "⤢";
      big.title = alone ? "넷 다 켜기" : `${PANE_NAME[k]}만 크게 보기`;
    }
  });

  // 가운데가 하나만 켜져 있으면 나누개를 숨기고 그 칸이 다 쓴다
  const solo = !(layout.video && layout.data);
  $("center").classList.toggle("solo", solo);
  if (solo) {
    $("videoPane").style.flex = "1 1 100%";
    $("dataPane").style.flex = "1 1 100%";
  }

  localStorage.setItem(LAYOUT_KEY, JSON.stringify(layout));
  redraw();
}

function toggle(k: PaneKey) {
  const on = (Object.keys(layout) as PaneKey[]).filter((x) => layout[x]);
  if (layout[k] && on.length === 1) {
    // 마지막 하나까지 끄면 볼 게 없어진다. 대신 넷 다 켜 준다 —
    // 거절만 하면 사람이 어떻게 되돌리는지 모른다.
    layout = { lib: true, video: true, data: true, info: true };
    setStatus("마지막 칸이라 넷 다 켰습니다.");
    applyLayout();
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
    setStatus(`${PANE_NAME[k]}만 크게 봅니다. ⤡ 를 누르면 되돌아옵니다.`);
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
  $("vtime").textContent = tl.formatDuration(v.currentTime * 1000);
  $("voff").textContent = videoOn
    ? `영상 0초 = 세션 ${vid.formatOffset(sync.offsetMs)}` +
      (sync.guessed ? "  (파일 시각으로 짐작)" : "")
    : "";
  ($("play") as HTMLButtonElement).textContent = v.paused ? "▶" : "⏸";
}

/** 타임라인 커서 → 영상 위치 */
function seekVideoTo(sessionMs: number) {
  if (!videoOn) return;
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
function syncHere() {
  if (!videoOn || cursorMs === null) {
    setStatus("타임라인에서 맞출 자리에 커서를 두고 누르세요.", "bad");
    return;
  }
  sync = { ...sync, offsetMs: cursorMs - video().currentTime * 1000, guessed: false };
  setStatus(`맞췄습니다 — ${vid.formatOffset(sync.offsetMs)}`, "good");
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
  // 한 번 누르면 켜고 끄기, 두 번 누르면 그 칸만 크게.
  ([["Lib", "lib"], ["Video", "video"], ["Data", "data"], ["Info", "info"]] as
    [string, PaneKey][]).forEach(([id, k]) => {
    const b = $("tgl" + id);
    b.onclick = () => toggle(k);
    b.ondblclick = () => only(k);
  });

  // ── 영상 조작 ──────────────────────────────────────────────────────
  const v = video();
  $("play").onclick = () => { v.paused ? v.play() : v.pause(); renderVbar(); };
  $("syncHere").onclick = syncHere;
  // 누른 부호대로 아래 숫자가 움직인다. 반대로 두면 사람이 헷갈린다.
  $("n10").onclick = () => nudge(-10000);
  $("n1").onclick = () => nudge(-1000);
  $("p1").onclick = () => nudge(1000);
  $("p10").onclick = () => nudge(10000);

  // 영상이 돌면 타임라인 커서가 따라간다.
  // requestVideoFrameCallback 은 프레임이 바뀔 때마다 불러 줘서 timeupdate
  // (초당 4번쯤)보다 훨씬 매끄럽다. Safari 15.4 부터 있다.
  const follow = () => {
    if (!v.paused) {
      cursorMs = vid.videoToSession(sync, v.currentTime);
      // 커서가 화면 밖으로 나가면 따라 밀어 준다
      if (cursorMs < view.from || cursorMs > view.to) {
        const span = view.to - view.from;
        view.from = cursorMs - span * 0.3;
        view.to = view.from + span;
        clampView();
      }
      renderVbar();
      redraw();
    }
    if ("requestVideoFrameCallback" in v) {
      (v as any).requestVideoFrameCallback(follow);
    }
  };
  if ("requestVideoFrameCallback" in v) (v as any).requestVideoFrameCallback(follow);
  else (v as HTMLVideoElement).addEventListener("timeupdate", follow);

  v.addEventListener("seeked", () => { seeking = false; renderVbar(); });
  v.addEventListener("play", renderVbar);
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
    // 커서를 끌면 영상이 따라온다. 태킹 순간을 짚으면 그때 화면이 뜬다.
    if (cursorMs !== null && video().paused) seekVideoTo(cursorMs);
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
      // 숫자만 누르면 켜고 끄기, 시프트를 같이 누르면 그 칸만 크게
      case "1": case "!": (e.shiftKey ? only : toggle)("lib"); break;
      case "2": case "@": (e.shiftKey ? only : toggle)("video"); break;
      case "3": case "#": (e.shiftKey ? only : toggle)("data"); break;
      case "4": case "$": (e.shiftKey ? only : toggle)("info"); break;
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
