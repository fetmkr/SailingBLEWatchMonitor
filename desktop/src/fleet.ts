// 함대 라이브 — 수신 전용 보드(boat 0)가 LoRa로 들은 배를 지도와 목록에 보여준다.
// 화면 규칙은 단순하다: 0척은 전체, 1척은 상세, 2척은 비교. 세 번째를 고르면 B가 바뀐다.

import * as ble from "./ble";
import { TrackMap, type FleetPoint, type FleetTrail, type FleetTrailPoint } from "./map";
import { decodeFleet, FleetTracker, FleetTransportCounter, freshMs, MAP_KEEP_MS, type FleetBoat } from "./fleet_model";

const $ = (id: string) => document.getElementById(id)!;
const esc = (v: string) => v.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]!));

function value(v: number | null, digits: number, unit: string) {
  return v === null ? "—" : `${v.toFixed(digits)}${unit}`;
}

function ageText(ms: number) {
  if (ms < 1500) return "방금";
  if (ms < 10000) return `${Math.floor(ms / 1000)}초 전`;
  return `${Math.floor(ms / 1000)}초 끊김`;
}

function shortestDeg(a: number, b: number) {
  let d = (b - a + 540) % 360 - 180;
  if (d === -180) d = 180;
  return d;
}

function between(a: FleetBoat, b: FleetBoat) {
  if (a.lat === null || a.lon === null || b.lat === null || b.lon === null) return null;
  const R = 6371000;
  const p1 = a.lat * Math.PI / 180, p2 = b.lat * Math.PI / 180;
  const dp = (b.lat - a.lat) * Math.PI / 180;
  const dl = (b.lon - a.lon) * Math.PI / 180;
  const h = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  const meters = 2 * R * Math.asin(Math.sqrt(h));
  const y = Math.sin(dl) * Math.cos(p2);
  const x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  const bearing = (Math.atan2(y, x) * 180 / Math.PI + 360) % 360;
  let rateKn: number | null = null;
  if (a.sogKn !== null && b.sogKn !== null && a.cogDeg !== null && b.cogDeg !== null) {
    const vec = (s: number, c: number) => ({ e: s * Math.sin(c * Math.PI / 180), n: s * Math.cos(c * Math.PI / 180) });
    const va = vec(a.sogKn, a.cogDeg), vb = vec(b.sogKn, b.cogDeg);
    rateKn = (vb.e - va.e) * Math.sin(bearing * Math.PI / 180) +
             (vb.n - va.n) * Math.cos(bearing * Math.PI / 180);
  }
  return { meters, bearing, rateKn };
}

let map: TrackMap | null = null;
let link: ble.Link | null = null;
let linkedName = "";
let boards: ble.Board[] = [];
let scanning = false;
let connecting = false;
let healthChecking = false;
let connectionGeneration = 0;
let reconnectTimer: number | null = null;
const tracker = new FleetTracker();
let selected: number[] = [];
let firstPosition = true;
let firstFitTimer: number | null = null;
const transport = new FleetTransportCounter();
let receiverRingDropped = 0;
let receiverAppDropped = 0;
let receiverNotifyFailed = 0;
const RECEIVER_KEY = "fleetReceiver.v1";
const wait = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const HISTORY_MS = 5 * 60 * 1000;
const HISTORY_MAX = 1800; // 5 Hz 시험에서도 최근 5분을 유지한다.
const TRAIL_MAX_POINTS = 30000;

interface FleetSample {
  at: number;
  sogKn: number | null;
  cogDeg: number | null;
  headingDeg: number | null;
  heelDeg: number | null;
  pitchDeg: number | null;
  rssi: number;
  snr: number;
}

const histories = new Map<number, FleetSample[]>();

interface TrailState {
  recording: boolean;
  points: FleetTrailPoint[];
  lastPacketAt: number | null;
  expectedMs: number;
  cadenceSamples: number;
  lastFrame: number | null;
  tie: number | null;
  pendingGap: boolean;
}

const trails = new Map<number, TrailState>();
let renderTimer: number | null = null;
let lastRenderAt = 0;

interface SavedReceiver { address: string; name: string }

function savedReceiver(): SavedReceiver | null {
  try {
    const v = JSON.parse(localStorage.getItem(RECEIVER_KEY) ?? "null");
    return v && typeof v.address === "string" && typeof v.name === "string" ? v : null;
  } catch { return null; }
}

function rememberReceiver(board: ble.Board) {
  localStorage.setItem(RECEIVER_KEY, JSON.stringify({ address: board.address, name: board.name }));
}

function clearReconnectTimer() {
  if (reconnectTimer !== null) window.clearTimeout(reconnectTimer);
  reconnectTimer = null;
}

function resetLiveData() {
  tracker.reset();
  histories.clear();
  trails.clear();
  selected = [];
  firstPosition = true;
  transport.reset();
  receiverRingDropped = 0;
  receiverAppDropped = 0;
  receiverNotifyFailed = 0;
  if (firstFitTimer !== null) window.clearTimeout(firstFitTimer);
  firstFitTimer = null;
}

