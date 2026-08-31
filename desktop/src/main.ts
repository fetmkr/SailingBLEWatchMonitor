// Sail Analyzer — 기록을 받아 타임라인으로 본다.
//
// 지금 되는 것
//   - 파일 열기 (.HLG) → 검사 → 타임라인
//   - 보드에 붙어 목록 보기 / 받기 (Range 이어받기)
//
// 설계는 ../../TRANSFER.md 에 있다.

import { fetch as tfetch } from "@tauri-apps/plugin-http";
import * as hlog from "./hlog";
import * as lib from "./library";
import * as vid from "./video";
import * as ble from "./ble";
import * as usb from "./usb";
import * as plat from "./platform";
import { convertFileSrc, invoke, addPluginListener } from "@tauri-apps/api/core";
import * as panes from "./panes";
import * as tl from "./timeline";
import { TrackMap, type TrackPoint } from "./map";
import "./styles.css";

// ── 상태 ────────────────────────────────────────────────────────────────

let session: hlog.Session | null = null;
let series: tl.Series[] = [];
/** 화면에 보이는 마킹. 파일에서 온 것 + 코치가 더한 것 */
let marks: tl.Mark[] = [];
/** 파일에서 읽은 마킹의 시각. 감추기·메모의 열쇠로 쓴다 */
let fileMarks: number[] = [];
/** 지도에 그릴 항적. 위성을 잡은 줄만 들어 있다 */
let track: TrackPoint[] = [];
/** 파일 요약(줄 수, Hz, IMU 종류…). 정보를 그릴 때마다 다시 넣는다 */
let lastMagFix: MagFix | null = null;
let metaHtml = "";
/**
 * 디버그 값을 보여줄지.
 *
 * 본 화면은 배가 어떻게 갔는지를 본다 — SOG · HDG · COG · Heel · Trim 과
 * 가속·자이로 세 축. 자력계 원본, 위성 수, 위치 정확도, 배터리는 그것과
 * 상관없는 값이라 무언가 이상할 때만 켠다.
 *
 * ★ 스위치는 **설정 서랍**에 둔다. 위 띠는 매번 하는 일(파일 열기, 마킹)
 *   자리고, 이건 어쩌다 한 번 켜는 것이다. 설정에는 이미 화면 색·지도
 *   색·배치 되돌리기 같은 가끔 만지는 스위치가 모여 있다.
 */
const DEBUG_KEY = "debugRows.v1";
let debugOn = localStorage.getItem(DEBUG_KEY) === "1";
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

/** 화면이 직접 파일을 고르게 한다. 고르면 File 이 그대로 온다.
 *
 *  Tauri 고르기 창은 경로만 주고 바이트는 IPC 를 거쳐야 하는데 그게 아주
 *  느리다. 여기서 받는 File 은 디스크에 그대로 있고, 필요한 조각만 읽힌다.
 *  (index.html 의 주석에 근거를 적어 뒀다) */
const waiting = new Map<string, (f: File | null) => void>();

function pickFile(inputId: string): Promise<File | null> {
  // ★ 앞서 기다리던 것이 있으면 먼저 닫는다.
  //
  //   고르기 창을 취소하면 change 가 안 온다. 그대로 두면 기다리던 것이
  //   안 끝나고 쌓인다. 다음에 또 누르면 하나 더 쌓이고, 마침내 파일을
  //   고르는 순간 쌓인 것들이 한꺼번에 터진다. 아이패드에서 고르기 창이
  //   계속 다시 뜨는 것처럼 보였다.
  waiting.get(inputId)?.(null);
  return new Promise((resolve) => {
    waiting.set(inputId, resolve);
    ($(inputId) as HTMLInputElement).click();
  });
}

/** 고르기 칸이 끝났을 때(골랐든 취소했든) 기다리던 것을 닫는다.
 *  듣는 자리는 앱이 뜰 때 한 번만 단다. 부를 때마다 달면 그것도 쌓인다. */
function wirePickers() {
  for (const id of ["pickVideo", "pickData"]) {
    const el = $(id) as HTMLInputElement;
    const settle = () => {
      const done = waiting.get(id);
      waiting.delete(id);
      const f = el.files && el.files[0] ? el.files[0] : null;
      el.value = "";              // 같은 파일을 다시 골라도 알아채게
      done?.(f);
    };
    el.addEventListener("change", settle);
    el.addEventListener("cancel", settle);   // 취소해도 끝은 끝이다
  }
}