// BLE 이벤트에서는 자료만 짧게 받아 둔다. 지도·목록·6개 SVG를 매 알림마다
// 다시 만들면 iPad WebView가 다음 알림을 늦게 받아 순번 공백이 커질 수 있다.
function scheduleRender() {
  if (renderTimer !== null) return;
  const delay = Math.max(0, 250 - (performance.now() - lastRenderAt));
  renderTimer = window.setTimeout(() => {
    renderTimer = null;
    requestAnimationFrame(() => {
      lastRenderAt = performance.now();
      render();
    });
  }, delay);
}

function newTrail(): TrailState {
  return { recording: true, points: [], lastPacketAt: null, expectedMs: 1000,
    cadenceSamples: 0, lastFrame: null, tie: null, pendingGap: false };
}

function observeTrailPacket(b: FleetBoat) {
  const s = trails.get(b.boat);
  if (!s?.recording) return;

  let gap = false;
  if (s.tie !== null && s.tie !== b.tie) gap = true;
  if (s.lastFrame !== null && b.timeValid && b.frame !== 0) {
    const step = (b.frame - s.lastFrame) >>> 0;
    if (step > 1 && step <= 120) gap = true;
  }
  if (s.lastPacketAt !== null) {
    const delta = b.receivedAt - s.lastPacketAt;
    if (delta > Math.max(350, s.expectedMs * 1.7)) {
      gap = true;
    } else if (delta >= 80 && delta <= 1500) {
      s.expectedMs = s.cadenceSamples === 0 ? delta : s.expectedMs * 0.7 + delta * 0.3;
      s.cadenceSamples++;
    }
  }
  if (gap) s.pendingGap = true;
  s.lastPacketAt = b.receivedAt;
  s.lastFrame = b.timeValid && b.frame !== 0 ? b.frame : null;
  s.tie = b.tie;

  if (b.lat === null || b.lon === null) return;
  const previous = s.points[s.points.length - 1];
  if (previous && previous.lat === b.lat && previous.lon === b.lon) return;
  s.points.push({ lat: b.lat, lon: b.lon, gapBefore: !!previous && s.pendingGap });
  s.pendingGap = false;
  if (s.points.length > TRAIL_MAX_POINTS) {
    s.points.splice(0, s.points.length - TRAIL_MAX_POINTS);
    if (s.points.length) s.points[0].gapBefore = false;
  }
}

function toggleTrail(boat: number) {
  const old = trails.get(boat);
  if (old?.recording) {
    old.recording = false;
  } else {
    // 다시 누르면 앞 시험과 섞지 않고 새 궤적을 시작한다.
    const fresh = newTrail();
    const b = tracker.boats.get(boat);
    if (b) observeTrailPacketInto(fresh, b);
    trails.set(boat, fresh);
  }
  render();
}

function observeTrailPacketInto(s: TrailState, b: FleetBoat) {
  s.lastPacketAt = b.receivedAt;
  s.lastFrame = b.timeValid && b.frame !== 0 ? b.frame : null;
  s.tie = b.tie;
  if (b.lat !== null && b.lon !== null) s.points.push({ lat: b.lat, lon: b.lon, gapBefore: false });
}

function mapTrails(): FleetTrail[] {
  return [...trails.entries()].filter(([, s]) => s.points.length > 0)
    .map(([boat, s]) => ({ boat, points: s.points }));
}

function readReceiverCounters(line: string | null) {
  const m = /\brx (\d+) ringdrop (\d+) appdrop (\d+) notifyfail (\d+)\b/.exec(line ?? "");
  if (!m) return;
  receiverRingDropped = Number(m[2]);
  receiverAppDropped = Number(m[3]);
  receiverNotifyFailed = Number(m[4]);
}

function scheduleReconnect(ms = 1200) {
  if (reconnectTimer !== null || link || connecting || !savedReceiver() || document.body.dataset.mode !== "fleet") return;
  reconnectTimer = window.setTimeout(() => {
    reconnectTimer = null;
    void reconnectSavedReceiver();
  }, ms);
}

function setState(text: string, kind: "" | "good" | "bad" = "") {
  const el = $("fleetState");
  el.textContent = text;
  el.className = kind;
}

function mapUp() {
  if (map) return map;
  map = new TrackMap($("fleetMap"));
  map.onFleetPick = choose;
  map.start();
  map.setColorBySog(false);
  map.setSeamark(($("fleetSeamark") as HTMLInputElement).checked);
  map.setDark(document.documentElement.dataset.theme !== "paper");
  return map;
}

function choose(boat: number) {
  if (tracker.conflicted(boat)) {
    selected = selected.filter((n) => n !== boat);
    setState(`${boat}번 배 번호를 서로 다른 보드가 쓰고 있습니다. 번호를 바꿔야 합니다.`, "bad");
    render();
    return;
  }
  if (selected.includes(boat)) {
    selected = selected.filter((n) => n !== boat);
  } else if (selected.length < 2) {
    selected.push(boat);
  } else {
    selected = [selected[0], boat];
  }
  render();
}

function markerPoints(now: number): FleetPoint[] {
  return [...tracker.boats.values()].filter((b) => b.lat !== null && b.lon !== null &&
    now - b.receivedAt <= MAP_KEEP_MS && !tracker.conflicted(b.boat, now)).map((b) => ({
    boat: b.boat, lat: b.lat!, lon: b.lon!, sogKn: b.sogKn,
    cogDeg: b.cogDeg, headingDeg: b.headingDeg, headingTrue: b.headingTrue,
    select: selected[0] === b.boat ? "a" : selected[1] === b.boat ? "b" : null,
    stale: now - b.receivedAt > 3000,
  }));
}

function renderList(now: number) {
  const box = $("fleetBoats");
  const ids = tracker.listedBoats(now);
  const directFresh = [...tracker.boats.values()].filter((b) => now - b.receivedAt <= freshMs(b)).length;
  const indirectOnly = ids.filter((id) => !tracker.boats.has(id)).length;
  $("fleetCount").textContent = `${directFresh}척 직접${indirectOnly ? ` · ${indirectOnly}척 간접` : ""}`;
  if (!ids.length) {
    const hint = link
      ? "수신기는 연결됐습니다. 송신 보드를 켜면 GPS 시각 전에도 확인 신호가 옵니다."
      : "오른쪽 보드 칸에서 수신 보드를 연결하세요.";
    box.innerHTML = `<div class="fleet-empty"><b>아직 들린 배가 없습니다</b><span>${hint}</span></div>`;
    return;
  }
  box.innerHTML = ids.map((boat) => {
    const b = tracker.boats.get(boat);
    if (!b) return `<div class="fleet-boat indirect">
      <span class="fleet-id">${boat}</span>
      <span class="fleet-main"><b>${boat}번 배</b><small>다른 배의 수신 목록에서 확인</small></span>
      <span class="fleet-health indirect"><i></i>본부 직접 수신 없음</span>
    </div>`;
    const age = now - b.receivedAt;
    const pick = selected[0] === b.boat ? "a" : selected[1] === b.boat ? "b" : "";
    const conflict = tracker.conflicted(b.boat, now);
    const indirect = !conflict && age > freshMs(b) && tracker.heardByOthers(b.boat, now);
    const health = conflict ? "conflict" : age <= freshMs(b) ? "live" : indirect ? "indirect" : age <= 20000 ? "late" : "lost";
    const healthText = conflict ? "번호 충돌" : indirect ? "다른 배는 수신" : ageText(age);
    const nav = conflict ? "서로 다른 보드가 같은 번호를 사용 중" : b.timeValid
      ? `SOG ${value(b.sogKn, 2, " kn")} · HDG ${value(b.headingDeg, 1, b.headingTrue ? "°T" : "°M")}`
      : "보드 켜짐 · GPS 시각 대기";
    const trail = trails.get(b.boat);
    const trailOn = !!trail?.recording;
    return `<div class="fleet-boat-row">
      <button class="fleet-boat ${conflict ? "conflict" : ""} ${pick ? `pick-${pick}` : ""}" data-boat="${b.boat}">
        <span class="fleet-id">${pick ? pick.toUpperCase() : b.boat}</span>
        <span class="fleet-main"><b>${b.boat}번 배</b><small>${nav}</small></span>
        <span class="fleet-health ${health}"><i></i>${healthText}</span>
      </button>
      <button class="fleet-trail-toggle${trailOn ? " on" : ""}" data-trail="${b.boat}" aria-pressed="${trailOn}">
        ${trailOn ? "● 녹화" : trail?.points.length ? "새로" : "궤적"}
      </button>
    </div>`;
  }).join("");
  box.querySelectorAll<HTMLElement>("[data-boat]").forEach((el) => {
    el.onclick = () => choose(Number(el.dataset.boat));
  });
  box.querySelectorAll<HTMLElement>("[data-trail]").forEach((el) => {
    el.onclick = () => toggleTrail(Number(el.dataset.trail));
  });
}

function metric(label: string, text: string, sub = "") {
  return `<div class="fleet-metric"><span>${label}</span><b>${text}</b>${sub ? `<small>${sub}</small>` : ""}</div>`;
}

function navMissingReason(b: FleetBoat) {
  if (!b.timeValid) return "GPS 시각 대기";
  if (!b.gpsFix) return "GPS fix 없음";
  return "GPS 품질 거절";
}