async function openFile() {
  const f = await pickFile("pickData");
  if (!f) return;
  setStatus("읽는 중…");
  await intake(new Uint8Array(await f.arrayBuffer()), f.name);
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
  // 못 닫힌 파일이면 세션 길이·줄 수·첫 fix 시각이 전부 0 이다. 줄에서 되찾는다.
  // 안 하면 목록에 "NAV 0줄 · 시각 없음" 으로 떠서 빈 세션처럼 보인다.
  hlog.recoverHeader(s);
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

  refreshMap();
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

/**
 * 선 색을 CSS 에서 읽어 온다.
 *
 * 캔버스라 CSS 가 안 먹으니 손으로 가져와야 한다. 색을 styles.css 한 군데에
 * 모아 두면 판을 바꿀 때 선 색까지 같이 따라온다 — "계기판" 판은 선마다
 * 쨍한 색을 쓴다.
 */
function sc(name: string, fallback: string): string {
  const v = getComputedStyle(document.documentElement)
    .getPropertyValue(`--s-${name}`).trim();
  return v || fallback;
}

/** 자력계 치우침 재기 결과. 화면에 그대로 보여준다. */
interface MagFix {
  off: [number, number, number];
  before: number;   // 빼기 전 세기 흔들림 (µT)
  after: number;    // 빼고 나서
  field: number;    // 빼고 나서 세기 평균 (µT). 한국은 약 50
  use: boolean;     // 좋아졌을 때만 쓴다
}

/**
 * 자력계 값들에 **구를 맞춰** 치우침을 구한다.
 *
 * 자세도 COG 도 안 쓴다. 자력계 값만 쓴다. 배가 이리저리 흔들릴수록 잘
 * 구해진다 — 한 자세로만 있으면 못 구한다.
 */
function fitHardIron(nav: hlog.NavRecord[]): MagFix {
  // 자력계 축을 가속도 축에 맞춘 값으로 본다 (MPU-9250 은 둘이 다르다)
  const m: [number, number, number][] = [];
  for (const r of nav) {
    if (r.mag[0] === 0 && r.mag[1] === 0 && r.mag[2] === 0) continue;
    m.push([r.mag[1], r.mag[0], -r.mag[2]]);
  }
  const none: MagFix = { off: [0, 0, 0], before: 0, after: 0, field: 0, use: false };
  if (m.length < 200) return none;

  const spread = (c: [number, number, number]) => {
    let s1 = 0, s2 = 0;
    for (const v of m) {
      const d = Math.hypot(v[0] - c[0], v[1] - c[1], v[2] - c[2]);
      s1 += d; s2 += d * d;
    }
    const mean = s1 / m.length;
    return { mean, sd: Math.sqrt(Math.max(0, s2 / m.length - mean * mean)) };
  };

  // 가운데를 옮겨 가며 |m - c| 가 제일 고르게 되는 자리를 찾는다 (최소제곱 구)
  const n = m.length;
  const mx = m.reduce((a, v) => a + v[0], 0) / n;
  const my = m.reduce((a, v) => a + v[1], 0) / n;
  const mz = m.reduce((a, v) => a + v[2], 0) / n;
  const A = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  const b = [0, 0, 0];
  for (const v of m) {
    const x = v[0] - mx, y = v[1] - my, z = v[2] - mz;
    const q = x * x + y * y + z * z;
    A[0][0] += x * x; A[0][1] += x * y; A[0][2] += x * z;
    A[1][1] += y * y; A[1][2] += y * z; A[2][2] += z * z;
    b[0] += q * x; b[1] += q * y; b[2] += q * z;
  }
  A[1][0] = A[0][1]; A[2][0] = A[0][2]; A[2][1] = A[1][2];
  const M = A.map((row, k) => [...row, b[k]]);
  for (let col = 0; col < 3; col++) {
    let piv = col;
    for (let r = col; r < 3; r++) if (Math.abs(M[r][col]) > Math.abs(M[piv][col])) piv = r;
    [M[col], M[piv]] = [M[piv], M[col]];
    if (Math.abs(M[col][col]) < 1e-9) return none;   // 못 푼다 (한 자세로만 있었다)
    for (let r = 0; r < 3; r++) {
      if (r === col) continue;
      const f = M[r][col] / M[col][col];
      for (let c2 = col; c2 < 4; c2++) M[r][c2] -= f * M[col][c2];
    }
  }
  const off: [number, number, number] = [
    M[0][3] / M[0][0] / 2 + mx, M[1][3] / M[1][1] / 2 + my, M[2][3] / M[2][2] / 2 + mz,
  ];
  const b0 = spread([0, 0, 0]), b1 = spread(off);
  // 좋아졌고, 세기가 지구 자기장 근처(25~75 µT)일 때만 쓴다
  const use = b1.sd < b0.sd * 0.7 && b1.mean > 25 && b1.mean < 75;
  return { off, before: b0.sd, after: b1.sd, field: b1.mean, use };
}

function buildSeries(s: hlog.Session) {
  const t0 = s.imu.length ? s.imu[0].ms : s.nav.length ? s.nav[0].ms : 0;

  const navX = new Float64Array(s.nav.length);
  const sog = new Float32Array(s.nav.length);
  const cog = new Float32Array(s.nav.length);
  const sv = new Float32Array(s.nav.length);
  const hdg = new Float32Array(s.nav.length);
  const hdgCal = new Float32Array(s.nav.length);   // 축·기울기를 보정한 방위
  const hacc = new Float32Array(s.nav.length);
  const magX = new Float32Array(s.nav.length);
  const magY = new Float32Array(s.nav.length);
  const magZ = new Float32Array(s.nav.length);
  const batt = new Float32Array(s.nav.length);
  fileMarks = [];
  track = [];
  let imuAt = 0;   // 방위 계산이 쓰는 가속도 줄 짚개

  // ── 자력계 치우침(하드아이언)을 구한다 ─────────────────────────────
  //
  // 자력계 옆에 쇠붙이나 전류가 있으면 **늘 같은 크기의 자기장이 얹힌다.**
  // 지구 자기장 위에 상수가 더해진 것이다. 자세와는 아무 상관 없다.
  //
  // 짐작으로 빼면 안 되지만, **검사할 수 있다.** 지구 자기장 세기는 자세와
  // 무관하게 일정하다 (한국 약 50 µT). 그러니 어떻게 돌리든 자력계가 재는
  // 크기가 일정해야 한다. 안 일정하면 상수가 얹혀 있다는 뜻이고, 어떤 상수를
  // 빼서 일정해지면 그게 그 상수다.
  //
  // 세션 27 실측:
  //   빼기 전   세기 평균 58.2 µT · 흔들림 5.5 µT   ← 너무 크고 들쭉날쭉
  //   빼고 나서 세기 평균 46.0 µT · 흔들림 1.0 µT   ← 50 에 가깝고 일정해졌다
  //
  // ★ 좋아졌을 때만 쓴다. 나빠지면 안 뺀다. 그리고 그 숫자를 화면에 남겨서
  //   사람이 믿을지 말지 볼 수 있게 한다.
  const magFix = fitHardIron(s.nav);
  lastMagFix = magFix;

  
  for (let i = 0; i < s.nav.length; i++) {
    const r = s.nav[i];
    navX[i] = r.ms - t0;
    // 값이 없으면 NaN 으로 둔다. 0 을 넣으면 정박과 구별이 안 된다.
    sog[i] = r.sogKn ?? NaN;
    cog[i] = r.cogDeg ?? NaN;
    sv[i] = r.numSv;
    hacc[i] = r.hAccM ?? NaN;
    // ── 자력계를 가속도·자이로와 같은 축으로 옮긴다 ──
    //
    // MPU-9250 은 한 칩인데 자력계만 따로 든 칩(AK8963)이고, 라이브러리는
    // 그 값을 축 정렬 없이 그대로 준다 (`MPU9250_WE.cpp:106`). 그래서 전에는
    // **MAGX 와 ACCX 가 서로 다른 방향**인 채 같은 이름을 달고 있었다.
    //
    // 실측으로 확인한 짝 (2026-08-30 세션 27, 박스가 모로 누워 있었다):
    //   가속도계 Y 에 중력이 걸림  ↔  자력계 X 가 안 돎  → 자력 X = 가속 Y
    //
    // 세 센서를 한 축으로 맞춰 둔다. 그래야 "X 축" 이 어디서나 같은 방향이다.
    magX[i] = r.mag[1]; magY[i] = r.mag[0]; magZ[i] = -r.mag[2];
    batt[i] = r.battMv ? r.battMv / 1000 : NaN;

    // ── 방위(HDG) ─────────────────────────────────────────────────────
    //
    // 전에는 보드와 같이 `atan2(자력Y, 자력X)` 를 썼다. **그게 틀렸다.**
    //
    // MPU-9250 안의 자력계는 따로 든 칩(AK8963)이고, 라이브러리는 그 값을
    // 축 정렬 없이 그대로 준다 (`MPU9250_WE.cpp:106`). 그래서 자력계 축이
    // 가속도계 축과 다르다. 2026-08-30 세션 27 로 실측한 것:
    //
    //   가속도계   Y 에 중력이 걸려 있었다 (박스가 모로 누움)
    //   자력계     X 가 안 돌았다 (폭 ±11)  ← 이게 세로축이라는 뜻
    //              Y·Z 가 제대로 돌았다 (±34, ±31)
    //
    // 즉 자력계 X 가 가속도계 Y 자리다. 옛 식은 **세로축을 수평인 양** 썼다.
    // 보드를 평평히 놓으면 우연히 맞는 짝이 되는데, 박스가 누우면 깨진다.
    //
    // 고침은 두 가지다. **둘 다 기하학이라 값을 더하거나 빼지 않는다.**
    //   1) 축 맞추기      (자력Y, 자력X, -자력Z) 를 가속도 축에 대응
    //   2) 기울기 보정    가속도로 그때그때 수평면을 구한다.
    //                    박스가 어느 쪽으로 누워 있든 상관없어진다
    //
    // ★ COG 에 맞춰 보정하지 않는다. 요트는 leeway 때문에 뱃머리와 실제
    //   가는 방향이 **원래 다르다.** COG 로 맞추면 그 차이를 지워버린다.
    //   우리가 보려는 게 바로 그 차이다.
    //
    // 아직 안 넣은 것 둘. 둘 다 재야 나오는 값이라 COG 로 짐작하지 않는다.
    //   - 치우침(하드아이언). 세션 27 에서 수평 자기장(32 µT)만 한 크기였다.
    //     박스를 손에 들고 돌려서 따로 재야 한다
    //   - 보드가 뱃머리에서 몇 도 돌아 앉았나, 그리고 자기 편각(한국 약 8도 서편)
    //   그래서 **지금 값은 「어느 쪽을 보는가」가 아니라 「얼마나 돌았는가」다.**
    //   돌아가는 모양은 맞고, 0 이 어디인지는 아직 모른다.
    if (r.mag[0] === 0 && r.mag[1] === 0 && r.mag[2] === 0) {
      hdg[i] = NaN;
      hdgCal[i] = NaN;
    } else {
      // ── 옛 값 (HDG). 보드가 지금 쓰는 식 그대로 둔다 ──
      //   틀린 값이지만 **지운 게 아니라 나란히 둔다.** 보드가 화면과 BLE 로
      //   내보내는 것이 이 값이라, 앱에서만 고치면 둘이 달라진다.
      //   두 줄을 겹쳐 보면 얼마나 달라졌는지가 한눈에 보인다.
      let h0 = Math.atan2(r.mag[1], r.mag[0]) * 180 / Math.PI;
      if (h0 < 0) h0 += 360;
      hdg[i] = h0;

      // 이 줄의 시각에 제일 가까운 가속도 값을 찾는다 (IMU 는 100 Hz)
      while (imuAt + 1 < s.imu.length && s.imu[imuAt + 1].ms <= r.ms) imuAt++;
      const a = s.imu[imuAt];
      const g = a ? Math.hypot(a.acc[0], a.acc[1], a.acc[2]) : 0;
      if (!a || g < 0.5) {
        hdgCal[i] = NaN;                    // 자세를 모르면 보정도 못 한다
      } else {
        const gx = a.acc[0] / g, gy = a.acc[1] / g, gz = a.acc[2] / g;
        // 자력계 축을 가속도계 축에 맞춘다
        const mx = r.mag[1] - (magFix.use ? magFix.off[0] : 0);
        const my = r.mag[0] - (magFix.use ? magFix.off[1] : 0);
        const mz = -r.mag[2] - (magFix.use ? magFix.off[2] : 0);
        // 중력 방향 성분을 빼서 수평면에 눕힌다
        const dot = mx * gx + my * gy + mz * gz;
        const hx = mx - dot * gx, hy = my - dot * gy, hz = mz - dot * gz;
        // 보드 X 축도 같은 평면에 눕혀 기준으로 삼는다
        let fx = 1 - gx * gx, fy = -gx * gy, fz = -gx * gz;
        const fn = Math.hypot(fx, fy, fz);
        if (fn < 1e-3) {
          hdgCal[i] = NaN;                  // 보드 X 가 똑바로 서 있으면 기준이 없다
        } else {
          fx /= fn; fy /= fn; fz /= fn;
          const rx = gy * fz - gz * fy, ry = gz * fx - gx * fz, rz = gx * fy - gy * fx;
          let h = Math.atan2(hx * rx + hy * ry + hz * rz,
                             hx * fx + hy * fy + hz * fz) * 180 / Math.PI;
          if (h < 0) h += 360;
          hdgCal[i] = h;
        }
      }
    }

    if (r.event & 0x01) fileMarks.push(r.ms - t0);
    // 지도에 그릴 항적. 위성을 못 잡은 줄은 건너뛴다 — 없는 자리를 0,0 으로
    // 채우면 배가 아프리카 앞바다(위도 0, 경도 0)를 지나간 것처럼 보인다.
    if (r.lat !== null && r.lon !== null) {
      track.push({ ms: r.ms - t0, lat: r.lat, lon: r.lon, sogKn: r.sogKn });
    }
  }

  // ── SOG Cal — 위치로 잰 속도 ────────────────────────────────────────
  //
  // 왜 필요한가. 수신기가 **저속에서 속도를 0 으로 뭉갠다.** 2026-08-30
  // 세션 27 에서 1노트 아래가 통째로 0 이었다 (1노트 위는 하나도 안 틀렸다).
  // 그때도 배는 0.24~0.62 kts 로 움직이고 있었다. 위치는 멀쩡히 들어온다.
  //
  // 창을 5초로 잡은 근거 — 세션 27 로 재봤다.
  //
  //   창     정박 때 흔들림   도플러와 차이 (1.5kn 위, 중앙값)
  //    2초      0.15 kn            0.19 kn
  //    5초      0.16 kn            0.13 kn     ← 제일 잘 맞는다
  //   10초      0.17 kn            0.15 kn
  //   30초      0.22 kn            0.27 kn
  //   60초      0.29 kn            0.51 kn
  //
  // 길게 잡을수록 나빠진다. 배가 돌면 두 점을 잇는 직선이 실제로 간 거리보다
  // 짧아진다. 흔들림이 0.16 kn 이라 0.2 kn 대는 겨우 가른다 — **이 값은
  // 도플러를 대신하는 값이 아니라 도플러가 0 일 때 견주는 값이다.**
  const sogCal = new Float32Array(s.nav.length).fill(NaN);
  // ── COG Cal — 위치로 잰 침로 ──────────────────────────────────────────
  //
  // 수신기는 저속에서 침로를 **얼린다.** 못 구하면 마지막 값을 그대로 계속
  // 내보낸다 (u-blox 통합 매뉴얼 2.2.6 "Freezing the course over ground",
  // CASIC 은 속도 표식 3 으로 알려 준다). 세션 27 에서 이렇게 갈렸다.
  //
  //   그때 속도            COG 가 직전 줄과 달라진 비율
  //   1.0 ~ 1.5 kts              0%    ← 여기까지 아예 안 움직인다
  //   2.0 ~ 2.5 kts             25%
  //   전체 시간의 65% 가 얼어 있었다
  //
  // 위치로 구한 방향은 믿을 만하다 — 세션 27 에서 COG 와 치우침 0.7도,
  // 흩어짐 10.0도였다 (2노트 위, 5,739줄).
  //
  // ★ 창 안에서 움직인 거리가 3 m 를 넘을 때만 값을 만든다. 그 아래는
  //   방향이 잡음이라 지어낸 값이 된다.
  const cogCal = new Float32Array(s.nav.length).fill(NaN);
  {
    const WIN_MS = 5000;
    const M_LAT = 111320;            // 위도 1도의 미터
    let j = 0;
    for (let i = 0; i < s.nav.length; i++) {
      const b = s.nav[i];
      if (b.lat === null || b.lon === null) continue;
      while (j < i && b.ms - s.nav[j].ms > WIN_MS) j++;
      const a = s.nav[j];
      if (a.lat === null || a.lon === null) continue;
      const dt = (b.ms - a.ms) / 1000;
      if (dt < 2.5) continue;        // 창이 반도 안 찼으면 값을 안 만든다
      const mLon = M_LAT * Math.cos(a.lat * Math.PI / 180);
      const dx = (b.lon - a.lon) * mLon, dy = (b.lat - a.lat) * M_LAT;
      const dist = Math.hypot(dx, dy);
      sogCal[i] = dist / dt * 1.943844;
      if (dist > 3) {                       // 3 m 아래는 방향이 잡음이다
        let c = Math.atan2(dx, dy) * 180 / Math.PI;
        if (c < 0) c += 360;
        cogCal[i] = c;
      }
    }
  }

  // 힐·피치를 어느 축에서 봤나. 머리글에 적혀 있다 (hlog.h 의 표).
  const hAxis = s.header.heelAxis, pAxis = s.header.pitchAxis;
  const hSign = s.header.heelSign ? -1 : 1;
  const pSign = s.header.pitchSign ? -1 : 1;
  const hOff = s.header.heelOff, pOff = s.header.pitchOff;

  const imuX = new Float64Array(s.imu.length);
  const heel = new Float32Array(s.imu.length);
  const pitch = new Float32Array(s.imu.length);
  const heelComp = new Float32Array(s.imu.length);
  const pitchComp = new Float32Array(s.imu.length);

  // ── 힐·트림 comp — 세로축을 데이터가 고르게 한다 ──────────────────────
  //
  // 머리글은 힐·트림 축을 **박아 둔다.** 박스가 다르게 놓이면 그 축이 세로가
  // 되어 버리는데, 세로축으로는 기울기를 못 잰다 (늘 ±90도 근처다).
  //
  // 그래서 중력이 제일 많이 걸린 축을 세로로 보고, **나머지 두 축**으로 기울기를
  // 잰다. 방위에서 가속도로 수평면을 구한 것과 같은 생각이다.
  //
  // ★ "수평이 어디냐" 는 정하지 않는다. 요트는 한쪽으로 기울어 있는 때가
  //   많아서 평균을 0 으로 잡으면 기울어 있는 것을 평평하다고 말하게 된다.
  //   기준각은 사람이 잔잔할 때 "지금이 수평" 을 눌러 정해야 한다.
  //   지금은 박스가 비뚤게 놓인 만큼이 그대로 값에 남는다. 그건 사실이다.
  let upAxis = 2;
  {
    const sum = [0, 0, 0];
    for (const r of s.imu) { sum[0] += r.acc[0]; sum[1] += r.acc[1]; sum[2] += r.acc[2]; }
    upAxis = sum.map(Math.abs).indexOf(Math.max(...sum.map(Math.abs)));
  }
  // 세로축을 뺀 나머지 둘. 앞의 것을 힐, 뒤의 것을 트림으로 본다.
  const tiltAx = [0, 1, 2].filter((k) => k !== upAxis);
  const gx = new Float32Array(s.imu.length);
  const gy = new Float32Array(s.imu.length);
  const gz = new Float32Array(s.imu.length);
  const ax_ = new Float32Array(s.imu.length);
  const ay_ = new Float32Array(s.imu.length);
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
    heelComp[i] = (Math.asin(clamp(a[tiltAx[0]] / mag)) * 180) / Math.PI;
    pitchComp[i] = (Math.asin(clamp(a[tiltAx[1]] / mag)) * 180) / Math.PI;
    gx[i] = r.gyr[0]; gy[i] = r.gyr[1]; gz[i] = r.gyr[2];
    ax_[i] = ax; ay_[i] = ay; az[i] = az_;
  }

  // ※ 힐·트림의 "수평" 은 여기서 정하지 않는다.
  //
  //   한때 그 세션의 중력 평균을 수평으로 삼아 봤다. **틀렸다.** 배가 한쪽
  //   태킹으로 더 오래 갔으면 평균이 그쪽으로 치우치고, 그걸 0 으로 잡으면
  //   기울어 있는 것을 평평하다고 말하게 된다.
  //
  //   수평은 사람이 알려줘야 한다 — 잔잔할 때 "지금이 수평" 을 눌러 그 값을
  //   머리글의 기준각(heelOff/pitchOff)에 넣는 식이다. 지금은 둘 다 0 이라
  //   박스가 비뚤게 놓인 만큼이 그대로 값에 남는다 (세션 27 은 힐 -7.0도,
  //   트림 +2.1도). 그건 사실이므로 지어내지 않고 그대로 둔다.

  // 이름은 영어가 기본이다. 클래스도 대회도 영어로 돌아가고, 코치가 다른
  // 분석 도구와 나란히 볼 때 말이 맞아야 한다. 누르면 고칠 수 있다.
  //
  // ── 본 화면 다섯 줄 ──
  //
  // 훈련을 되돌아볼 때 실제로 보는 것은 이것뿐이다. 다섯이면 하나하나가
  // 크게 보인다. 나머지는 위 띠의 "센서 원본" 을 눌렀을 때만 나온다.
  //
  // HDG 와 COG 를 붙여 둔다 — 뱃머리가 향한 쪽과 배가 실제로 간 쪽이
  // 다르면 그 차이가 조류나 옆미끄러짐이다.
  // `limit` 은 그 값이 물리적으로 가질 수 있는 범위다. 없으면 축 여백이
  // 있을 수 없는 눈금을 만든다 — 속도 칸에 -0.3 kn, 방위 칸에 389 deg.
  // 속도는 아래만, 방위는 양쪽 다 막는다. 힐·자이로는 음수가 진짜라 안 막는다.
  const main: tl.Series[] = [
    // ── `alt` 는 같은 자리에서 갈아 끼울 수 있는 값이다 ──
    //
    //   cal   위치로 계산한 값. 수신기가 저속에서 속도를 0 으로, 침로를
    //         마지막 값으로 굳혀 버릴 때 이것만 살아 있다
    //   comp  축과 기울기를 보정한 값. 박스가 어떻게 놓였든 같은 기준이 된다
    //
    // 줄을 하나 더 만드는 대신 이름 칸의 작은 단추로 바꾼다. 줄이 반으로
    // 줄고, 두 줄을 눈으로 맞춰볼 필요가 없다.
    { code: "SOG",   name: n("SOG", "Speed Over Ground"), unit: "kn",
      color: sc("sog", "#4ea1ff"), xs: navX, ys: sog, limit: [0],
      alt: { ys: sogCal, name: n("SOG cal", "SOG from position (5s)"), tag: "cal" } },
    { code: "HDG",   name: n("HDG", "Heading"), unit: "deg",
      color: sc("hdg", "#ffd166"), xs: navX, ys: hdg, limit: [0, 360],
      alt: { ys: hdgCal, name: n("HDG comp", "Heading (axis + tilt)"), tag: "comp" } },
    { code: "COG",   name: n("COG", "Course Over Ground"), unit: "deg",
      color: sc("cog", "#77d4e8"), xs: navX, ys: cog, limit: [0, 360],
      alt: { ys: cogCal, name: n("COG cal", "COG from position (5s)"), tag: "cal" } },
    { code: "HEEL",  name: n("HEEL", "Heel"), unit: "deg",
      color: sc("heel", "#ff7a59"), xs: imuX, ys: heel, zeroCentered: true,
      alt: { ys: heelComp, name: n("HEEL comp", "Heel (axis picked)"), tag: "comp" } },
    { code: "TRIM",  name: n("TRIM", "Trim"), unit: "deg",
      color: sc("trim", "#ffc857"), xs: imuX, ys: pitch, zeroCentered: true,
      alt: { ys: pitchComp, name: n("TRIM comp", "Trim (axis picked)"), tag: "comp" } },

    // 가속·자이로 원본도 본 화면에 둔다. 힐과 트림이 여기서 나오고, 파도와
    // 태킹이 그대로 보인다. 100 Hz 로 기록하는 이유가 이 두 줄이다.
    { code: "ACCX",  name: n("ACCX", "Accel X"), unit: "g",
      color: sc("accx", "#ff9f7a"), xs: imuX, ys: ax_, zeroCentered: true },
    { code: "ACCY",  name: n("ACCY", "Accel Y"), unit: "g",
      color: sc("accy", "#ffb3a0"), xs: imuX, ys: ay_, zeroCentered: true },
    { code: "ACCZ",  name: n("ACCZ", "Accel Z"), unit: "g",
      color: sc("accz", "#5ad19a"), xs: imuX, ys: az },
    { code: "GYRX",  name: n("GYRX", "Gyro X"), unit: "deg/s",
      color: sc("gyrx", "#b39dff"), xs: imuX, ys: gx, zeroCentered: true },
    { code: "GYRY",  name: n("GYRY", "Gyro Y"), unit: "deg/s",
      color: sc("gyry", "#c9b8ff"), xs: imuX, ys: gy, zeroCentered: true },
    { code: "GYRZ",  name: n("GYRZ", "Gyro Z"), unit: "deg/s",
      color: sc("gyrz", "#9d7bff"), xs: imuX, ys: gz, zeroCentered: true },
  ];

  // ── 디버그 값 ──
  //
  // 배가 어떻게 갔는지와는 상관없는 것들이다. 자력계 원본은 방위가 이상할
  // 때, 위성 수와 위치 정확도는 그 구간 값을 믿어도 되나 볼 때, 배터리는
  // 훈련 중에 떨어졌나 볼 때 쓴다. 평소에는 자리만 차지한다.
  const debug: tl.Series[] = [
    { code: "MAGX",  name: n("MAGX", "Mag X"), unit: "uT",
      color: sc("magx", "#7ad4b0"), xs: navX, ys: magX, zeroCentered: true },
    { code: "MAGY",  name: n("MAGY", "Mag Y"), unit: "uT",
      color: sc("magy", "#8fdcc0"), xs: navX, ys: magY, zeroCentered: true },
    { code: "MAGZ",  name: n("MAGZ", "Mag Z"), unit: "uT",
      color: sc("magz", "#a4e4d0"), xs: navX, ys: magZ, zeroCentered: true },
    { code: "SAT",   name: n("SAT", "Satellites"), unit: "count",
      color: sc("sat", "#8a8a8a"), xs: navX, ys: sv },
    // ★ 파일에는 HDOP 이 아니라 **위치 정확도(hAcc)** 가 들어 있다.
    //   폰은 NMEA 의 HDOP 을 그대로 보여주는데, 우리 파일에는 그게 없다.
    //   뜻은 비슷하다 — 작을수록 믿을 만하다. 단위는 미터다.
    { code: "HACC",  name: n("HACC", "Position Accuracy"), unit: "m",
      color: sc("hacc", "#b0a0d0"), xs: navX, ys: hacc },
    { code: "BATT",  name: n("BATT", "Battery"), unit: "V",
      color: sc("batt", "#d0c060"), xs: navX, ys: batt },
  ];

  series = debugOn ? [...main, ...debug] : main;
  applyRowsShut();   // 접어 둔 줄을 새 파일에도 그대로 적용한다
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
  if (tab === "mark") renderMarkList();
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
// ★ v1 이 아니라 v2 다. 글자를 키우면서 이 값의 뜻이 바뀌었다.
//   148 과 62 는 글자가 12px 이던 시절에 고른 폭이다. 그대로 두면 이름이
//   더 잘린다. 이름을 올려서 옛 값을 안 읽게 하고, 새 글자 크기에 맞춰
//   다시 잡게 한다 (timeline.ts 의 tuneWidths).
const LABELW_KEY = "labelWidth.v2";
const NUMW_KEY = "numWidth.v2";
const SHUT_KEY = "rowsShut.v1";
{
  const w = Number(localStorage.getItem(LABELW_KEY));
  if (w > 0) tl.setLabelWidth(w);
  const nw = Number(localStorage.getItem(NUMW_KEY));
  if (nw > 0) tl.setNumWidth(nw);
}

const NAMES_KEY = "seriesNames.v1";
let names: Record<string, string> = {};
try { names = JSON.parse(localStorage.getItem(NAMES_KEY) ?? "{}"); } catch { /* 처음 */ }
const n = (code: string, def: string) => names[code] ?? def;

// ── 센서 줄 접기 ────────────────────────────────────────────────────────
//
// 일곱 줄을 다 펴 두면 하나하나가 납작하다. 지금 보려는 것만 펴 두면 그 줄이
// 화면을 다 쓴다. 접은 줄은 이름만 남는다 — 사라지면 되돌릴 길이 없다.
let rowsShut: Record<string, boolean> = {};
try { rowsShut = JSON.parse(localStorage.getItem(SHUT_KEY) ?? "{}"); } catch { /* 처음 */ }

function renderDbgBtn() {
  const c = document.getElementById("dbgRows") as HTMLInputElement | null;
  if (c) c.checked = debugOn;
}

/** 줄 수가 바뀌었으니 굴린 자리를 다시 잡는다. */
function fitRows() { tl.scrollTo(0); }

function applyRowsShut() {
  for (const s of series) s.collapsed = !!rowsShut[s.code];
}

function toggleRow(i: number) {
  const s = series[i];
  if (!s) return;
  const shut = !s.collapsed;
  // 마지막 하나까지 접으면 볼 게 없다. 그건 막는다.
  if (shut && series.every((x, k) => k === i || x.collapsed)) {
    setStatus("마지막 줄입니다. 다른 줄을 먼저 펴세요.", "bad");
    return;
  }
  s.collapsed = shut;
  if (shut) rowsShut[s.code] = true; else delete rowsShut[s.code];
  localStorage.setItem(SHUT_KEY, JSON.stringify(rowsShut));
  redraw();
}

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

  // 위 띠에는 이름만. 나머지는 오른쪽 서랍(세션)에 있다.
  $("fileTag").textContent = `${name} · 세션 ${h.session}`;

  // ★ 담아 두고 정보를 그릴 때마다 다시 넣는다.
  //   정보는 이제 목록 안에 있어서, 목록을 다시 그리면 이 자리도 새로
  //   만들어진다. 여기서 한 번만 넣으면 다음 번에 사라진다.
  metaHtml = `
    <div class="row"><b>${name}</b> <span class="dim">${(bytes / 1048576).toFixed(2)} MB · ${parseMs.toFixed(0)} ms 만에 읽음</span></div>
    <div class="row">세션 ${h.session} · 모듈 ${h.module} · ${when}</div>
    ${lastMagFix ? `<div class="row dim">
      자력계 치우침 ${lastMagFix.use ? "뺐음" : "안 뺌"} ·
      세기 흔들림 ${lastMagFix.before.toFixed(1)} → ${lastMagFix.after.toFixed(1)} µT ·
      세기 ${lastMagFix.field.toFixed(0)} µT (한국 약 50)
    </div>` : ""}
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

// ── 지도 ────────────────────────────────────────────────────────────────
//
// 처음 볼 때까지 안 띄운다. 남의 타일 서버를 쓰는 일이라 안 보는 사람 몫까지
// 받아 오면 안 된다.
let tmap: TrackMap | null = null;

function mapUp(): TrackMap | null {
  if (!layout.map) return tmap;              // 접혀 있으면 띄우지 않는다
  if (!tmap) {
    tmap = new TrackMap($("map"));
    tmap.onHover = (ms) => { cursorMs = ms; redraw(); };
    tmap.onPick = (ms) => {
      pinMs = clampPin(ms); cursorMs = pinMs; seekVideoTo(pinMs); redraw();
    };
    tmap.start();
    tmap.setSeamark(($("seamark") as HTMLInputElement).checked);
    tmap.setColorBySog(($("bySog") as HTMLInputElement).checked);
    tmap.setTrack(track);
    // 만드는 중에만 밖에서 만질 수 있게 내놓는다. 배포판에는 없다.
    if (import.meta.env.DEV) (window as any).__map = tmap;
  }
  return tmap;
}

/** 항적이 바뀌었을 때. 위치가 없으면 안내를 띄운다. */
function refreshMap() {
  const has = track.length >= 2;
  $("mapNone").style.display = has ? "none" : "flex";
  $("mapBar").style.display = has ? "flex" : "none";
  const m = mapUp();
  if (m) { m.setTrack(track); m.resize(); }
}

function redraw() {
  // 지도의 배 표시는 그래프 칸이 꺼져 있어도 따라와야 한다
  tmap?.setBoat(pinMs !== null ? pinMs : cursorMs);

  // 시간 막대도 커서를 따라온다. 그래프를 눌러 커서를 옮겨도 맞아야 한다.
  renderTransport();

  // 전체 보기 단추를 흐리게. 이미 전체를 보고 있으면 눌러도 바뀔 게 없는데,
  // 그걸 안 보여주면 "단추가 고장 났나" 싶다 (실제로 그렇게 보였다).
  const fitBtn = document.querySelector<HTMLElement>("#dataPane .handles .fit");
  if (fitBtn) {
    const all = Math.abs(view.from - fullSpan.from) < 1 &&
                Math.abs(view.to - fullSpan.to) < 1;
    fitBtn.classList.toggle("off", all);
  }

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

/**
 * 보드에 요청한다. **반드시 제한 시간을 건다.**
 *
 * 안 걸었더니 이런 일이 있었다. 맥이 보드 WiFi 에 안 붙은 채로 목록을 눌렀는데,
 * 192.168.4.1 로 간 요청이 집 공유기로 나갔다가 사라졌다. 앱은 "받는 중" 을
 * 띄운 채 영원히 기다렸다. 사용자는 앱이 멈춘 줄 알았다.
 *
 * 두 가지를 건다.
 *   붙는 데까지    connectTimeout — Tauri 의 http 가 rust 쪽에 넘긴다
 *   전체          AbortSignal    — 붙은 뒤 응답이 안 와도 끝낸다
 *
 * [확인: node_modules/@tauri-apps/plugin-http/dist-js/index.js:46,113]
 *   init.signal 을 보고 rust 쪽 요청까지 취소한다.
 */
const CONNECT_MS = 4000;

async function askBoard(
  path: string, wholeMs: number | null, ac = new AbortController(),
  connectMs = CONNECT_MS,
): Promise<Response> {
  const timer = wholeMs === null ? null
    : setTimeout(() => ac.abort(new Error("응답이 없습니다")), wholeMs);
  try {
    return await netFetch(`${boardUrl()}${path}`, {
      method: "GET",
      signal: ac.signal,
      ...(inApp ? { connectTimeout: connectMs } : {}),
    } as RequestInit);
  } finally {
    if (timer) clearTimeout(timer);
  }
}

/** 왜 못 붙었는지 사람 말로. 원문도 같이 남긴다 — 감추면 다음에 못 고친다. */
function boardWhy(e: unknown): string {
  const raw = e instanceof Error ? e.message : String(e);
  const s = raw.toLowerCase();
  // rust 쪽 http 가 "요청을 못 보냈다" 고 할 때 나오는 말이다. 붙기는
  // 붙었는데 답이 오다 끊긴 경우도 여기로 온다 — 보드가 다시 켜지면 그렇다.
  if (s.includes("error sending request") || s.includes("connection closed")) {
    return `보드가 대답을 멈췄습니다. 배 찾기로 다시 깨워 보세요 (${raw})`;
  }
  if (s.includes("abort") || s.includes("응답이 없습니다") || s.includes("timed out")
      || s.includes("timeout")) {
    return `보드가 대답이 없습니다. 맥이 보드 WiFi 에 붙어 있는지 보세요 (${raw})`;
  }
  if (s.includes("connect") || s.includes("unreachable") || s.includes("refused")
      || e instanceof TypeError) {
    return `보드에 닿지 못했습니다. 주소와 WiFi 를 보세요 (${raw})`;
  }
  return raw;
}

let listing = false;

async function listBoard() {
  // 깨우기 끝에서도, 이미 붙은 배의 "목록 새로" 에서도 이걸 부른다.
  if (listing) { setStatus("목록을 받는 중입니다.", "bad"); return; }
  if (fetching) { setStatus("파일을 받는 중입니다. 끝난 뒤에 하세요.", "bad"); return; }
  listing = true;
  setStatus("보드에 물어보는 중…");
  // 보드가 파일마다 머리글을 읽어야 해서 몇 초 걸린다. 그동안 표시를 둔다.
  setProgress("보드에서 파일 목록을 받는 중…");
  try {
    // 파일이 많으면 머리글 읽는 데 오래 걸린다. 그래도 25초면 충분하다.
    const r = await askBoard("/api/files", 25000);
    const j = (await r.json()) as { ok: boolean; files: FileInfo[] };
    boardFiles = j.files ?? [];
    // 목록이 왔으면 붙어 있는 것이다. 주소를 손으로 친 경우도 여기서부터
    // 연락을 보낸다 — 그래야 앱을 닫았을 때 보드가 알아서 꺼진다.
    keepAliveStart();
    renderSide();
    setProgress(null);
    setStatus(`파일 ${j.files?.length ?? 0}개 — 받을 것을 고르세요`, "good");
  } catch (e) {
    setProgress(null);
    const also = othersDoing();
    setStatus(`보드에 못 붙었습니다 — ${boardWhy(e)}` +
              (also ? ` · ${also}` : ""), "bad");
  } finally {
    listing = false;
  }
}

let boardFiles: FileInfo[] = [];

// 보드 서랍 안의 작은 것들. 서랍을 그릴 때마다 맞춘다.
/**
 * 단추를 연달아 눌러도 일은 한 번만 한다.
 *
 * 사람은 반응이 없으면 또 누른다. 그게 정상이다. 문제는 그때마다 같은
 * 요청이 하나씩 더 나가는 것이다. **보드는 한 번에 한 사람만 상대하므로**
 * 두 번째부터는 줄을 서고, 화면은 점점 더 느려진다. 찾기는 더 나쁘다 —
 * 두 번째 찾기가 첫 번째의 블루투스 스캔을 꺼 버려서 목록이 빈 채로 끝난다.
 *
 * 그래서 일이 끝날 때까지 그 단추를 눌리지 않게 한다. 눌러도 안 되는 게
 * 아니라 눌러지지 않는 것이라, 사람이 기다려야 한다는 걸 바로 안다.
 */
function job(id: string, fn: () => unknown | Promise<unknown>) {
  const btn = $(id) as HTMLButtonElement;
  let running = false;
  btn.onclick = async () => {
    if (running) return;
    running = true;
    btn.disabled = true;
    try { await fn(); }
    finally { running = false; btn.disabled = false; }
  };
}

function syncBoardBar() {
  // 주소 칸은 BLE 를 못 쓰거나 사람이 "주소로" 를 눌렀을 때만 보인다
  ($("hostBar") as HTMLElement).style.display =
    (byHand || !plat.caps().ble) ? "flex" : "none";
  // 연결 해제는 붙어 있을 때만 뜬다. 연락을 보내고 있으면 붙어 있는 것이다.
  ($("btDrop") as HTMLElement).style.display =
    pinger !== null ? "inline-block" : "none";
}

// ── 보드 찾기 ───────────────────────────────────────────────────────────
//
// 보드 WiFi 는 평소에 꺼져 있다. 그래서 IP 로는 못 찾는다. 찾는 길이 둘이다.
//
//   블루투스   광고는 늘 나가고 있다. 배가 물에 떠 있어도 잡힌다.
//   USB        블루투스가 없는 컴퓨터의 길. 케이블을 꽂아야 한다.
//
// **한 단추로 둘 다 찾는다.** 사람이 "지금은 어느 쪽이지" 를 고를 일이
// 아니다. 찾은 뒤에 어느 길로 찾았는지만 보여주면 된다.
type Via = "ble" | "usb";

interface Found {
  via: Via;
  key: string;        // BLE 는 주소, USB 는 포트 경로
  name: string;       // 보여줄 이름
  sub: string;        // 밑에 작게
  board?: ble.Board;  // BLE 일 때
}

let found: Found[] = [];
let scanning = false;
let byHand = false;          // 사람이 주소를 직접 치겠다고 했나
/** USB 목록에서 우리 것 같지 않은 포트까지 보여줄지 */
let usbAll = false;
let waking: string | null = null;   // 지금 깨우는 중인 것의 열쇠
/**
 * 지금 붙어 있는 배의 열쇠. 안 붙었으면 null.
 *
 * ★ 위쪽의 `linked` 와 다른 것이다. 그건 영상과 데이터를 맞췄나(싱크)이고
 *   이건 어느 배에 붙어 있나다. 이름이 겹쳐서 갈라 뒀다.
 *
 * 이게 없어서 꼬였다. **보드는 WiFi 를 켜는 동안 블루투스를 내린다**
 * (firmware-rak: "[BLE] 내렸습니다 (WiFi 쓰는 동안)"). 그런데 앱은 붙고
 * 나서도 찾기 목록을 그대로 두었다. 거기서 연결을 다시 누르면 이미 사라진
 * 블루투스에 붙으려 하니 반드시 시간 초과가 난다.
 */
let boardLinked: string | null = null;

function renderBoards() {
  const box = $("boards");
  if (!found.length) {
    box.innerHTML = scanning
      ? "<div class='dim pad'>찾는 중…</div>"
      : "<div class='dim pad'>배 찾기를 누르세요.<br>" +
        "블루투스와 USB 를 같이 봅니다.<br>" +
        "보드 WiFi 가 꺼져 있어도 찾습니다.</div>";
    return;
  }
  box.innerHTML = found.map((f) => {
    const busy = waking === f.key;
    const on = boardLinked === f.key;
    return `<div class="file board${on ? " linked" : ""}" data-key="${f.key}">
      <div>
        <b>${f.name}</b> <span class="via ${f.via}">${f.via === "ble" ? "블루투스" : "USB"}</span>
        ${on ? '<span class="via on">붙어 있음</span>' : ""}
        <div class="dim">${f.sub}</div>
      </div>
      <button class="wake" ${busy ? "disabled" : ""}>${
        busy ? "깨우는 중…" : on ? "목록 새로" : "연결"}</button>
    </div>`;
  }).join("");

  // USB 를 보고 있으면 "다른 포트도 보기" 를 붙인다. 맥에는 늘 여러 개가
  // 떠 있어서 기본은 우리 것 같은 것만 보여준다.
  if (found.some((f) => f.via === "usb") || usbAll) {
    box.insertAdjacentHTML("beforeend",
      `<div class="pad"><label class="chk"><input id="usbAll" type="checkbox"
        ${usbAll ? "checked" : ""}> 다른 USB 포트도 보기</label></div>`);
    const chk = document.getElementById("usbAll") as HTMLInputElement | null;
    if (chk) chk.onchange = () => { usbAll = chk.checked; void scanBoards(); };
  }

  box.querySelectorAll<HTMLElement>(".board .wake").forEach((btn) => {
    btn.onclick = (e) => {
      e.stopPropagation();
      const key = btn.closest<HTMLElement>(".board")?.dataset.key;
      const f = found.find((x) => x.key === key);
      if (!f) return;
      if (f.via === "ble" && f.board) void wake(f.board);
      else void wakeUsb(f.key);
    };
  });
}

/**
 * 세기를 눈으로. -50 이면 코앞, -85 면 겨우 잡힌다.
 *
 * ★ 0 이상이면 세기가 아니다. 블루투스는 못 잰 경우 127 을 준다
 *   (Bluetooth Core Spec 의 "RSSI is not available"). 아이패드에서 실제로
 *   "▮▮▮▮ 127 dBm" 이라고 떴다. 없는 값을 세기인 척 보여주면 안 된다.
 */
function rssiText(rssi: number): string {
  if (rssi >= 0) return "세기 모름";
  const n = rssi > -55 ? 4 : rssi > -68 ? 3 : rssi > -80 ? 2 : 1;
  return "▮".repeat(n) + "▯".repeat(4 - n) + ` ${rssi} dBm`;
}

/** BLE 로 찾은 것들을 목록에 반영한다. USB 쪽은 건드리지 않는다. */
function mergeBle(list: ble.Board[]) {
  const usbOnly = found.filter((f) => f.via === "usb");
  found = [
    ...list.map((b): Found => ({
      via: "ble", key: b.address, name: b.name,
      sub: rssiText(b.rssi), board: b,
    })),
    ...usbOnly,
  ];
  renderBoards();
}

/**
 * 배를 찾는다. **블루투스 먼저, 못 찾으면 USB.**
 *
 * 둘 다 보여주면 같은 보드가 두 줄로 나온다. 블루투스가 되면 그쪽이 낫다 —
 * 케이블이 필요 없고 배가 물에 떠 있어도 잡힌다. USB 는 블루투스가 없거나
 * 아무것도 못 찾았을 때의 길이다.
 */
async function scanBoards() {
  // 단추 말고 "다른 USB 포트도 보기" 체크박스도 이걸 부른다. 그래서
  // job() 만으로는 안 되고 여기서도 막는다.
  if (scanning) return;
  scanning = true;
  try {
    await scanBoardsRun();
  } finally {
    // ★ finally 여야 한다. 안 그러면 어딘가에서 한 번 터졌을 때 깃발이
    //   걸린 채 남아, 그 뒤로는 배 찾기를 눌러도 아무 일도 안 일어난다.
    scanning = false;
    renderBoards();
  }
}

async function scanBoardsRun() {
  found = [];
  renderBoards();

  // ── 블루투스 ──
  const st = await ble.ready();
  if (st.ok) {
    setStatus("블루투스로 주변 배를 찾는 중…");
    try {
      // 6초를 다 채우지 않는다. 부두에서는 배가 한 대뿐인 경우가 많은데
      // 그때마다 6초를 서 있을 이유가 없다. 하나라도 찾고 2초가 지나면
      // 거기서 끊는다. 아무것도 못 찾으면 6초를 다 쓴다.
      await ble.scan(6000, (list) => mergeBle(list));
      const t0 = performance.now();
      while (performance.now() - t0 < 6200) {
        await new Promise((r) => setTimeout(r, 200));
        const n = found.filter((f) => f.via === "ble").length;
        if (n && performance.now() - t0 > 2000) break;
      }
      await ble.scanStop();
    } catch (e) {
      setStatus(`블루투스 찾기 실패 — ${e}`, "bad");
    }
  }

  const nBle = found.filter((f) => f.via === "ble").length;
  if (nBle) {
    scanning = false;
    renderBoards();
    setStatus(`배 ${nBle}대 찾았습니다.`, "good");
    return;
  }

  // ── 블루투스로 못 찾았다 ──
  //
  // 여기가 갇히는 자리였다. 보드가 WiFi 를 켠 채면 블루투스 광고가 안 나가서
  // 아무리 찾아도 안 보인다. 그런데 그때는 **주소로는 열려 있다.**
  // 그러니 포기하기 전에 지난번 주소를 두드린다.
  const known = knownHosts();
  if (known.length) {
    setStatus("블루투스로는 안 보입니다. 지난번 주소를 두드려 봅니다…");
    const hit = await knock(known);
    if (hit) {
      setStatus(`${hit} 로 이미 열려 있습니다. 목록을 받습니다.`, "good");
      keepAliveStart();
      await listBoard();
      return;
    }
  }

  // ── 주소로도 없다. USB 를 본다 ──
  if (!plat.caps().usb) {
    scanning = false;
    renderBoards();
    setStatus(st.ok ? "못 찾았습니다. 보드가 켜져 있는지 보세요."
                    : `블루투스도 USB 도 못 씁니다 — ${st.why}`, "bad");
    byHand = true;
    renderSide();
    return;
  }

  setStatus(st.ok ? "블루투스로 못 찾았습니다. USB 를 봅니다…"
                  : "블루투스를 못 씁니다. USB 를 봅니다…");
  try {
    const ports = await usb.list(usbAll);
    found = ports.map((p): Found => ({
      via: "usb", key: p.path, name: p.path, sub: "듣는 중…",
    }));
    renderBoards();

    // 아무것도 안 보내고 1.8초 들어 본다. 보드는 1초에 한 번 제 이름을 뱉는다.
    for (const p of ports) {
      const boat = await usb.sniff(p.path);
      const row = found.find((f) => f.key === p.path);
      if (!row) continue;
      row.name = boat ?? p.path;
      row.sub = boat ? p.path : "우리 보드인지 모름 · 눌러 보면 압니다";
      renderBoards();
    }

    scanning = false;
    renderBoards();
    const named = found.filter((f) => f.name.startsWith("SAIL-")).length;
    setStatus(
      ports.length === 0
        ? (usbAll ? "USB 로 꽂힌 것이 없습니다." :
           "USB 로 꽂힌 보드가 없습니다. 다른 포트도 보려면 아래를 누르세요.")
        : named ? `USB 로 배 ${named}대 찾았습니다.`
                : `USB 포트 ${ports.length}개 보입니다. 눌러 보세요.`,
      ports.length ? "good" : "bad");
    if (!ports.length) { byHand = true; renderSide(); }
  } catch (e) {
    scanning = false; renderBoards();
    setStatus(`USB 를 못 봅니다 — ${e}`, "bad");
  }
}

/**
 * 그 주소를 블루투스로 다시 찾는다. 못 찾으면 null.
 *
 * 찾는 시간은 짧게 잡는다. 이건 다시 해 보는 길이지 처음 찾는 길이 아니다.
 */
async function findAgain(address: string): Promise<ble.Board | null> {
  let hit: ble.Board | null = null;
  try {
    await ble.scan(4000, (list) => {
      const m = list.find((x) => x.address === address);
      if (m) hit = m;
    });
    await new Promise((r) => setTimeout(r, 4200));
    await ble.scanStop();
  } catch { /* 못 찾으면 그대로 실패로 둔다 */ }
  return hit;
}

/**
 * 블루투스로 붙는다. **한 번 실패하면 다시 찾아서 한 번 더 해 본다.**
 *
 * 왜냐면 — 보드가 WiFi 를 껐다 켜면 블루투스가 통째로 새로 올라온다.
 * 그 전에 찾아 둔 것으로 붙으려 하면 시간만 흐르고 안 붙는다.
 *
 * 실제로 이랬다. 앞사람이 연결을 끊자 보드가 WiFi 를 끄면서 블루투스를
 * 새로 올렸다. 두 번째 앱은 그 전에 찾아 둔 목록을 들고 있었고, 연결을
 * 누르니 시간만 흘렀다. 배 찾기를 다시 누르면 됐다.
 * **사람이 그걸 알아야 할 이유가 없다.**
 */
async function openLink(b: ble.Board): Promise<ble.Link> {
  try {
    return await ble.Link.open(b);
  } catch (e) {
    setStatus(`${b.name} 을 다시 찾는 중… (목록이 오래됐을 수 있습니다)`);
    const fresh = await findAgain(b.address);
    if (!fresh) throw e;
    // 찾은 것으로 목록도 새로 고친다. 세기 표시가 옛 값으로 남으면 안 된다.
    const row = found.find((f) => f.key === b.address);
    if (row) { row.board = fresh; row.sub = rssiText(fresh.rssi); renderBoards(); }
    return await ble.Link.open(fresh);
  }
}

/**
 * 보드를 깨운다. BLE 로 붙어서 "WiFi 켜" 를 시키고, 답에 적힌 주소로 옮겨간다.
 *
 * ★ WiFi 가 켜지면 BLE 는 끊긴다. 그건 잘못된 게 아니다 (PROTOCOL.md §9).
 *   그래서 답을 받자마자 BLE 는 잊고 HTTP 로 넘어간다.
 */
async function wake(b: ble.Board) {
  if (waking) { setStatus("이미 한 대를 깨우는 중입니다.", "bad"); return; }
  if (fetching) { setStatus("파일을 받는 중입니다. 끝난 뒤에 하세요.", "bad"); return; }

  // 이미 이 배에 붙어 있다. 블루투스로 다시 붙는 길은 아예 없다 —
  // 보드가 WiFi 를 켜는 동안 블루투스를 내려 두기 때문이다. 사람이 여기서
  // 바라는 건 목록을 다시 보는 것이니 그걸 한다.
  if (boardLinked === b.address) {
    setStatus(`${b.name} 에는 이미 붙어 있습니다. 목록을 다시 받습니다.`);
    await listBoard();
    return;
  }
  // 다른 배에 붙어 있으면 먼저 놓아준다. 안 놓으면 그 배는 WiFi 를 켠 채로
  // 남아 전기를 먹고, 블루투스로도 다시 못 찾는다.
  //
  // boardLinked 가 아니라 pinger 를 본다. 주소를 손으로 쳐서 붙은 경우에는
  // 어느 배인지 이름을 모르지만, 연락을 보내고 있다는 건 붙어 있다는 뜻이다.
  if (pinger) {
    setStatus("앞의 배를 놓아주는 중…");
    await sleepBoard(true);
  }

  waking = b.address;
  renderBoards();
  await ble.scanStop();
  setStatus(`${b.name} 에 붙는 중…`);

  let link: ble.Link | null = null;
  try {
    link = await openLink(b);
    const st = await link.ask("wifi status");
    if (st?.startsWith("status") && / rec on/.test(st)) {
      setStatus("기록 중입니다. 기록을 멈춘 뒤에 받으세요.", "bad");
      return;
    }

    setProgress(`${b.name} 의 WiFi 를 켜는 중…`);
    // 번호를 같이 보낸다. 보드가 나를 뺀 나머지가 몇 대인지 세어 답한다.
    const reply = await link.ask(`wifi on ${appId()}`, 8000);
    if (!reply) { setStatus("보드가 대답이 없습니다.", "bad"); return; }

    if (reply.startsWith("err wifi no-ssid")) {
      setStatus("이 보드에 붙을 WiFi 가 정해져 있지 않습니다. WiFi 설정을 먼저 하세요.", "bad");
      return;
    }
    const up = ble.parseWifiUp(reply);
    if (!up) { setStatus(`알 수 없는 답 — ${reply}`, "bad"); return; }

    // ── 남이 쓰고 있으면 아예 안 붙는다 ────────────────────────────
    //
    // 보드는 한 번에 한 대만 상대한다. 반쯤 붙여 놓으면 목록은 보이는데
    // 받기만 안 되는 어정쩡한 상태가 된다. 사람은 그걸 고장으로 본다.
    // 그래서 여기서 끝낸다.
    //
    // users 는 **나를 뺀** 수다. 보드가 내 번호를 알고 빼 준다.
    // 그래서 앱은 0 인지만 보면 된다. 견주는 일을 앱이 안 한다.
    if (up.users >= 1) {
      setProgress(null);
      setStatus(
        `${b.name} 은 지금 다른 기기가 쓰고 있습니다` +
        (up.by ? ` (${up.by})` : "") + `. ` +
        `보드는 한 번에 한 대만 상대합니다. ` +
        `그쪽에서 연결을 끊거나 15초쯤 뒤에 다시 해 보세요.`, "bad");
      return;
    }

    ($("host") as HTMLInputElement).value = up.hosts[0];

    if (up.kind === "ap") {
      setProgress(null);
      setStatus(`${b.name} 이 자기 WiFi 를 열었습니다 — ` +
                `WiFi 를 "${up.ssid}" 로 바꾼 뒤 목록을 누르세요.`, "good");
      byHand = true; renderSide();
      return;
    }

    // 붙는 데 시간이 걸린다. 주소가 답할 때까지 두드려 본다.
    setProgress(`${b.name} 이 ${up.ssid} 에 붙는 중…`);
    const found = await waitForBoard(up.hosts, 25000);
    setProgress(null);
    if (!found) {
      setStatus(`${up.hosts.join(" 도 ")} 도 아직 답이 없습니다. ` +
                `잠시 뒤 목록을 눌러 보세요.`, "bad");
      byHand = true; renderSide();
      return;
    }
    setStatus(`${b.name} 준비됐습니다 — ${found}`, "good");
    // 다음에 블루투스로 못 찾을 때 여기로 들어온다. 답한 주소를 앞에 둔다.
    localStorage.setItem(HOSTS_KEY,
      JSON.stringify([found, ...up.hosts.filter((h) => h !== found)]));
    boardLinked = b.address;
    keepAliveStart();
    await listBoard();
  } catch (e) {
    setProgress(null);
    setStatus(`${b.name} — ${e}`, "bad");
  } finally {
    try { await link?.close(); } catch { /* 이미 끊겼으면 그만이다 */ }
    waking = null;
    renderBoards();
  }
}


let usbBusy: string | null = null;

/**
 * USB 로 깨운다.
 *
 * 보드가 AP 를 열면 사람이 맥 WiFi 를 그 이름으로 바꿔야 한다. 그건 앱이
 * 대신 못 한다. 대신 **바뀌었는지 스스로 지켜보다 이어서 진행한다** —
 * 사람은 WiFi 만 고르면 되고 주소를 칠 일이 없다.
 */
async function wakeUsb(path: string) {
  if (usbBusy) { setStatus("이미 하나 깨우는 중입니다.", "bad"); return; }
  usbBusy = path;
  waking = path;
  renderBoards();

  let link: usb.Link | null = null;
  try {
    setStatus(`${path} 를 여는 중…`);
    link = await usb.Link.open(path);

    const st = await link.ask("wifi status");
    if (!st) {
      // 왜 안 오는지 실마리를 같이 준다. 아무 줄도 안 왔으면 우리 보드가
      // 아니거나 포트가 다른 프로그램에 잡혀 있는 것이다.
      const heard = link.peek();
      setStatus(
        heard.length
          ? `보드가 대답이 없습니다. 들린 것: ${heard[heard.length - 1].slice(0, 60)}`
          : "보드가 대답이 없습니다. 우리 보드가 맞는지, " +
            "다른 프로그램(시리얼 모니터)이 잡고 있지 않은지 보세요.",
        "bad");
      return;
    }
    if (/ rec on/.test(st)) {
      setStatus("기록 중입니다. 기록을 멈춘 뒤에 받으세요.", "bad");
      return;
    }

    // 이미 AP 를 열어 둔 보드면 다시 열 이유가 없다. 다시 열면 붙어 있던
    // 노트북이 떨어지고 처음부터 다시 붙어야 한다.
    let up: ble.WifiUp | null = null;
    const already = /status name (\S+).* mode ap ip (\S+)/.exec(st);
    if (already) {
      up = { kind: "ap", hosts: [already[2]], ssid: already[1], pass: "",
             users: 0, by: "" };
      setStatus(`${already[1]} 이 이미 WiFi 를 열어 두었습니다.`);
    } else {
      setProgress("보드가 자기 WiFi 를 여는 중…");
      const reply = await link.ask(`wifi ap ${appId()}`, 10000);
      up = reply ? ble.parseWifiUp(reply) : null;
      if (!up) {
        setProgress(null);
        setStatus(`알 수 없는 답 — ${reply ?? "없음"}`, "bad");
        return;
      }
    }

    // 여기서부터는 WiFi 다. 시리얼은 더 쓸 일이 없다.
    ($("host") as HTMLInputElement).value = up.hosts[0];
    setProgress(up.pass
      ? `맥 WiFi 를 "${up.ssid}" 로 바꾸세요 · 비밀번호 ${up.pass}`
      : `맥 WiFi 를 "${up.ssid}" 로 바꾸세요`);
    setStatus(`${up.ssid} 가 열렸습니다. 맥 WiFi 를 그것으로 바꾸면 이어서 갑니다.`);
    byHand = true;
    renderSide();

    // 사람이 WiFi 를 바꿀 때까지 기다린다. 넉넉히 준다 — 비밀번호도 쳐야 한다.
    const found = await waitForBoard(up.hosts, 90000);
    setProgress(null);
    if (!found) {
      setStatus(`${up.hosts[0]} 이 아직 답이 없습니다. ` +
                `WiFi 를 "${up.ssid}" 로 바꾼 뒤 목록을 눌러 보세요.`, "bad");
      return;
    }
    setStatus(`준비됐습니다 — ${found}`, "good");
    keepAliveStart();
    await listBoard();
  } catch (e) {
    setProgress(null);
    setStatus(`USB — ${e}`, "bad");
  } finally {
    try { await link?.close(); } catch { /* 이미 닫혔으면 그만 */ }
    usbBusy = null;
    waking = null;
    renderBoards();
  }
}

/**
 * 보드가 답할 때까지 두드려 본다. 먼저 답하는 주소를 쓴다.
 *
 * 한 번에 6초를 준다. 이름(mDNS)은 실측 2.6초가 걸렸는데, 3초로 잡았더니
 * 아슬아슬하게 놓쳤다.
 */
/**
 * 지난번에 붙었던 주소. 보드가 WiFi 를 켠 채로 남았을 때 들어가는 문이다.
 *
 * 없으면 갇힌다. 보드는 WiFi 를 켜는 동안 블루투스를 내리므로, 앱이 주소를
 * 잊으면 블루투스로도 WiFi 로도 못 찾는다. 보드가 스스로 끌 때까지 (연락이
 * 없으면 5분) 아무것도 못 한다. 그 5분을 없애려고 적어 둔다.
 */
const HOSTS_KEY = "board.hosts.v1";

/**
 * 이 앱의 번호. 보드가 "나 말고 몇 대가 쓰나" 를 셀 때 나를 빼는 데 쓴다.
 *
 * 왜 주소로 안 하냐면 두 군데서 틀리기 때문이다.
 *   공유기가 주소를 바꾸면      같은 앱을 남으로 본다
 *   한 기기에서 앱 두 개를 띄우면 서로 다른 앱을 나로 본다
 *
 * **관리할 것은 없다.** 처음 켤 때 스스로 만들고, 보드는 30초 안 오면 잊고,
 * 전원을 빼면 사라진다. 잃어버리면 새로 만들면 되고 그 15초만 남으로 보인다.
 *
 * 앞에 기기 종류를 붙인다. 사람에게 "ios-8c21 이 쓰고 있습니다" 라고
 * 보여줄 수 있어야 해서다. 무작위 열두 자리만 보여주면 누군지 모른다.
 */
const APPID_KEY = "board.appid.v1";

function appId(): string {
  let v = localStorage.getItem(APPID_KEY);
  if (!v) {
    const r = Math.floor(Math.random() * 0xffff).toString(16).padStart(4, "0");
    v = `${plat.caps().os}-${r}`;
    localStorage.setItem(APPID_KEY, v);
  }
  return v;
}

function knownHosts(): string[] {
  try {
    const v = JSON.parse(localStorage.getItem(HOSTS_KEY) ?? "[]");
    return Array.isArray(v) ? v.filter((x) => typeof x === "string") : [];
  } catch { return []; }
}

/** 아는 주소들을 한 번씩만 두드려 본다. 답하면 그 주소를 돌려준다. */
async function knock(hosts: string[]): Promise<string | null> {
  const inp = $("host") as HTMLInputElement;
  for (const h of hosts) {
    inp.value = h;
    try { if ((await askBoard("/api/status", 3000)).ok) return h; }
    catch { /* 그 주소엔 아무도 없다. 다음 */ }
  }
  return null;
}

/**
 * 보드가 그 주소로 대답할 때까지 두드린다.
 *
 * ★ 한 번 물어보는 제한 시간을 짧게 잡는다.
 *
 *   보드는 `wifi on` 을 받고 0.7초에 공유기에 붙고 1.8초면 대답한다
 *   [확인: 2026-08-27 실측]. 그런데 예전에는 한 번에 6초를 기다렸다.
 *   그래서 아직 안 뜬 보드에 첫 번째로 물어보고는 **6초를 그냥 서 있었다.**
 *   보드가 빠른데 앱이 느렸다.
 *
 *   이제 1.2초씩 짧게, 대신 자주 물어본다. 전체 시간은 그대로다.
 */
async function waitForBoard(hosts: string[], ms: number): Promise<string | null> {
  const TRY_MS = 1200;
  const until = Date.now() + ms;
  const inp = $("host") as HTMLInputElement;
  while (Date.now() < until) {
    for (const h of hosts) {
      inp.value = h;
      try {
        const r = await askBoard("/api/status", TRY_MS, new AbortController(), TRY_MS);
        if (r.ok) return h;
      } catch { /* 아직 안 떴다. 다음 주소를 두드린다 */ }
      if (Date.now() > until) break;
    }
    await new Promise((r) => setTimeout(r, 250));
  }
  return null;
}

// ── 4초마다 연락하기 ────────────────────────────────────────────────────
//
// 보드 WiFi 를 켜 두면 전기를 먹고 BLE 도 안 돌아온다. 다 쓰면 꺼야 한다.
// 그런데 사람에게 "다 받았으면 이 단추를 누르세요" 라고 시킬 일이 아니다.
// 안 누르고 노트북을 덮으면 그만이다.
//
// 그래서 앱이 4초마다 보드에 짧은 요청을 보낸다. 요청이 끊기면 보드가
// 15초 뒤에 스스로 끈다. 앱이 죽었는지, 노트북을 덮었는지, 배가 멀어졌는지
// 따질 필요가 없다. 요청이 안 오는 것 하나로 충분하다.
//
//   빌리는 시간   15초    이만큼 요청이 없으면 보드가 끈다
//   보내는 주기    4초    세 번까지 놓쳐도 안 끊긴다
/**
 * 보드를 지금 누가 쓰고 있는지. 보드가 연락(ping) 답에 같이 보내 준다.
 *
 * 왜 필요하냐면 — **보드는 한 번에 한 대만 상대한다.** 다른 기기가 파일을
 * 받는 중이면 내 요청은 그냥 멈춰 있다. 4MB 를 받는 동안 다른 기기의 상태
 * 물어보기가 21.7초 기다린 적이 있다 [확인: 2026-08-27 실측].
 * 이유를 모르면 사람은 앱이 고장 난 줄 안다.
 *
 * busy 는 거의 못 본다. 받는 동안에는 보드가 대답을 못 하니, 내 물음의 답이
 * 올 때쯤엔 이미 받기가 끝나 있다. 그래서 보드가 last_* 로 방금 누가
 * 무엇을 받았는지 같이 알려준다. 그걸로 왜 기다렸는지 설명한다.
 */
interface BoardWho {
  you: string; users: number;
  busy: string; busy_file: string;
  last_ip: string; last_file: string; last_ago_s: number; last_took_ms: number;
}
let who: BoardWho | null = null;

/** 다른 기기가 쓰고 있나. 목록·받기가 느릴 때 이걸 보고 설명한다. */
function othersHere(): boolean {
  return !!who && who.users > 1;
}

/** 남이 방금 받았거나 지금 받는 중이면 사람 말로. 없으면 빈 글자. */
function othersDoing(): string {
  if (!who) return "";
  if (who.busy && who.busy !== who.you) {
    return `${who.busy} 가 ${who.busy_file} 를 받는 중입니다`;
  }
  if (who.last_ip && who.last_ip !== who.you && who.last_ago_s <= 60) {
    const sec = (who.last_took_ms / 1000).toFixed(1);
    return `${who.last_ago_s}초 전에 ${who.last_ip} 가 ` +
           `${who.last_file} 를 받았습니다 (${sec}초 걸림)`;
  }
  return "";
}

const LEASE_S = 15;
const PING_MS = 4000;
let pinger: ReturnType<typeof setInterval> | null = null;

function keepAliveStart() {
  if (pinger) return;
  // 연달아 몇 번 놓쳤나. 한 번 성공하면 0 으로 돌아간다.
  let missed = 0;
  const tick = async () => {
    // 파일을 받는 중이면 보내지 않는다. 보드는 한 번에 한 사람만 상대하고,
    // 받는 것 자체가 앱이 살아 있다는 표시다.
    if (fetching) return;
    try {
      const r = await askBoard(
        `/api/ping?lease=${LEASE_S}&id=${encodeURIComponent(appId())}`, 5000);
      const j = await r.json();
      if (j && typeof j.users === "number") { who = j as BoardWho; renderSide(); }
      missed = 0;
    }
    catch {
      // 한두 번 놓치는 건 흔하다. 보드가 빌린 시간이 15초니 4초짜리를
      // 세 번 놓치면 보드 쪽도 이미 끈 뒤다. **거기서 화면도 같이 놓는다.**
      //
      // 예전에는 여기서 조용히 넘어갔다. 그래서 보드가 죽어 사라진 뒤에도
      // 앱은 "연결 해제" 단추와 파일 목록을 그대로 보여줬다. 사람이 받기를
      // 누르고 나서야 알았다.
      // 다른 기기가 큰 파일을 받는 중이면 보드는 멀쩡한데도 대답을 못 한다.
      // 그때 "끊겼습니다" 라고 하면 거짓말이다. 더 기다린다.
      const limit = othersHere() ? 8 : 3;    // 32초 / 12초
      if (++missed < limit) {
        if (missed === 2 && othersHere()) {
          setStatus("보드가 다른 기기를 상대하는 중입니다. 기다립니다…");
        }
        return;
      }
      keepAliveStop();
      boardFiles = [];
      setStatus("보드와 연락이 끊겼습니다. 배 찾기로 다시 깨우세요.", "bad");
      renderSide();
    }
  };
  void tick();
  pinger = setInterval(() => void tick(), PING_MS);
  renderSide();
}

function keepAliveStop() {
  boardLinked = null;
  if (!pinger) return;
  clearInterval(pinger);
  pinger = null;
  renderSide();
}

/** 다 썼다고 알린다. 못 해도 그만이다 — 요청이 끊기면 보드가 알아서 끈다. */
async function sleepBoard(quiet = false) {
  keepAliveStop();
  try {
    const r = await askBoard("/api/wifi/off", 4000);
    // 보드는 다른 기기가 파일을 받는 중이면 안 끈다 (PROTOCOL.md).
    // 그때는 우리가 손을 놓기만 하면 된다. 마지막 사람이 나가면 보드가
    // 빌린 시간이 지나고 스스로 끈다.
    if (!r.ok) {
      if (!quiet) {
        setStatus("다른 기기가 파일을 받는 중이라 안 껐습니다. " +
                  "그쪽이 끝나면 저절로 꺼집니다.", "bad");
        renderSide();
      }
      return;
    }
    boardFiles = [];
    if (!quiet) {
      setStatus("보드 WiFi 를 껐습니다. 블루투스로 다시 찾을 수 있습니다.", "good");
      renderSide();
    }
  } catch (e) {
    if (!quiet) setStatus(`끄기 실패 — ${boardWhy(e)}`, "bad");
  }
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
      const open = e.id === openId;
      return `<div class="file ${open ? "on" : ""}" data-id="${e.id}">
        <div>${e.starred ? "★ " : ""}<b>${esc(who)}</b> ${bad}</div>
        <div class="dim">${when} · ${dur} · ${esc(sub) || e.module.slice(-5)}</div>
      </div>` + (open ? `<div class="detail" id="details"></div>` : "");
    }).join("");
    return head + rows;
  }).join("");

  box.querySelectorAll<HTMLElement>(".file").forEach((el) => {
    el.onclick = () => openEntry(el.dataset.id!);
  });
  // 연 세션의 줄 밑에 그 세션의 정보를 편다. 정보는 "지금 연 세션의 속성"
  // 이라 목록과 떼어 놓을 이유가 없다.
  renderDetails();
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
  library = lib.noteOpen(library, id);   // 앱을 껐다 켜면 이걸 다시 연다
  const s2 = loadBytes(await lib.readEntry(e), e.title || e.sailor || e.file);
  // 예전에 0 으로 적어 둔 항목이면 지금 되찾은 값으로 고쳐 준다.
  // 다시 받을 필요가 없다 — 원본은 이미 보관함에 있다.
  if (s2 && (e.navRows !== s2.header.navRows || e.utcStart !== s2.header.utcStart)) {
    library = lib.update(library, id, {
      navRows: s2.header.navRows, imuRows: s2.header.imuRows,
      durationS: s2.header.durationS, utcStart: s2.header.utcStart,
    });
  }
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

/**
 * 연 세션의 정보. **목록 안, 그 줄 바로 밑에 편다.**
 *
 * 예전에는 따로 서랍이 있었는데, 정보는 "지금 연 세션의 속성" 이라 목록과
 * 떼어 놓을 이유가 없었다. 서랍을 갈아 끼우며 "이게 어느 세션 거였지" 를
 * 되짚을 일도 없어진다.
 */
function renderDetails() {
  const box = document.getElementById("details");
  if (!box) return;                       // 목록에 연 세션이 없으면 자리도 없다
  const e = library.entries.find((x) => x.id === openId);
  if (!e) { box.innerHTML = ""; return; }

  box.innerHTML = `
    <div class="drow">
      <button id="star" class="${e.starred ? "on" : ""}">${e.starred ? "★" : "☆"}</button>
      <span class="dim">${e.module} · 세션 ${e.session} · ${(e.bytes / 1048576).toFixed(2)} MB</span>
      ${e.verified ? "<span class='good'>검사 통과</span>" : "<span class='bad'>검사 실패</span>"}
    </div>
    <div id="meta">${e.id === openId ? metaHtml : ""}</div>
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
    renderSide();
  };
}