function boatCard(b: FleetBoat, letter: "A" | "B" | null) {
  const age = Date.now() - b.receivedAt;
  const rx = tracker.reception(b);
  const reception = rx ? ` · 수신 ${rx.percent.toFixed(1)}%${rx.missed ? ` (${rx.missed}회 누락)` : ""}` : "";
  return `<section class="fleet-card ${letter ? `pick-${letter.toLowerCase()}` : ""}">
    <div class="fleet-card-head"><span>${letter ?? b.boat}</span><b>${b.boat}번 배</b><small>${ageText(age)} · ${b.rssi} dBm · SNR ${b.snr}${reception}</small></div>
    <div class="fleet-metrics">
      ${metric("SOG", value(b.sogKn, 2, " kn"), b.sogKn === null ? navMissingReason(b) : "")}
      ${metric("HDG", value(b.headingDeg, 1, b.headingTrue ? "°T" : "°M"), b.headingDeg === null ? "IMU·자력계 값 없음" : b.headingTrue ? "진북" : "자북")}
      ${metric("COG", value(b.cogDeg, 1, "°T"), b.cogDeg === null ? navMissingReason(b) : "진북")}
      ${metric("HEEL", value(b.heelDeg, 0, "°"))}
      ${metric("PITCH", value(b.pitchDeg, 0, "°"))}
      ${metric("BAT", `${b.batteryPct}%`)}
      ${metric("REC", b.recording ? "기록 중" : "꺼짐")}
    </div>
    ${!b.timeValid
      ? `<div class="fleet-warn">보드·LoRa 응답 확인 · GPS 시각 대기 — 위치·SOG·COG만 비웁니다. HDG·자세·REC는 계속 받습니다.</div>`
      : !b.gpsFix ? `<div class="fleet-warn">GPS 위치 없음 — 마지막 위치를 새 위치처럼 쓰지 않습니다.</div>` : ""}
  </section>`;
}

function rememberSample(b: FleetBoat) {
  const list = histories.get(b.boat) ?? [];
  list.push({
    at: b.receivedAt, sogKn: b.sogKn, cogDeg: b.cogDeg, headingDeg: b.headingDeg,
    heelDeg: b.heelDeg, pitchDeg: b.pitchDeg, rssi: b.rssi, snr: b.snr,
  });
  const cutoff = b.receivedAt - HISTORY_MS;
  while (list.length && list[0].at < cutoff) list.shift();
  if (list.length > HISTORY_MAX) list.splice(0, list.length - HISTORY_MAX);
  histories.set(b.boat, list);
}

type NumberAt = (s: FleetSample) => number | null;

function graphPath(samples: FleetSample[], pick: NumberAt, min: number, max: number, now: number, circular = false) {
  let out = "", drawing = false, previous: number | null = null, points = 0;
  const step = Math.max(1, Math.floor(samples.length / 600));
  for (let i = 0; i < samples.length; i += step) {
    const sample = samples[i];
    const v = pick(sample);
    if (v === null || !Number.isFinite(v) || sample.at < now - HISTORY_MS) {
      drawing = false; previous = null; continue;
    }
    const x = 8 + Math.max(0, Math.min(1, (sample.at - (now - HISTORY_MS)) / HISTORY_MS)) * 584;
    const y = 6 + (1 - Math.max(0, Math.min(1, (v - min) / (max - min)))) * 88;
    if (circular && previous !== null && Math.abs(v - previous) > 180) drawing = false;
    out += `${drawing ? "L" : "M"}${x.toFixed(1)},${y.toFixed(1)}`;
    drawing = true; previous = v; points++;
  }
  if (points === 1) out += "l0.2,0";
  return out;
}

function chart(title: string, subtitle: string, boats: FleetBoat[], now: number,
               range: [number, number], lines: { label: string; pick: NumberAt; digits?: number; unit?: string; circular?: boolean }[]) {
  const colors = ["#38bdf8", "#fb923c"];
  const paths: string[] = [];
  const legend: string[] = [];
  boats.forEach((boat, bi) => {
    const samples = histories.get(boat.boat) ?? [];
    lines.forEach((line) => {
      const path = graphPath(samples, line.pick, range[0], range[1], now, !!line.circular);
      if (path) paths.push(`<path d="${path}" stroke="${colors[bi]}"/>`);
      const current = samples.length ? line.pick(samples[samples.length - 1]) : null;
      const shown = current === null ? "—" : `${current.toFixed(line.digits ?? 1)}${line.unit ?? ""}`;
      legend.push(`<span style="color:${colors[bi]}"><i></i>B${boat.boat} ${line.label} ${shown}</span>`);
    });
  });
  return `<section class="fleet-chart">
    <div class="fleet-chart-title"><b>${title}</b><small>${subtitle}</small></div>
    <div class="fleet-chart-legend">${legend.join("")}</div>
    <svg viewBox="0 0 600 100" preserveAspectRatio="none" aria-label="${title} 실시간 그래프">
      <path class="grid" d="M8 6H592M8 35H592M8 64H592M8 94H592" />${paths.join("")}
    </svg>
  </section>`;
}