/**
 * 마킹 목록.
 *
 * 배에서 찍힌 것과 코치가 더한 것을 한 줄씩 늘어놓는다. 누르면 그 자리로
 * 간다. 타임라인에서 작은 깃발을 찾아 누르는 것보다 빠르다.
 */
function renderMarkList() {
  const box = $("markList");
  if (!session) { box.innerHTML = "<div class='dim pad'>파일을 먼저 여세요.</div>"; return; }
  if (!marks.length) {
    box.innerHTML = "<div class='dim pad'>아직 마킹이 없습니다.<br>" +
                    "파란 고정선을 옮기고 위의 ＋ 마킹 을 누르세요.</div>";
    return;
  }
  box.innerHTML = marks.map((m, i) => `
    <div class="file mrow" data-i="${i}">
      <div><b>⚑ ${i + 1}</b>
        <span class="mono dim">${tl.formatDuration(m.ms - originMs())}</span>
        ${m.from === "file" ? "<span class='dim'>배</span>" : ""}</div>
      <div class="dim">${m.note ? m.note : "<i>메모 없음</i>"}</div>
    </div>`).join("");

  box.querySelectorAll<HTMLElement>(".mrow").forEach((el) => {
    el.onclick = () => {
      const m = marks[+(el.dataset.i ?? -1)];
      if (!m) return;
      pinMs = m.ms;
      cursorMs = m.ms;
      // 화면 밖이면 그 자리가 보이게 옮긴다
      if (m.ms < view.from || m.ms > view.to) {
        const span = view.to - view.from;
        view = { from: m.ms - span / 2, to: m.ms + span / 2 };
        clampView();
      }
      seekVideoTo(m.ms);
      redraw();
    };
  });
}