function renderCharts(now: number) {
  const panel = $("fleetCharts");
  const body = $("fleetChartsBody");
  const chosen = selected.map((n) => tracker.boats.get(n)).filter((b): b is FleetBoat => !!b && !tracker.conflicted(b.boat));
  panel.hidden = chosen.length === 0;
  if (!chosen.length) {
    $("fleetChartScope").textContent = "배를 선택하세요";
    body.innerHTML = "";
    return;
  }
  const boats = chosen.slice(0, 2);
  $("fleetChartScope").textContent = `${boats.map((b) => `B${b.boat}`).join(" · ")} · 최근 5분`;
  const samples = boats.flatMap((b) => histories.get(b.boat) ?? []);
  const headingRefs = new Set(boats.map((b) => b.headingTrue ? "T" : "M"));
  const headingRef = headingRefs.size === 1 ? [...headingRefs][0] : "T/M";
  const headingSubtitle = headingRef === "T" ? "0–360° · 진북(T)" : headingRef === "M" ? "0–360° · 자북(M)" : "0–360° · T/M 혼합";
  const sogMax = Math.max(1, Math.ceil(Math.max(0, ...samples.map((s) => s.sogKn ?? 0)) * 1.2 * 2) / 2);
  const attitudeMax = Math.max(10, Math.ceil(Math.max(0, ...samples.flatMap((s) => [Math.abs(s.heelDeg ?? 0), Math.abs(s.pitchDeg ?? 0)])) / 5) * 5);
  body.innerHTML = [
    chart("SOG", `0–${sogMax.toFixed(1)} kn`, boats, now, [0, sogMax], [{ label: "SOG", pick: (s) => s.sogKn, digits: 2, unit: " kn" }]),
    chart("HDG", headingSubtitle, boats, now, [0, 360], [{ label: "HDG", pick: (s) => s.headingDeg, unit: `°${headingRef}`, circular: true }]),
    chart("COG", "0–360° · 진북(T)", boats, now, [0, 360], [{ label: "COG", pick: (s) => s.cogDeg, unit: "°T", circular: true }]),
    chart("HEEL", `−${attitudeMax}–+${attitudeMax}°`, boats, now, [-attitudeMax, attitudeMax], [{ label: "HEEL", pick: (s) => s.heelDeg, unit: "°" }]),
    chart("PITCH", `−${attitudeMax}–+${attitudeMax}°`, boats, now, [-attitudeMax, attitudeMax], [{ label: "PITCH", pick: (s) => s.pitchDeg, unit: "°" }]),
    chart("RSSI", "LoRa 수신 세기 · −130…−20 dBm", boats, now, [-130, -20], [{ label: "RSSI", pick: (s) => s.rssi, digits: 0, unit: " dBm" }]),
  ].join("");
}

function renderDetail() {
  const box = $("fleetDetailBody");
  const picked = selected.map((n) => tracker.boats.get(n)).filter((b): b is FleetBoat => !!b && !tracker.conflicted(b.boat));
  if (!picked.length) {
    box.innerHTML = `<div class="fleet-empty large"><b>전체 함대</b><span>목록이나 지도에서 한 척을 누르면 상세, 두 척을 누르면 비교합니다.</span></div>`;
    return;
  }
  if (picked.length === 1) { box.innerHTML = boatCard(picked[0], null); return; }
  const [a, b] = picked;
  const pos = between(a, b);
  // 수신 보드의 PPS 프레임 번호가 같아도, 송신 배가 PPS 없이 보낸 확인 신호는
  // 임의 시각의 패킷이다. 양쪽 모두 TDMA 시각이 있을 때만 같은 GPS 초로 본다.
  const framesKnown = a.timeValid && b.timeValid && a.frame !== 0 && b.frame !== 0;
  const same = framesKnown && a.frame === b.frame;
  const frameText = !framesKnown ? "수신기 GPS 시각 없음" : same ? "같은 GPS 초" : "수신 초가 다름";
  const cogDelta = a.cogDeg !== null && b.cogDeg !== null ? shortestDeg(a.cogDeg, b.cogDeg) : null;
  const sogDelta = a.sogKn !== null && b.sogKn !== null ? b.sogKn - a.sogKn : null;
  const rate = pos?.rateKn;
  box.innerHTML = `${boatCard(a, "A")}${boatCard(b, "B")}
    <section class="fleet-compare">
      <div class="fleet-compare-head"><b>A ↔ B 비교</b><span class="${same ? "good" : "bad"}">${frameText}</span></div>
      <div class="fleet-metrics">
        ${metric("거리", pos ? (pos.meters < 1000 ? `${Math.round(pos.meters)} m` : `${(pos.meters / 1852).toFixed(2)} nm`) : "—")}
        ${metric("A→B", pos ? `${pos.bearing.toFixed(0)}°T` : "—")}
        ${metric("Δ SOG", sogDelta === null ? "—" : `${sogDelta >= 0 ? "+" : ""}${sogDelta.toFixed(2)} kn`, "B − A")}
        ${metric("Δ COG", cogDelta === null ? "—" : `${cogDelta >= 0 ? "+" : ""}${cogDelta.toFixed(1)}°`, "B − A")}
        ${metric("상대 거리", rate === null || rate === undefined ? "—" : `${Math.abs(rate).toFixed(2)} kn`, rate === null || rate === undefined ? "" : rate < 0 ? "가까워짐" : "멀어짐")}
        ${metric("수신", same ? `프레임 ${a.frame}` : `${a.frame} / ${b.frame}`)}
      </div>
      ${same ? "" : `<div class="fleet-warn">${framesKnown ? "두 배가 같은 GPS 초에 받은 값이 아닙니다." : "수신 보드에 유효한 GPS PPS가 없어 같은 초의 값인지 확인할 수 없습니다."} 차이를 확정값처럼 보지 마세요.</div>`}
    </section>`;
}

function render() {
  const now = Date.now();
  renderList(now);
  renderDetail();
  renderCharts(now);
  const points = markerPoints(now);
  if (document.body.dataset.mode === "fleet" || map) {
    const liveMap = mapUp();
    liveMap.setFleet(points);
    liveMap.setFleetTrails(mapTrails());
    $("fleetTrailLegend").toggleAttribute("hidden", trails.size === 0);
    if (firstPosition && points.length && firstFitTimer === null) {
      // 첫 배 한 척에 즉시 화면을 끌려가지 않고 2초 동안 같이 들어오는 배를 모은다.
      firstFitTimer = window.setTimeout(() => {
        firstFitTimer = null;
        if (!firstPosition || !markerPoints(Date.now()).length) return;
        firstPosition = false;
        map?.fitFleet();
      }, 2000);
    }
  }
  $("fleetLinkName").textContent = linkedName || "수신 보드 연결 안 됨";
  $("fleetLinkName").closest(".fleet-link-row")?.classList.toggle("connected", !!link);
  $("fleetLinkName").closest(".fleet-board-block")?.classList.toggle("connected", !!link);
  $("fleetDisconnect").toggleAttribute("hidden", !link);
  if (!link) {
    $("fleetTransport").textContent = "연결 후 BLE·보드 누락을 측정합니다.";
    $("fleetTransport").className = "dim";
    return;
  }
  const transportText = !transport.ready
    ? transport.legacySeen ? "구형 수신 보드 · BLE 누락 측정 불가" : "보드→앱 BLE 순번 대기"
    : `보드→앱 BLE ${transport.percent.toFixed(1)}% · 앱 미수신 ${transport.missed}개`;
  const boardDrops = receiverRingDropped + receiverAppDropped + receiverNotifyFailed;
  $("fleetTransport").textContent = `${transportText} · 보드 내부 버림 ${receiverRingDropped + receiverAppDropped} · BLE 송신 실패 ${receiverNotifyFailed}`;
  $("fleetTransport").className = transport.missed || boardDrops ? "bad" : "dim";
}

function renderReceivers() {
  const box = $("fleetReceivers");
  if (!boards.length) { box.innerHTML = scanning ? `<span class="dim">0번 수신 전용 보드를 찾는 중…</span>` : ""; return; }
  box.innerHTML = boards.map((b, i) => `<button data-rx="${i}"><b>${esc(b.name)}</b><small>${b.rssi < 0 ? `${b.rssi} dBm` : "세기 모름"}</small></button>`).join("");
  box.querySelectorAll<HTMLElement>("[data-rx]").forEach((el) => {
    el.onclick = () => void connectBoard(boards[Number(el.dataset.rx)]);
  });
}

function keepReceiverBoards(list: ble.Board[]) {
  const next = list.filter((b) => b.boatId === 0);
  // 광고가 올 때마다 버튼을 다시 만들면 누르는 순간 DOM이 바뀌어 클릭이 실패한다.
  if (next.length === boards.length && next.every((b, i) => b.address === boards[i].address)) return;
  boards = next;
  renderReceivers();
}

async function scanReceivers() {
  if (scanning || connecting) return;
  scanning = true; boards = []; renderReceivers();
  const ready = await ble.ready();
  if (!ready.ok) { scanning = false; setState(ready.why, "bad"); renderReceivers(); return; }
  setState("0번 수신 전용 보드를 찾는 중…");
  try {
    await ble.scan(5000, keepReceiverBoards);
    await wait(5200);
    await ble.scanStop();
    setState(boards.length ? "연결할 수신 보드를 고르세요." : "주변에 0번 수신 전용 보드가 없습니다.", boards.length ? "" : "bad");
  } catch (e) {
    setState(`보드 찾기 실패 — ${e}`, "bad");
  } finally { scanning = false; renderReceivers(); }
}