/** 보드를 누가 쓰고 있는지 한 줄로. 나 혼자면 아무것도 안 보여준다. */
function whoLine(): string {
    if (!who || !pinger) return "";
    const doing = othersDoing();
    if (who.users <= 1 && !doing) return "";
    const head = who.users > 1
      ? `이 보드를 ${who.users}대가 쓰고 있습니다.` : "";
    return `<div class="pad busybar">${head}${head && doing ? "<br>" : ""}${doing}</div>`;
}

function renderFileList(files: FileInfo[]) {
  syncBoardBar();
  const box = $("boardFiles");
  if (!files.length) {
    box.innerHTML = whoLine() +
      "<div class='dim pad'>보드에서 목록을 받아오세요.</div>";
    return;
  }

  const have = new Set(library.entries.map((e) => e.id));
  box.innerHTML = whoLine() + files
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
  setStatus(othersHere()
    ? `${name} 받는 중… (다른 기기도 이 보드를 씁니다. 줄을 설 수 있습니다)`
    : `${name} 받는 중…`);
  setProgress(size ? `${name}  0 / ${MB(size)} MB` : `${name} 받는 중…`, size ? 0 : null);
  const t0 = performance.now();

  // 90 MB 는 몇 분 걸릴 수 있으니 전체 제한 시간은 안 건다. 대신 **끊긴 것을
  // 본다** — 한동안 한 바이트도 안 오면 그만둔다. 배가 멀어져서 WiFi 가
  // 끊기면 받기가 그냥 멈춰 있는데, 그걸 영원히 기다리면 안 된다.
  //
  // 기다리는 시간을 둘로 나눈다.
  //   첫 바이트까지   120초   다른 기기가 받는 중이면 줄을 선다. 16MB 한 개가
  //                          300 KB/초로 53초 걸리니 그만큼은 기다려 줘야 한다
  //   시작한 뒤로는   20초    한 번 흐르기 시작하면 멈추는 건 진짜 문제다
  //
  // 예전에는 둘 다 20초였다. 그래서 다른 기기가 큰 파일을 받는 중이면
  // 줄 서 있다가 20초 만에 포기했다.
  const FIRST_MS = 120000;
  const STALL_MS = 20000;
  const ac = new AbortController();
  let started = false;
  const why = () => started
    ? "20초 동안 아무것도 안 왔습니다"
    : "보드가 계속 다른 일을 하고 있습니다";
  let stall = setTimeout(() => ac.abort(new Error(why())), FIRST_MS);
  const alive = () => {
    clearTimeout(stall);
    stall = setTimeout(() => ac.abort(new Error(why())), started ? STALL_MS : FIRST_MS);
  };

  try {
    const r = await askBoard(`/file/${name}`, null, ac);
    alive();
    if (!r.ok) {
      setProgress(null);
      // 409 는 다른 기기가 받는 중이라는 뜻이다 (PROTOCOL.md).
      // 이건 고장이 아니라 순서를 기다리라는 말이므로 그렇게 말한다.
      if (r.status === 409) {
        const j = await r.json().catch(() => null) as
          { owner?: string; owner_file?: string } | null;
        setStatus(
          j?.owner
            ? `${j.owner} 가 ${j.owner_file} 를 받는 중입니다. 끝나면 다시 누르세요.`
            : "다른 기기가 받는 중입니다. 끝나면 다시 누르세요.", "bad");
        return;
      }
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
        started = true;      // 흐르기 시작했다. 이제부터는 20초만 본다
        alive();
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
    const also = othersDoing();
    setStatus(`받기 실패 — ${boardWhy(e)}` + (also ? ` · ${also}` : ""), "bad");
  } finally {
    clearTimeout(stall);
    fetching = false;
    document.querySelectorAll<HTMLButtonElement>(".file .get")
      .forEach((b) => { b.disabled = false; });
  }
}

// ── 뼈대 ────────────────────────────────────────────────────────────────
//
// 화면을 사방으로 나눈다.
//
//   위     시키는 것 — 단추만 한 줄
//   가운데  보는 것 — 영상·지도(위) / 데이터(아래)
//   오른쪽  지금 것의 속성 — 서랍 하나를 아이콘으로 갈아 끼운다
//   아래    지금 상태 — 한 줄
//
// **왼쪽에는 서랍을 두지 않는다.** 그 자리는 타임라인의 이름 칸이 쓴다
// (Speed Over Ground, Heel …). 양옆에 서랍을 두면 가운데가 양쪽에서
// 깎이는데, 타임라인은 높이보다 너비가 생명이다.

type Tab = "lib" | "board" | "mark" | "set";
const DOCK_KEY = "dock.v2";

/** 지금 열린 서랍. null 이면 닫혀 있다. */
let tab: Tab | null = "lib";

function showTab(t: Tab | null) {
  tab = t;
  $("dock").classList.toggle("shut", t === null);
  document.querySelectorAll<HTMLElement>("#dock .drawer").forEach((d) => {
    d.hidden = d.dataset.tab !== t;
  });
  document.querySelectorAll<HTMLElement>("#rail .rbtn").forEach((b) => {
    b.classList.toggle("on", b.dataset.tab === t);
  });
  localStorage.setItem(DOCK_KEY, t ?? "");
  panes.refresh();
  requestAnimationFrame(() => { tmap?.resize(); redraw(); });
  if (t) renderTab(t);
}

/** 아이콘을 누르면. 켜져 있는 것을 다시 누르면 닫는다. */
function hitTab(t: Tab) { showTab(tab === t ? null : t); }

function renderTab(t: Tab) {
  if (t === "lib") renderLibrary();
  else if (t === "board") { renderBoards(); renderFileList(boardFiles); }
  else if (t === "mark") renderMarkList();
  else renderTheme();
}

/** 어느 서랍이 열려 있든, 바뀐 곳만 다시 그린다. */
function renderSide() { if (tab) renderTab(tab); }

// ── 색 판 고르기 ────────────────────────────────────────────────────────
//
// 넷을 둔다. 잉크·심해·전기는 어둡고 종이는 밝다.
//
// 로고가 순수 파랑에 흰색이다 (brand/FETMMarine.png). 파랑기 도는 회색
// 배경을 쓰면 그 파랑이 죽는다. 그래서 넷 다 그 파랑을 살리는 쪽으로 짰다.
//
// 타임라인은 캔버스라 CSS 가 안 먹는다. 색을 styles.css 한 군데에 모아 두고
// timeline.ts 가 거기서 읽어 간다. 그래서 여기서는 data-theme 만 바꾸고
// 다시 그리라고 시키면 된다.
type Theme = "ink" | "sea" | "paper" | "electric" | "vivid" | "auto";
const THEMES: Theme[] = ["ink", "sea", "paper", "electric", "vivid", "auto"];
const THEME_KEY = "theme.v2";
const DARK_KEY = "themeDark.v1";

function readTheme(): Theme {
  const v = localStorage.getItem(THEME_KEY);
  return THEMES.includes(v as Theme) ? (v as Theme) : "auto";
}
let theme: Theme = readTheme();
/** 맥이 어두울 때 쓸 판. 사람이 마지막으로 고른 어두운 판을 기억한다. */
let darkPick: Theme =
  (localStorage.getItem(DARK_KEY) as Theme) ?? "sea";

const sysLight = matchMedia("(prefers-color-scheme: light)");

function applyTheme() {
  const real = theme !== "auto" ? theme
             : sysLight.matches ? "paper" : darkPick;
  document.documentElement.dataset.theme = real;
  if (theme !== "auto" && theme !== "paper") {
    darkPick = theme;
    localStorage.setItem(DARK_KEY, theme);
  }
  localStorage.setItem(THEME_KEY, theme);
  renderTheme();
  // 선 색도 판마다 다르다. 다시 읽어서 새로 만든다.
  if (session) buildSeries(session);
  // 캔버스는 스스로 다시 안 그린다. 색을 새로 읽게 시킨다.
  //
  // 미루지 않고 바로 그린다. requestAnimationFrame 은 창이 가려져 있으면
  // 아예 안 돈다 — 그러면 색이 안 바뀐 채로 남는다 (실제로 그랬다).
  redraw();
}

function renderTheme() {
  document.querySelectorAll<HTMLElement>("#themeSeg button").forEach((b) => {
    b.classList.toggle("on", b.dataset.theme === theme);
  });
  renderDbgBtn();
}

sysLight.addEventListener("change", () => { if (theme === "auto") applyTheme(); });

// ── 영상·지도 접기 ──────────────────────────────────────────────────────
//
// 영상만 볼 때, 지도만 볼 때가 있다. 칸 오른쪽 위의 ✕ 로 접는다.
// **아주 없애지는 않는다** — 얇은 띠로 남겨야 어떻게 되돌리는지 보인다.
type MediaKey = "video" | "map" | "data";
const MEDIA_EL: Record<MediaKey, string> = {
  video: "videoPane", map: "mapPane", data: "dataPane",
};
const MEDIA_NAME: Record<MediaKey, string> = {
  video: "영상", map: "지도", data: "데이터",
};
// ★ v2 가 아니라 v3 다. 글자를 키우면서 창들의 알맞은 폭이 다 바뀌었다.
//   저장해 둔 옛 배치를 그대로 불러오면 서랍이 좁아 글자가 잘린다.
//   이름을 올려 한 번만 새로 잡게 한다. 사람이 다시 끌어 놓으면 그게 남는다.
const LAYOUT_KEY = "layout.v3";
let layout: Record<MediaKey, boolean> = { video: true, map: true, data: true };

function mountPaneHandles() {
  (Object.keys(MEDIA_EL) as MediaKey[]).forEach((k) => {
    const el = $(MEDIA_EL[k]);
    if (el.querySelector(".handles")) return;
    el.classList.add("hasHandles");

    const box = document.createElement("div");
    box.className = "handles";

    // 데이터 칸에는 "전체 보기" 를 붙인다. 위 띠에 두면 볼 것을 보다가 눈이
    // 저 위로 갔다 와야 한다. 손잡이는 보고 있는 칸에 있는 게 맞다.
    if (k === "data") {
      const fit = document.createElement("button");
      fit.textContent = "↔";
      fit.className = "fit";
      fit.title = "전체 보기";
      fit.onclick = (e) => {
        e.stopPropagation();
        if (fit.classList.contains("off")) {
          setStatus("이미 전체를 보고 있습니다.");
          return;
        }
        view = { ...fullSpan };
        redraw();
      };
      box.append(fit);
    }

    // 키우기. 이 칸만 남기고 나머지를 접는다.
    //
    // 접는 기능이 이미 있으니 그것으로 만든다. 접힌 칸은 얇은 띠로 남아서
    // 하나씩 도로 펼 수도 있고, 이 단추를 다시 누르면 한 번에 돌아온다.
    const big = document.createElement("button");
    big.className = "big";
    big.textContent = "⤢";
    big.title = `${MEDIA_NAME[k]} 크게 보기`;
    big.onclick = (e) => { e.stopPropagation(); toggleMax(k); };
    box.append(big);

    const shut = document.createElement("button");
    shut.textContent = "✕";
    shut.title = `${MEDIA_NAME[k]} 접기`;
    shut.onclick = (e) => { e.stopPropagation(); toggleMedia(k); };
    box.append(shut);

    const rail = document.createElement("button");
    rail.className = "rail";
    rail.innerHTML = `<span>▸</span><span class="railname">${MEDIA_NAME[k]}</span>`;
    rail.title = `${MEDIA_NAME[k]} 펼치기`;
    rail.onclick = () => toggleMedia(k);

    el.append(box, rail);
  });
}

/** 지금 혼자 커져 있는 칸. 없으면 null. */
let maxed: MediaKey | null = null;
/** 키우기 전의 배치. 되돌릴 때 쓴다. */
let beforeMax: Record<MediaKey, boolean> | null = null;

/** 이 칸만 남기고 나머지를 접는다. 다시 누르면 원래 배치로 돌아온다. */
function toggleMax(k: MediaKey) {
  if (maxed === k) {
    if (beforeMax) layout = { ...beforeMax };
    maxed = null;
    beforeMax = null;
  } else {
    // 이미 다른 칸이 커져 있으면 그때 저장해 둔 배치를 그대로 물려받는다.
    // 안 그러면 "커진 상태" 가 원래 배치로 기억돼 되돌릴 자리를 잃는다.
    if (!beforeMax) beforeMax = { ...layout };
    maxed = k;
    (Object.keys(layout) as MediaKey[]).forEach((o) => { layout[o] = (o === k); });
  }
  applyLayout();
}

function toggleMedia(k: MediaKey) {
  const on = (Object.keys(layout) as MediaKey[]).filter((x) => layout[x]);
  if (layout[k] && on.length === 1) {
    setStatus("마지막 칸입니다. 다른 칸을 먼저 펼치세요.", "bad");
    return;
  }
  layout = { ...layout, [k]: !layout[k] };
  // 손으로 하나를 건드렸으면 "크게 보기" 는 더 이상 유효하지 않다.
  // 그 상태를 들고 있으면 되돌리기가 엉뚱한 배치로 간다.
  maxed = null;
  beforeMax = null;
  applyLayout();
}

function applyLayout() {
  (Object.keys(MEDIA_EL) as MediaKey[]).forEach((k) => {
    const el = $(MEDIA_EL[k]);
    el.classList.toggle("mini", !layout[k]);
    // .shut 은 panes.ts 가 보는 이름이다. 생김새(.mini/.rowmini)와 갈라 둔다 —
    // 생김새를 바꾸다가 크기 셈이 같이 틀어지면 안 된다.
    el.classList.toggle("shut", !layout[k]);
  });
  // 한 줄에 담긴 둘이 다 접히면 그 줄 자체가 얇은 띠가 된다.
  //
  // ★ .mini 가 아니라 .rowmini 다. .mini 를 붙이면 그 안의 칸들이 통째로
  //   사라지고 되살릴 띠까지 없어진다 (styles.css 의 주석 참조).
  //
  // ★ 담긴 둘이 배치마다 다르다. 보통은 영상·지도가 한 줄이고,
  //   영상을 왼쪽 전체로 놓으면 지도·데이터가 한 줄이다.
  const bothShut = shape === "videoLeft"
    ? (!layout.map && !layout.data)
    : (!layout.video && !layout.map);
  $("mediaRow").classList.toggle("rowmini", bothShut);
  $("mediaRow").classList.toggle("shut", bothShut);

  // 키우기 단추는 지금 상태를 보여준다. 커져 있으면 되돌리기 모양이 된다.
  (Object.keys(MEDIA_EL) as MediaKey[]).forEach((k) => {
    const b = $(MEDIA_EL[k]).querySelector<HTMLElement>(".handles .big");
    if (!b) return;
    const on = maxed === k;
    b.textContent = on ? "⤡" : "⤢";
    b.title = on ? `${MEDIA_NAME[k]} 원래대로` : `${MEDIA_NAME[k]} 크게 보기`;
    b.classList.toggle("on", on);
  });

  // 접거나 폈으니 나누개를 다시 놓는다. 접힌 칸 옆에 나누개가 남아 있으면
  // 잡아 끌어도 움직일 게 없어서 고장 난 것처럼 보인다.
  panes.refresh();

  localStorage.setItem(LAYOUT_KEY, JSON.stringify(layout));
  if (layout.map) { const m = mapUp(); requestAnimationFrame(() => m?.resize()); }
  redraw();
}

// ── 가운데를 위아래로 놓을지 좌우로 놓을지 ──────────────────────────────
//
// 기본은 위아래다. 영상·지도가 위에 나란히, 데이터가 아래에 가로 전체.
// 타임라인은 너비가 생명이라 가로로 까는 편이 훨씬 잘 보인다.
/* ── 세로 영상 배치 ──────────────────────────────────────────────────
 *
 * 요즘 영상은 세로가 많다 (9:16). 세로 영상은 **높이가 밑천**인데, 기본
 * 배치는 영상을 납작한 칸에 넣는다. 재보면 이렇다.
 *
 *   기본 배치        칸 611x257   영상 145x257    칸의 76%가 검은 띠
 *   영상 왼쪽 전체    칸 340x604   영상 340x604    띠가 없다
 *
 * 영상 넓이가 5.5배다. 지도와 데이터도 안 좁아진다 (882 폭).
 *
 * 그래서 세로 영상을 열면 칸을 이렇게 옮긴다.
 *
 *   보통                        영상 왼쪽 전체
 *   ┌───────┬───────┐          ┌─────┬───────────┐
 *   │ 영상  │ 지도  │          │     │   지도    │
 *   ├───────┴───────┤          │영상 ├───────────┤
 *   │    데이터     │          │     │  데이터   │
 *   └───────────────┘          └─────┴───────────┘
 *
 * 옮기는 것은 둘뿐이다. 영상을 영상·지도 줄에서 빼내 가운데의 첫 칸으로,
 * 데이터를 그 줄 안으로 넣는다. 그러면 그 줄이 오른쪽 세로줄이 된다.
 */
type Shape = "normal" | "videoLeft";
const SHAPE_KEY = "shape.v1";
let shape: Shape =
  localStorage.getItem(SHAPE_KEY) === "videoLeft" ? "videoLeft" : "normal";
/** 사람이 배치를 손으로 정했나. 그러면 영상을 열어도 안 건드린다. */
let shapeByHand = localStorage.getItem(SHAPE_KEY + ".byHand") === "1";
/** 영상 칸을 마지막으로 맞춰 준 너비. 사람이 끌었는지 가리는 데 쓴다. */
let fittedW = 0;

/**
 * 칸을 옮기고 나누개를 다시 단다.
 *
 * ★ 등록을 지우고 다시 한다. 통마다 담긴 칸이 달라지기 때문이다.
 *   안 지우고 또 등록하면 옛 등록이 남아서 없는 칸에 나누개를 놓으려 든다.
 */
function applyShape() {
  const C = $("center"), MR = $("mediaRow"), V = $("videoPane"), D = $("dataPane");
  const after = () => { tmap?.resize(); redraw(); };

  panes.forget(C);
  panes.forget(MR);
  fittedW = 0;      // 배치가 바뀌면 맞춰 준 폭도 뜻이 없어진다

  if (shape === "videoLeft") {
    C.insertBefore(V, MR);
    MR.appendChild(D);
    C.style.flexDirection = "row";
    MR.style.flexDirection = "column";
    panes.register(C, "row", [
      { el: V,  kind: "fixed", base: 340, min: 160 },
      { el: MR, kind: "flex",  min: 260 },
    ], after);
    // ★ 등록한 뒤에 폭을 다시 박는다.
    //   panes 는 칸마다 지난 크기를 이름으로 기억하는데, 보통 배치에서 영상은
    //   "몫 1.0" 으로 저장돼 있다. 여기서는 그 1.0 을 1px 로 읽어서 영상 칸이
    //   최소 크기(160)로 쪼그라들었다 (실측).
    V.style.flex = `0 0 ${fittedW || 340}px`;
    panes.register(MR, "col", [
      { el: $("mapPane"),  kind: "flex", min: 140 },
      { el: $("dataPane"), kind: "flex", min: 160 },
    ], after);
  } else {
    MR.insertBefore(V, $("mapPane"));
    C.appendChild(D);
    C.style.flexDirection = centerDir === "row" ? "row" : "column";
    MR.style.flexDirection = "row";
    panes.register(C, centerDir, [
      { el: MR, kind: "flex", min: 140 },
      { el: D,  kind: "flex", min: 160 },
    ], after);
    panes.register(MR, "row", [
      { el: V,             kind: "flex", min: 180 },
      { el: $("mapPane"),  kind: "flex", min: 180 },
    ], after);
  }

  C.classList.toggle("row", shape === "videoLeft" || centerDir === "row");
  localStorage.setItem(SHAPE_KEY, shape);
  localStorage.setItem(SHAPE_KEY + ".byHand", shapeByHand ? "1" : "0");
  applyLayout();
  requestAnimationFrame(() => { fitVideoPane(); tmap?.resize(); redraw(); });
}

/**
 * 영상 칸 너비를 영상 비율에 딱 맞춘다. 검은 띠가 사라진다.
 *
 * 사람이 나누개를 끌어 폭을 바꿨으면 안 건드린다. 자동이 사람을 이기면 안 된다.
 */
function fitVideoPane() {
  if (shape !== "videoLeft") return;
  const v = video();
  if (!v.videoWidth || !v.videoHeight) return;
  const wrap = v.parentElement as HTMLElement;
  const h = wrap.getBoundingClientRect().height;
  if (h < 60) return;
  const cur = $("videoPane").getBoundingClientRect().width;
  if (fittedW && Math.abs(cur - fittedW) > 4) return;   // 사람이 끌었다
  const want = Math.round(h * (v.videoWidth / v.videoHeight));
  $("videoPane").style.flex = `0 0 ${want}px`;
  fittedW = want;
}

// [시험용] 브라우저 탭이 숨어 있으면 영상이 아예 안 열려서 (크롬이 숨은
// 탭의 디코딩을 미룬다) 배치 바꾸기를 눌러 볼 수가 없다. 여기서 직접 부른다.
if (import.meta.env.DEV) {
  (window as unknown as Record<string, unknown>).__shape =
    (s: Shape) => { shape = s; shapeByHand = false; applyShape(); };
}

/** 세로 영상이면 배치를 바꾼다. 사람이 정해 뒀으면 그대로 둔다. */
function shapeForVideo() {
  const v = video();
  if (!v.videoWidth || !v.videoHeight) return;
  if (shapeByHand) return;
  const want: Shape = v.videoHeight > v.videoWidth ? "videoLeft" : "normal";
  if (want === shape) { fitVideoPane(); return; }
  shape = want;
  applyShape();
  setStatus(want === "videoLeft"
    ? "세로 영상입니다. 영상을 왼쪽 전체로 놓았습니다."
    : "가로 영상입니다. 배치를 되돌렸습니다.");
}

const DIR_KEY = "centerDir.v2";
let centerDir: panes.Dir =
  (localStorage.getItem(DIR_KEY) as panes.Dir) === "row" ? "row" : "col";

function applyDir() {
  // 영상을 왼쪽 전체로 놓은 동안에는 방향을 여기서 안 건드린다.
  // applyShape 이 이미 가로로 세워 뒀는데 여기서 되돌리면 배치가 깨진다.
  if (shape === "videoLeft") {
    document.querySelectorAll<HTMLElement>("#dirSeg button").forEach((b) => {
      b.classList.toggle("on", b.dataset.dir === centerDir);
    });
    return;
  }
  $("center").classList.toggle("row", centerDir === "row");
  panes.setDir($("center"), centerDir);
  document.querySelectorAll<HTMLElement>("#dirSeg button").forEach((b) => {
    b.classList.toggle("on", b.dataset.dir === centerDir);
  });
  localStorage.setItem(DIR_KEY, centerDir);
  requestAnimationFrame(() => { tmap?.resize(); redraw(); });
}

/**
 * 칸마다 나누개를 붙인다.
 *
 * 서랍은 정해진 크기를 지킨다 — 창을 넓혔는데 서랍까지 같이 넓어지면
 * 성가시다. 가운데 칸들은 남는 자리를 몫대로 나눠 갖는다.
 */
/**
 * 칸 크기가 바뀌면 그림을 다시 그린다.
 *
 * 캔버스는 자기 크기가 바뀌어도 스스로 다시 안 그린다. 전에는 크기를 바꾸는
 * 자리마다 손으로 redraw() 를 불렀는데, 한 군데라도 빠지면 그림이 옛 크기로
 * 남아서 아래가 검게 비었다 (실제로 그랬다).
 *
 * 크기를 지켜보는 쪽이 확실하다. 어디서 무슨 이유로 바뀌든 한 자리에서 잡는다.
 */
function watchSizes() {
  new ResizeObserver(() => redraw()).observe($("dataPane"));
  new ResizeObserver(() => tmap?.resize()).observe($("mapPane"));
}

function mountSplitters() {
  const after = () => { tmap?.resize(); redraw(); };
  panes.register($("shell"), "row", [
    { el: $("center"), kind: "flex", min: 320 },
    // 280 과 190 은 글자가 12px 이던 시절 값이다. 17px 로 키우면서 서랍
    // 안쪽이 244px 인데 내용은 285px 를 원하는 상태가 됐다. 같은 비율로 넓혔다.
    { el: $("dock"),   kind: "fixed", base: 340, min: 240 },
  ], after);

  // 가운데와 그 안쪽은 배치에 따라 달라진다. applyShape 가 맡는다.
  applyShape();

  // 창 크기가 바뀌면 영상 칸 너비를 다시 맞춘다. 높이가 달라지면 비율도
  // 달라지기 때문이다. 사람이 끌어 둔 폭은 fitVideoPane 이 알아서 지킨다.
  window.addEventListener("resize", () => fitVideoPane());
}

function loadLayout() {
  mountPaneHandles();
  try {
    const j = localStorage.getItem(LAYOUT_KEY);
    if (j) {
      const l = JSON.parse(j) as Partial<Record<MediaKey, boolean>>;
      if (l && typeof l === "object") {
        layout = { ...layout, ...l };
        // 다 꺼진 채로 저장돼 있으면 되돌린다. 볼 게 없어진다.
        if (!Object.values(layout).some(Boolean)) {
          layout = { video: true, map: true, data: true };
        }
      }
    }
  } catch { /* 처음이면 없는 게 정상이다 */ }

  const t = localStorage.getItem(DOCK_KEY);
  tab = t === "" ? null
      : (t === "board" || t === "mark" || t === "set") ? t : "lib";

  mountSplitters();
  watchSizes();
  renderDbgBtn();
  applyTheme();
  applyDir();
  applyLayout();
  showTab(tab);
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
  try {
    if (plat.caps().os === "ios") return await openVideoNative();

    const f = await pickFile("pickVideo");
    if (!f) return;

    // 얼마나 걸렸는지 남긴다. 시험용으로 넣었다가 그대로 둔다 — 느릴 때
    // 원인을 가려 준다. 기기에 있는 영상은 1초 안쪽이고, 아이클라우드에서
    // 내려받으면 몇십 초가 나온다.
    const t0 = performance.now();
    const v0 = video();
    const ready = new Promise<void>((res) => {
      const on = () => { v0.removeEventListener("loadedmetadata", on); res(); };
      v0.addEventListener("loadedmetadata", on);
      setTimeout(res, 20000);
    });
    await useVideoUrl(URL.createObjectURL(f), f.name, vid.blobReader(f));
    await ready;
    setStatus(`${f.name} · ${(f.size / 1048576).toFixed(0)}MB · ` +
              `${((performance.now() - t0) / 1000).toFixed(1)}초 만에 열림`, "good");
  } catch (e) {
    setStatus(`영상을 못 열었습니다 — ${e}`, "bad");
  }
}

/** 아이패드에서 영상 고르기.
 *
 *  애플 사진 고르기를 전체 화면으로 띄우고, 고른 영상을 **변환 없이 원본
 *  그대로** 받는다. 우리 부품이 하는 일이다 (plugins/tauri-plugin-videopick).
 *
 *  Tauri 의 dialog 부품으로도 전체 화면은 되는데, 그쪽은 원본 그대로 받는
 *  설정을 안 걸어 둬서 아이폰이 찍은 HEVC 영상을 다시 인코딩한다. 11초짜리
 *  영상에 3초가 걸렸다. 재보고 알았다.
 *
 *  붙이는 것은 asset:// 다. 재보니 첫 그림까지 0.1초다 — Tauri 도 wry 도
 *  Range 요청을 제대로 다룬다. 느렸던 것은 붙이기가 아니라 그 앞이었다. */
async function openVideoNative() {
  // 아이클라우드에만 있는 영상은 먼저 내려받는다. 몇십 초가 걸릴 수 있어서
  // 진행 정도를 알려 준다. 아무 말도 없으면 앱이 멎은 줄 안다.
  const tap = await addPluginListener<{ percent: number; phase: string }>(
    "videopick", "progress", (p) => {
      setStatus(p.phase === "start" || p.percent <= 0
        ? "영상을 가져오는 중… (아이클라우드에 있으면 내려받습니다)"
        : `영상을 가져오는 중… ${p.percent}%`);
    });

  const t0 = performance.now();
  let r: { path: string; name: string; size: number; shotAt: number };
  try {
    r = await invoke("plugin:videopick|pick");
  } finally {
    await tap.unregister();
  }
  if (!r || !r.path) { setStatus("영상 고르기를 그만두었습니다."); return; }
  const tPick = performance.now() - t0;

  const t1 = performance.now();
  const v0 = video();
  const shown = new Promise<void>((res) => {
    const on = () => { v0.removeEventListener("loadeddata", on); res(); };
    v0.addEventListener("loadeddata", on);
    setTimeout(res, 30000);
  });
  // 찍은 시각은 부품이 스위프트에서 읽어 같이 보내 준다. 파일을 화면 쪽으로
  // 끌어올 필요가 없다.
  await useVideoUrl(convertFileSrc(r.path), r.name, null,
                    r.shotAt > 0 ? new Date(r.shotAt) : null);
  await shown;

  setStatus(`${r.name} · ${(r.size / 1048576).toFixed(0)}MB · ` +
            `가져오기 ${(tPick / 1000).toFixed(1)}초 · ` +
            `붙이기 ${((performance.now() - t1) / 1000).toFixed(1)}초`, "good");
}

async function useVideoUrl(url: string, name: string, src: vid.RangeReader | null,
                           knownShotAt: Date | null = null) {
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
  if (knownShotAt) ft = knownShotAt;              // 부품이 이미 읽어 왔다
  else if (src) { try { ft = await vid.mp4CreationTime(src); } catch { /* 못 읽어도 된다 */ } }
  const first = session?.imu[0]?.ms ?? session?.nav[0]?.ms ?? 0;
  sync = vid.guessOffset(ft, session?.header.utcStart ?? 0, first);

  setStatus(
    ft
      ? `${name} — 파일이 적어 둔 시각 ${ft.toLocaleString()}${sync.guessed ? " 로 맞춰 봤습니다" : ""}. 어긋나면 아래에서 맞추세요.`
      : `${name} — 파일에 시각이 없습니다. 아래에서 맞추세요.`,
  );
  renderVbar();
}

/* ── 시간을 굴리는 시계 ──────────────────────────────────────────────
 *
 * 영상·지도·그래프가 다 같은 시간을 본다. 그런데 지금까지 그 시간을 굴리는
 * 것은 영상뿐이었다. 영상이 없으면 재생이라는 게 아예 없었다. 카메라를 안
 * 달고 나간 날은 항적을 돌려볼 수 없었다.
 *
 * 이제 시계를 앱이 갖는다.
 *   영상이 물려 있으면   영상이 시계다 (timeupdate 가 커서를 민다)
 *   그 밖에는           우리가 실제 흐른 시간을 세어서 커서를 민다
 */
let playing = false;
let clockRaf = 0;
let clockPrev = 0;

/** 고정 자리는 세션 안에만 있을 수 있다.
 *
 *  왼쪽 끝을 넘겨 끌면 0보다 작은 값이 됐다. 그러면 그림 그리는 쪽에서
 *  "보는 범위 밖" 으로 치고 아예 안 그린다. 화면에서는 커서가 이름칸 뒤로
 *  숨어 버린 것처럼 보였다. 없는 시각을 가리키고 있으니 당연하다. */
function clampPin(ms: number): number {
  return Math.min(fullSpan.to, Math.max(fullSpan.from, ms));
}

/** 커서가 보는 자리 밖으로 나가면 화면을 따라 밀어 준다. */
function followCursor() {
  if (pinMs === null) return;
  if (pinMs >= view.from && pinMs <= view.to) return;
  const span = view.to - view.from;
  view.from = pinMs - span * 0.3;
  view.to = view.from + span;
  clampView();
}

/**
 * 지금 돌고 있나.
 *
 * ★ 깃발 하나만 보면 안 된다. "돌고 있다" 가 두 군데에 따로 있기 때문이다.
 *
 *   playing        시간 막대의 시계가 도나
 *   !video.paused  영상이 도나
 *
 * 싱크 전에는 이 둘이 따로 노는 게 맞다. 영상만 돌려 보고 데이터는 세워 둘
 * 수 있어야 한다. 그런데 **싱크한 뒤에는 하나여야 한다.**
 *
 * 어긋나는 자리가 있었다. 영상만 돌리는 중에 [싱크] 를 누르면 영상은 도는데
 * playing 은 꺼진 채였다. 그 상태에서 멈추려고 누르면 코드가 깃발만 보고
 * "아직 안 돈다" 로 읽어 오히려 다시 틀었다. 그래서 두 번 눌러야 멈췄다
 * [확인: 2026-08-27 화면에서 눌러 가며 재현].
 *
 * 그래서 싱크된 동안에는 둘 중 하나라도 돌면 도는 것으로 본다.
 */
function running(): boolean {
  return linked ? (playing || (videoOn && !video().paused)) : playing;
}

function playAll() {
  if (pinMs === null) pinMs = fullSpan.from;
  // 끝에 서 있으면 처음부터 다시 튼다. 안 그러면 눌러도 아무 일이 없다.
  // 맞추는 중에는 데이터 커서를 안 건드린다. 그 자리가 기준이다.
  if (pinMs >= fullSpan.to - 30) { pinMs = fullSpan.from; cursorMs = pinMs; }
  playing = true;
  // ★ 안 물린 영상은 안 튼다.
  //
  //   시간 막대는 세션의 시계다. 세션이 주인공이고 영상은 거기에 갖다
  //   붙이는 그림이다. 아직 안 붙었으면 따라올 이유가 없다. 오히려
  //   가만히 있어야 "아직 안 붙었구나" 가 한눈에 보인다.
  //
  //   맞출 때는 영상 칸의 제 슬라이더로 영상을 옮긴다. 영상만 다루는
  //   일이라 영상 칸에 있는 것이 맞다.
  if (videoOn && linked) void video().play();
  clockPrev = performance.now();
  cancelAnimationFrame(clockRaf);
  clockRaf = requestAnimationFrame(clockTick);
  renderTransport();
}

function pauseAll() {
  playing = false;
  if (videoOn) video().pause();   // 물렸든 아니든 돌고 있으면 세운다
  cancelAnimationFrame(clockRaf);
  renderTransport();
}

/* 창이 가려지면 재생을 세운다.
 *
 * 맥이든 아이패드든 창이 안 보이면 화면 갱신을 멈춘다. 그러면 우리 시계도
 * 같이 멈춘다. 그것만이면 괜찮은데, 영상은 안 멈춘다. 돌아와 보면 영상과
 * 커서가 어긋나 있다.
 *
 * 그래서 가려질 때 영상까지 같이 세운다. 자리는 그대로 남는다. 잠깐 다른
 * 걸 보고 왔는데 재생이 저만치 가 있으면 그게 더 곤란하다.
 *
 * 돌아와도 저절로 다시 틀지 않는다. 사람이 누를 때 튼다. */
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "hidden" && playing) pauseAll();
});

function clockTick(now: number) {
  if (!playing) return;
  const dt = now - clockPrev;
  clockPrev = now;
  // 영상이 물려 있으면 영상이 시계다. 여기서 또 밀면 두 번 민다.
  // 그 밖에는 (영상이 없거나, 있어도 아직 안 물렸으면) 우리가 민다.
  if (!(videoOn && linked) && pinMs !== null) {
    pinMs = pinMs + dt;
    if (pinMs >= fullSpan.to) { pinMs = fullSpan.to; cursorMs = pinMs; pauseAll(); return; }
    cursorMs = pinMs;
    followCursor();
    redraw();
  }
  clockRaf = requestAnimationFrame(clockTick);
}

/** 시간 막대를 지금 상태에 맞춘다. */
/** 시간 막대를 지금 상태에 맞춘다.
 *  여기가 다루는 것은 언제나 세션 시간이다. 영상 시간이 아니다. */
function renderTransport() {
  ($("play") as HTMLButtonElement).textContent = running() ? "⏸" : "▶";
  const sl = $("tslider") as HTMLInputElement;
  const at = pinMs ?? cursorMs ?? fullSpan.from;
  const span = Math.max(1, fullSpan.to - fullSpan.from);
  $("ttime").textContent =
    `${tl.formatDuration(at - originMs())} / ${tl.formatDuration(fullSpan.to - originMs())}`;
  if (!sliderHeld) sl.value = String(Math.round(((at - fullSpan.from) / span) * 1000));
}