async function connectBoard(board: ble.Board, automatic = false) {
  if (connecting) return;
  connecting = true;
  rememberReceiver(board);
  await ble.scanStop();
  let fresh: ble.Link | null = null;
  let retry = false;
  try {
    await disconnectFleet(false, false);
    // 앱이 강제 종료됐으면 plugin/CoreBluetooth에 예전 연결이 남을 수 있다.
    // 새 connect 전에 한 번 끊어 즉시 재실행해도 `Device disconnected`에 갇히지 않게 한다.
    await ble.resetConnection();
    await wait(250);
    const mine = ++connectionGeneration;
    setState(`${board.name}${automatic ? " 자동 재연결" : " 연결"} 중…`);
    let lastError: unknown = null;
    for (let attempt = 0; attempt < 2 && !fresh; attempt++) {
      try {
        fresh = await ble.Link.open(board, () => {
          if (connectionGeneration !== mine) return;
          link = null; linkedName = "";
          setState("수신 보드 연결이 끊겼습니다. 다시 연결하는 중…", "bad");
          render();
          scheduleReconnect();
        });
      } catch (e) {
        lastError = e;
        await ble.resetConnection();
        if (attempt === 0) await wait(500);
      }
    }
    if (!fresh) throw lastError ?? new Error("보드에 연결하지 못했습니다");
    await fresh.onFleet((raw) => {
      const b = decodeFleet(raw);
      if (!b) return;
      transport.ingest(b.notifySeq);
      rememberSample(b);
      tracker.ingest(b);
      observeTrailPacket(b);
      if (tracker.conflicted(b.boat)) selected = selected.filter((n) => n !== b.boat);
      scheduleRender();
    });
    // 수신 보드는 사람이 미리 0번으로 정한 보드만 받는다. 앱이 여기서 번호를
    // 몰래 0으로 바꾸면 선수가 쓰던 송신 보드를 잘못 골랐을 때 함대에서 사라진다.
    const boat = await fresh.ask("boat", 2500);
    const parsed = /^boat (\d+)$/.exec(boat ?? "");
    if (!parsed) throw new Error(boat || "배 번호를 확인하지 못했습니다");
    if (Number(parsed[1]) !== 0) {
      throw new Error(`${board.name}은 B${parsed[1].padStart(2, "0")} 송신 보드입니다. 수신 전용 0번 보드를 고르세요`);
    }
    const on = await fresh.ask("lora live on", 3000);
    if (!on?.startsWith("ok lora live on boat 0")) throw new Error(on || "장거리 수신을 켜지 못했습니다");
    link = fresh;
    linkedName = board.name;
    boards = []; renderReceivers();
    resetLiveData();
    readReceiverCounters(await fresh.ask("lora live", 2500));
    setState(`${board.name} · LoRa 수신 중`, "good");
    render();
  } catch (e) {
    ++connectionGeneration;
    try { await fresh?.close(); } catch { /* 이미 끊김 */ }
    await ble.resetConnection();
    link = null; linkedName = "";
    setState(`연결 실패 — ${e}`, "bad"); render();
    retry = true;
  } finally {
    connecting = false;
    if (retry) scheduleReconnect(3000);
  }
}

async function disconnectFleet(turnRadioOff = true, forget = false) {
  clearReconnectTimer();
  ++connectionGeneration;
  if (forget) localStorage.removeItem(RECEIVER_KEY);
  const old = link;
  link = null; linkedName = "";
  if (!old) { render(); return; }
  try { if (turnRadioOff) await old.ask("lora live off", 1500); } catch { /* 끊겨도 닫는다 */ }
  try { await old.close(); } catch { /* 이미 끊김 */ }
  setState("수신 보드 연결을 끊었습니다.");
  render();
}

async function reconnectSavedReceiver() {
  const wanted = savedReceiver();
  if (!wanted || link || connecting || scanning || document.body.dataset.mode !== "fleet") return;
  const ready = await ble.ready();
  if (!ready.ok) { setState(ready.why, "bad"); scheduleReconnect(5000); return; }

  scanning = true;
  boards = [];
  renderReceivers();
  setState(`${wanted.name} 자동 재연결 중…`);
  let found: ble.Board | null = null;
  try {
    await ble.scan(4000, (list) => {
      found = list.find((b) => b.boatId === 0 && (b.address === wanted.address || b.name === wanted.name)) ?? null;
    });
    await wait(4200);
  } catch { /* 아래에서 재시도한다 */ }
  finally {
    await ble.scanStop();
    scanning = false;
    renderReceivers();
  }
  if (found) await connectBoard(found, true);
  else {
    setState(`${wanted.name} 수신 보드를 기다리는 중…`, "bad");
    scheduleReconnect(5000);
  }
}