function renderVbar() {
  const v = video();
  const dur = Number.isFinite(v.duration) ? v.duration : 0;
  $("vtime").textContent =
    `${tl.formatDuration(v.currentTime * 1000)} / ${tl.formatDuration(dur * 1000)}`;

  // 싱크와 어긋남 맞추기는 아래 시간 막대에 있다. 영상이 없으면 할 일이
  // 없으므로 흐리게 해서 못 누르게 한다. 자리는 비우지 않는다 — 있다가
  // 없어지면 화면이 들썩이고, 그런 기능이 있다는 것도 안 보인다.
  const sb = $("syncBtn") as HTMLButtonElement;
  sb.textContent = linked ? "싱크 ●" : "싱크";
  sb.className = linked ? "on" : "";
  sb.disabled = !videoOn;
  for (const id of ["n10", "n1", "p1", "p10"]) {
    ($(id) as HTMLButtonElement).disabled = !videoOn;
  }

  const vp = $("vplay") as HTMLButtonElement;
  vp.disabled = !videoOn;
  vp.textContent = (videoOn && !v.paused) ? "⏸" : "▶";
  vp.title = linked ? "재생 (싱크됐으니 데이터도 같이)" : "영상만 재생";

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
  // 영상이 돌고 있는 채로 물렸으면 시계도 돈다고 적어 둔다.
  // 여기서 안 맞추면 그 뒤로 계속 어긋난 채 남는다.
  if (!video().paused) playing = true;
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
      await useVideoUrl(URL.createObjectURL(blob), "sample.mp4 (시험용)", vid.blobReader(blob));
    }
  } catch { /* 없으면 그만이다 */ }
}

function wire() {
  job("open", openFile);
  job("sample", loadSample);
  job("list", listBoard);
  // ── 보드 찾기 단추들 ──
  job("btScan", scanBoards);
  job("btDrop", () => sleepBoard());
  $("byHand").onclick = () => {
    byHand = !byHand;
    renderSide();
    setStatus(byHand ? "주소를 직접 치는 방식입니다." : "블루투스로 찾습니다.");
  };

  // ── 지도 단추들 ──
  const baseSel = $("baseSel") as HTMLSelectElement;
  baseSel.innerHTML = TrackMap.bases()
    .map((b) => `<option value="${b.id}">${b.label}</option>`).join("");
  baseSel.onchange = () => mapUp()?.setBase(baseSel.value);
  ($("seamark") as HTMLInputElement).onchange = (e) =>
    mapUp()?.setSeamark((e.target as HTMLInputElement).checked);
  ($("bySog") as HTMLInputElement).onchange = (e) =>
    mapUp()?.setColorBySog((e.target as HTMLInputElement).checked);
  $("mapFit").onclick = () => mapUp()?.fit();
  document.querySelectorAll<HTMLElement>("#dirSeg button").forEach((b) => {
    b.onclick = () => {
      centerDir = b.dataset.dir as panes.Dir;
      // 사람이 배치를 정했다. 이제부터 영상을 열어도 안 건드린다.
      shapeByHand = true;
      if (shape !== "normal") { shape = "normal"; applyShape(); }
      applyDir();
      setStatus(centerDir === "row"
        ? "가운데 칸을 좌우로 놓았습니다."
        : "가운데 칸을 위아래로 놓았습니다.");
    };
  });

  $("addMark").onclick = () => {
    if (!session) { setStatus("먼저 파일을 여세요.", "bad"); return; }
    if (pinMs === null) { setStatus("파란 고정선이 없습니다.", "bad"); return; }
    addMarkAt(pinMs);
  };
  job("openVideo", openVideo);

  // ── 영상 조작 ──────────────────────────────────────────────────────
  const v = video();
  $("play").onclick = () => (running() ? pauseAll() : playAll());

  // 시간 막대의 훑는 칸. 세션 전체를 훑는다 (영상 길이가 아니다).
  const ts = $("tslider") as HTMLInputElement;
  const tsSeek = () => {
    sliderHeld = true;
    const span = Math.max(1, fullSpan.to - fullSpan.from);
    pinMs = fullSpan.from + (Number(ts.value) / 1000) * span;
    cursorMs = pinMs;
    seekVideoTo(pinMs);
    followCursor();
    renderTransport();
    redraw();
  };
  ts.addEventListener("input", tsSeek);
  ts.addEventListener("change", () => { tsSeek(); sliderHeld = false; });

  /*
   * 영상만 돌려 보는 단추.
   *
   * 아래 시간 막대의 재생과 하는 일이 다르다.
   *
   *   싱크 전   영상만 움직인다. 데이터는 가만히 있는다.
   *             영상에서 "이 순간" 을 찾아 두고 [싱크] 를 누르라는 뜻이다.
   *   싱크 후   영상이 시계다. 둘이 같이 움직이므로 시간 막대와 같은 일을
   *             한다. 그때는 아래 단추 모양도 같이 바뀐다.
   */
  $("vplay").onclick = () => {
    if (!videoOn) { setStatus("영상을 먼저 여세요.", "bad"); return; }
    if (linked) { running() ? pauseAll() : playAll(); return; }
    if (v.paused) {
      // 끝에 서 있으면 처음부터 다시 튼다. 시간 막대와 같은 규칙이다.
      if (v.duration && v.currentTime >= v.duration - 0.05) v.currentTime = 0;
      void v.play();
      setStatus("영상만 돌립니다. 같은 순간을 찾으면 [싱크] 를 누르세요.");
    } else {
      v.pause();
    }
  };

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
  // ★ 영상 사건이 playing 을 건드리지 않는다.
  //
  //   영상은 시간 막대에 딸린 것이지 주인이 아니다. 그런데 여기서
  //   playing 을 켜 두면, 멈추기를 눌러 껐다가 뒤늦게 도착한 영상의 "play"
  //   사건이 다시 켜 버린다. 영상 재생은 약속(Promise)이라 늦게 온다.
  //   그래서 한 번 눌러서는 안 멈추고 두 번 눌러야 멈췄다.
  //
  //   이제 playing 을 정하는 것은 playAll 과 pauseAll 뿐이다.
  v.addEventListener("play", () => { armFollow(); renderVbar(); renderTransport(); });
  v.addEventListener("timeupdate", syncFromVideo);
  armFollow();

  v.addEventListener("seeked", () => { seeking = false; syncFromVideo(); });
  v.addEventListener("pause", () => { renderVbar(); renderTransport(); });
  // 영상이 제 끝에 닿아 스스로 섰으면 시간 막대도 같이 선다.
  v.addEventListener("ended", () => { if (playing) pauseAll(); });
  // 화면이 영상을 못 읽으면 그것도 말해 준다. 그냥 까맣게 두면 왜인지 모른다.
  v.addEventListener("error", () => {
    const err = v.error;
    if (!err) return;
    const why = ["", "사람이 멈춤", "네트워크", "영상 형식을 못 품", "이 형식은 못 봄"][err.code] ?? "알 수 없음";
    setStatus(`영상을 못 봅니다 — ${why} (${err.code})  ‹${v.currentSrc.slice(0, 90)}›`, "bad");
  });
  v.addEventListener("loadedmetadata", () => { renderVbar(); shapeForVideo(); });

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
    if (f) await useVideoUrl(URL.createObjectURL(f), f.name, vid.blobReader(f));
  });

  // 오른쪽 아이콘 줄. 켜져 있는 것을 다시 누르면 닫힌다.
  document.querySelectorAll<HTMLElement>("#rail .rbtn").forEach((b) => {
    b.onclick = () => hitTab(b.dataset.tab as Tab);
  });
  document.querySelectorAll<HTMLElement>("#themeSeg button").forEach((b) => {
    b.onclick = () => { theme = b.dataset.theme as Theme; applyTheme(); };
  });
  $("resetLayout").onclick = () => {
    // 칸이 다 접혔거나 크기가 이상해졌을 때 빠져나오는 길.
    // 색과 보관함 내용은 안 건드린다 — 배치만 되돌린다.
    for (const k of ["layout.v2", "panes.v1", "centerDir.v2", "dock.v2"]) {
      localStorage.removeItem(k);
    }
    location.reload();
  };
  ($("dbgRows") as HTMLInputElement).onchange = (e) => {
    debugOn = (e.target as HTMLInputElement).checked;
    localStorage.setItem(DEBUG_KEY, debugOn ? "1" : "0");
    if (session) { buildSeries(session); fitRows(); redraw(); }
    setStatus(debugOn ? "디버그 값을 켰습니다." : "디버그 값을 껐습니다.");
  };
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
    // 이름 칸 위에서는 줄을 위아래로 굴린다.
    //
    // 센서가 열여섯 줄이면 한 칸에 다 안 들어간다. 그림 위에서는 예전처럼
    // 시간을 당기고 미는 게 맞고, 줄을 굴리는 건 이름 칸이 제자리다 —
    // 그 칸이 곧 줄 목록이니까.
    if (tl.onLabelColumn(c, e.clientX) && tl.canScroll()) {
      tl.scrollBy(e.deltaY);
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
  type Drag = { kind: "pan" | "over" | "scrub" | "pill" | "vbar"; x: number; from: number };
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
  // 왼쪽 두 칸의 경계를 끌면 넓어진다. 이름이 길면 넘치고, 숫자도 길 때가 있다.
  //   이름 칸 | 숫자 칸 | 그림
  let edgeDrag: "label" | "num" | null = null;

  c.addEventListener("pointerdown", (e) => {
    if (!session) return;
    if (tl.onLabelEdge(c, e.clientX)) {
      edgeDrag = "label";
      c.setPointerCapture(e.pointerId);
      return;
    }
    if (tl.onNumEdge(c, e.clientX)) {
      edgeDrag = "num";
      c.setPointerCapture(e.pointerId);
      return;
    }
    // 오른쪽 세로 굴림대
    if (tl.onVBar(c, e.clientX)) {
      drag = { kind: "vbar", x: e.clientX, from: 0 };
      c.setPointerCapture(e.pointerId);
      tl.scrollToBarY(c, e.clientY);
      redraw();
      return;
    }
    // 접기 표시(▾ ▸)를 누르면 그 줄이 접힌다
    const ch = tl.chevronAt(c, e.clientX, e.clientY);
    if (ch >= 0) { toggleRow(ch); return; }
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

    // ★ 값 갈아끼우기 단추를 **먼저** 본다. 단추가 이름 칸 안에 있어서
    //   순서를 바꾸면 단추를 눌러도 이름 고치기가 열린다.
    const altRow = tl.altAtY(c, e.clientX, e.clientY);
    if (altRow >= 0) {
      const sr = series[altRow];
      if (sr?.alt) { sr.altOn = !sr.altOn; redraw(); }
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
      pinMs = clampPin(tl.msAtX(c, view, e.clientX));
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
      const x = e.clientX - c.getBoundingClientRect().left;
      if (edgeDrag === "label") {
        tl.setLabelWidth(x);
        localStorage.setItem(LABELW_KEY, String(tl.labelWidth()));
      } else {
        tl.setNumWidth(x - tl.labelWidth());
        localStorage.setItem(NUMW_KEY, String(tl.numWidth()));
      }
      c.style.cursor = "col-resize";
      redraw();
      return;
    }
    if (drag?.kind === "vbar") {
      tl.scrollToBarY(c, e.clientY);
      cursorMs = null;
      c.style.cursor = "grabbing";
    } else if (drag?.kind === "over") {
      jumpTo(tl.msAtOverviewX(c, fullSpan, e.clientX));
      cursorMs = null;
      c.style.cursor = "grabbing";
    } else if (drag?.kind === "pill") {
      // 알약을 끌면 고정 자리가 따라온다. 영상도 (물려 있으면) 같이 간다.
      pinMs = clampPin(tl.msAtX(c, view, e.clientX));
      cursorMs = pinMs;
      seekVideoTo(pinMs);
    } else if (drag?.kind === "scrub") {
      pinMs = clampPin(tl.msAtX(c, view, e.clientX));
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
      const vbar = tl.onVBar(c, e.clientX);
      const edge = !vbar && (tl.onLabelEdge(c, e.clientX) || tl.onNumEdge(c, e.clientX));
      const chev = !edge && tl.chevronAt(c, e.clientX, e.clientY) >= 0;
      const pill = tl.onPinPill(c, e.clientX, e.clientY);
      const flag = tl.flagAt(c, e.clientX, e.clientY) >= 0;
      const axis = !pill && tl.onTimeAxis(c, e.clientY);
      const onLabel = !edge && tl.rowAtY(c, series.length, e.clientX, e.clientY) >= 0;
      cursorMs = over || onLabel || edge || axis || pill || flag || vbar
        ? null : tl.msAtX(c, view, e.clientX);
      c.style.cursor = vbar ? "grab"
        : edge ? "col-resize"
        : chev ? "pointer"             // 접기 표시
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
    if (edgeDrag) { edgeDrag = null; return; }
    if (drag?.kind === "scrub") setStatus("이 자리에 고정했습니다. Esc 로 풉니다.");
    drag = null;
    c.style.cursor = inOverview(e) ? "grab"
      : tl.onTimeAxis(c, e.clientY) ? "grab" : "crosshair";
    redraw();
  });
  c.addEventListener("pointercancel", () => {
    drag = null; edgeDrag = null; c.style.cursor = "crosshair";
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

  // 단축키는 뺐다.
  //
  // 창 전체에서 키를 받고 있었다. 그래서 보드 주소에 192.168.0.76 을 치려고
  // 1 을 누르면 왼쪽 칸이 닫혔다. 1~4 가 칸 켜고 끄기였다.
  //
  // 초점이 입력칸에 있으면 넘기는 식으로 막을 수도 있지만, 지금 있는 일은
  // 전부 단추와 마우스로 된다. 없는 게 낫다.
  //
  //   칸 켜고 끄기   칸마다 있는 ⤢ 와 ✕
  //   전체 보기      위쪽 단추
  //   당기고 밀기    휠, 그리고 스크롤바 끌기
  //   마킹 더하기    위쪽 "＋ 마킹" 단추
  //   영상 재생      영상 밑 단추

  addEventListener("resize", () => { tmap?.resize(); redraw(); });

  // 앱을 닫으면 보드 WiFi 도 끈다.
  //
  // 이건 빨리 끄려는 것뿐이고 못 해도 괜찮다. 연락이 끊기면 보드가 15초 뒤에
  // 스스로 끈다. 창이 닫히는 중이라 답을 기다릴 수도 없다.
  addEventListener("beforeunload", () => { if (pinger) void sleepBoard(true); });
  addEventListener("dragover", (e) => e.preventDefault());
  wirePickers();

  // 서랍에 더 볼 게 있으면 그쪽 가장자리를 흐리게 해서 알린다.
  //
  // 내용이 길면 서랍은 원래 위아래로 밀어서 본다. 그런데 아이패드는 손을
  // 대야만 스크롤 막대가 잠깐 보인다. 설정 서랍이 "가운데" 줄에서 끊겨
  // 있는데도 더 있는 줄 모르고 지나치게 된다. 흐린 가장자리는 가만히
  // 있어도 보인다.
  const dock = $("dock");
  const bar = document.createElement("div");
  bar.id = "dockBar";
  dock.appendChild(bar);

  const markEdges = () => {
    const d = dock.querySelector<HTMLElement>(".drawer:not([hidden])");
    const more = d ? d.scrollHeight - d.clientHeight : 0;
    dock.classList.toggle("moreDown", !!d && more > 2 && d.scrollTop < more - 2);
    dock.classList.toggle("moreUp", !!d && more > 2 && d.scrollTop > 2);

    if (!d || more <= 2) { bar.classList.remove("on"); return; }
    bar.classList.add("on");
    // 막대 길이는 "보이는 만큼 / 전체", 자리는 얼마나 내려왔는지에 비례한다.
    const track = d.clientHeight;
    const h = Math.max(28, Math.round(track * (d.clientHeight / d.scrollHeight)));
    const top = Math.round((track - h) * (d.scrollTop / more));
    bar.style.height = `${h}px`;
    bar.style.top = `${d.offsetTop + top}px`;
  };

  const watchDrawer = () => {
    const d = dock.querySelector<HTMLElement>(".drawer:not([hidden])");
    if (d && !d.dataset.scrollWatched) {
      d.dataset.scrollWatched = "1";
      d.addEventListener("scroll", markEdges, { passive: true });
      new ResizeObserver(markEdges).observe(d);
    }
    markEdges();
  };
  new MutationObserver(watchDrawer).observe(dock, { attributes: true, subtree: true,
                                                    attributeFilter: ["hidden"] });
  new ResizeObserver(watchDrawer).observe(dock);
  watchDrawer();
}

wire();
loadLayout();
redraw();
// 영상이 아직 없으므로 싱크와 어긋남 맞추기를 흐리게 해 둔다.
renderVbar();
document.body.classList.add("ready");   // 자리를 다 잡았으니 이제 보여준다
setStatus("파일을 열거나 보드에서 받으세요.");

// 이 기기에서 뭐가 되는지 먼저 물어본다 (platform.ts 참고).
// 답이 늦게 와도 앱은 이미 떠 있다. 오면 그때 화면만 다시 맞춘다.
void plat.load().then(() => { syncBoardBar(); renderSide(); });

lib.load().then(async (l) => {
  library = l;
  renderSide();

  // ── 마지막에 보던 세션을 다시 연다 ──
  //
  // 앱을 껐다 켜면 빈 화면으로 시작했다. 보관함에 원본이 그대로 있는데도
  // 사람이 매번 다시 골라야 했다. 코치는 같은 훈련을 하루에도 여러 번 본다.
  //
  // 지도까지 같이 돌아온다 — loadBytes 가 refreshMap 을 부르고, 거기서
  // setTrack → fit 으로 항적에 맞춰 화면이 옮겨 간다.
  //
  // 파일이 없어졌으면(사람이 지웠거나 다른 기기) 조용히 넘어간다.
  // 없는 걸 열려고 오류를 띄우면 켤 때마다 빨간 줄이 뜬다.
  const id = library.lastOpen;
  if (!id) return;
  const e = library.entries.find((x) => x.id === id);
  if (!e || !(await lib.hasFile(e))) { library = lib.noteOpen(library, null); return; }
  await openEntry(id);
});

// 만드는 중에만 밖에서 들여다볼 수 있게 내놓는다. 배포판에는 없다.
if (import.meta.env.DEV) {
  (window as any).__tl = tl;
  (window as any).__series = () => series.map((x) => ({ code: x.code, shut: !!x.collapsed }));
}

// 만드는 중에는 시험용 데이터를 바로 띄운다. 배포판에서는 안 그런다.
// ★ 마지막에 보던 세션이 있으면 그쪽이 이긴다. 시험용이 덮어쓰면 "껐다 켜도
//   그대로" 를 만드는 중에 확인할 수가 없다.
if (import.meta.env.DEV) {
  void lib.load().then((l) => { if (!l.lastOpen) void loadSample(); });
}