async function checkReceiver() {
  const current = link;
  if (!current || healthChecking || connecting || document.body.dataset.mode !== "fleet") return;
  healthChecking = true;
  try {
    let state = await current.ask("lora live", 2200);
    if (state?.startsWith("lora live off")) state = await current.ask("lora live on", 3000);
    if (!state || !state.includes("boat 0") || state.includes(" off ")) throw new Error("수신 상태 답 없음");
    readReceiverCounters(state);
    render();
  } catch {
    if (link === current) {
      ++connectionGeneration;
      link = null; linkedName = "";
      await ble.resetConnection();
      setState("수신 보드 응답이 없어 다시 연결하는 중…", "bad");
      render();
      scheduleReconnect();
    }
  } finally { healthChecking = false; }
}

function setMode(mode: "review" | "fleet") {
  const before = document.body.dataset.mode;
  document.body.dataset.mode = mode;
  document.querySelectorAll<HTMLElement>("#modeSeg button").forEach((b) => b.classList.toggle("on", b.dataset.mode === mode));
  localStorage.setItem("appMode.v1", mode);
  // 화면을 바꾸는 것만으로 수신 보드의 LoRa까지 끄면 리뷰 중 함대 자료가 끊긴다.
  // BLE 연결만 놓고 무전기는 계속 듣게 하며, 돌아오면 저장한 보드로 다시 붙는다.
  if (mode === "review" && before === "fleet") void disconnectFleet(false, false);
  if (mode === "fleet") {
    requestAnimationFrame(() => { mapUp().resize(); render(); });
    scheduleReconnect(300);
  }
}

function seedBrowserPreview() {
  if ((globalThis as any).__TAURI_INTERNALS__) return;
  const now = Date.now();
  const seed = [
    [3, 37.4492, 126.5531, 5.42, 37, -9, 2, 82],
    [7, 37.4510, 126.5560, 5.88, 42, 11, 1, 73],
    [12, 37.4475, 126.5583, 4.96, 34, -4, -1, 91],
    [18, 37.4532, 126.5514, 6.12, 48, 16, 3, 67],
  ] as const;
  for (const [boat, lat, lon, sogKn, cogDeg, heelDeg, pitchDeg, batteryPct] of seed) {
    const sample: FleetBoat = { radioVersion: 3, boat, lat, lon, sogKn, cogDeg, headingDeg: (cogDeg + 352) % 360, headingTrue: true, heelDeg, pitchDeg, batteryPct,
      gpsFix: true, recording: true, timeValid: true, changed: false, heard: 0, tie: boat,
      frame: 1042, rssi: -62 - boat, snr: 9, notifySeq: boat, receivedAt: now };
    rememberSample(sample);
    tracker.ingest(sample);
  }
  // 브라우저 미리보기에서도 녹화·5 Hz 그래프·공백 점선을 실제 흐름으로 확인한다.
  let tick = 0;
  window.setInterval(() => {
    tick++;
    const at = Date.now();
    for (const old of [...tracker.boats.values()]) {
      if (old.boat === 7 && tick % 40 >= 22 && tick % 40 <= 25) continue; // 수신 공백 예시
      const meters = (old.sogKn ?? 0) * 0.514444 * 0.2;
      const rad = (old.cogDeg ?? 0) * Math.PI / 180;
      const lat = old.lat === null ? null : old.lat + Math.cos(rad) * meters / 111320;
      const lon = old.lon === null || old.lat === null ? null : old.lon + Math.sin(rad) * meters /
        (111320 * Math.cos(old.lat * Math.PI / 180));
      const sample: FleetBoat = { ...old, lat, lon, frame: 1042 + Math.floor(tick / 5),
        notifySeq: (old.notifySeq ?? 0) + 1, receivedAt: at };
      rememberSample(sample);
      tracker.ingest(sample);
      observeTrailPacket(sample);
    }
    scheduleRender();
  }, 200);
}

export function initFleetUI() {
  document.querySelectorAll<HTMLElement>("#modeSeg button").forEach((b) => {
    b.onclick = () => setMode(b.dataset.mode === "fleet" ? "fleet" : "review");
  });
  $("fleetScan").onclick = () => void scanReceivers();
  $("fleetDisconnect").onclick = () => void disconnectFleet(true, true);
  $("fleetFit").onclick = () => mapUp().fitFleet();
  const base = $("fleetBase") as HTMLSelectElement;
  base.innerHTML = TrackMap.bases().map((b) => `<option value="${b.id}">${b.label}</option>`).join("");
  base.onchange = () => mapUp().setBase(base.value);
  ($("fleetSeamark") as HTMLInputElement).onchange = () => mapUp().setSeamark(($("fleetSeamark") as HTMLInputElement).checked);
  addEventListener("resize", () => map?.resize());
  seedBrowserPreview();
  setInterval(render, 1000);
  setInterval(() => void checkReceiver(), 10000);
  setMode(localStorage.getItem("appMode.v1") === "fleet" ? "fleet" : "review");
  render();
}
