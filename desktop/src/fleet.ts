// 함대 라이브 — 수신 전용 보드(boat 0)가 LoRa로 들은 배를 지도와 목록에 보여준다.
// 화면 규칙은 단순하다: 0척은 전체, 1척은 상세, 2척은 비교. 세 번째를 고르면 B가 바뀐다.

import * as ble from "./ble";
import { TrackMap, type FleetPoint } from "./map";

export interface FleetBoat {
  boat: number;
  lat: number | null;
  lon: number | null;
  sogKn: number | null;
  cogDeg: number | null;
  heelDeg: number | null;
  pitchDeg: number | null;
  batteryPct: number;
  gpsFix: boolean;
  recording: boolean;
  timeValid: boolean;
  changed: boolean;
  heard: number;
  tie: number;
  frame: number;
  rssi: number;
  snr: number;
  receivedAt: number;
}

const INVALID_POS = -2147483648;
const $ = (id: string) => document.getElementById(id)!;
const esc = (v: string) => v.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]!));
const u16 = (d: DataView, at: number) => d.getUint16(at, true);

/** PROTOCOL.md §10.13의 30바이트 알림을 푼다. 이상하면 조용히 버린다. */
export function decodeFleet(bytes: number[], now = Date.now()): FleetBoat | null {
  if (bytes.length !== 30) return null;
  const a = Uint8Array.from(bytes);
  const d = new DataView(a.buffer);
  if (a[0] !== 1) return null;
  const boat = a[1];
  if (boat < 1 || boat > 32) return null;
  const latRaw = d.getInt32(2, true);
  const lonRaw = d.getInt32(6, true);
  const sog = u16(d, 10);
  const cog = u16(d, 12);
  const heel = d.getInt8(14);
  const pitch = d.getInt8(15);
  const flags = a[16];
  if (latRaw !== INVALID_POS && (latRaw < -900000000 || latRaw > 900000000)) return null;
  if (lonRaw !== INVALID_POS && (lonRaw < -1800000000 || lonRaw > 1800000000)) return null;
  if (cog !== 0xffff && cog > 3599) return null;
  return {
    boat,
    lat: latRaw === INVALID_POS ? null : latRaw / 1e7,
    lon: lonRaw === INVALID_POS ? null : lonRaw / 1e7,
    sogKn: sog === 0xffff ? null : sog / 100,
    cogDeg: cog === 0xffff ? null : cog / 10,
    heelDeg: heel === -128 ? null : heel,
    pitchDeg: pitch === -128 ? null : pitch,
    batteryPct: Math.round(((flags >> 4) & 0x0f) * 100 / 15),
    gpsFix: !!(flags & 0x01),
    recording: !!(flags & 0x02),
    timeValid: !!(flags & 0x04),
    changed: !!(flags & 0x08),
    heard: d.getUint32(17, true),
    tie: u16(d, 21),
    frame: d.getUint32(23, true),
    rssi: d.getInt16(27, true),
    snr: d.getInt8(29),
    receivedAt: now,
  };
}

function value(v: number | null, digits: number, unit: string) {
  return v === null ? "—" : `${v.toFixed(digits)}${unit}`;
}

function ageText(ms: number) {
  if (ms < 1500) return "방금";
  if (ms < 10000) return `${Math.floor(ms / 1000)}초 전`;
  return `${Math.floor(ms / 1000)}초 끊김`;
}

// PPS 전 확인 신호는 5~8초마다 온다. 정상 1 Hz 자료와 같은 3초 기준을 쓰면
// 살아 있는 보드가 주기 사이마다 끊긴 것으로 보이므로 12초까지 확인 상태로 둔다.
function freshMs(b: FleetBoat) { return b.timeValid ? 3000 : 12000; }

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
const boats = new Map<number, FleetBoat>();
let selected: number[] = [];
let firstPosition = true;
const RECEIVER_KEY = "fleetReceiver.v1";
const wait = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

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
  if (map) { map.resize(); return map; }
  map = new TrackMap($("fleetMap"));
  map.onFleetPick = choose;
  map.start();
  map.setColorBySog(false);
  map.setSeamark(($("fleetSeamark") as HTMLInputElement).checked);
  map.setDark(document.documentElement.dataset.theme !== "paper");
  return map;
}

function choose(boat: number) {
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
  return [...boats.values()].filter((b) => b.lat !== null && b.lon !== null).map((b) => ({
    boat: b.boat, lat: b.lat!, lon: b.lon!, cogDeg: b.cogDeg,
    select: selected[0] === b.boat ? "a" : selected[1] === b.boat ? "b" : null,
    stale: now - b.receivedAt > 3000,
  }));
}

function renderList(now: number) {
  const box = $("fleetBoats");
  const list = [...boats.values()].sort((a, b) => a.boat - b.boat);
  $("fleetCount").textContent = `${list.filter((b) => now - b.receivedAt <= freshMs(b)).length}척 수신`;
  if (!list.length) {
    const hint = link
      ? "수신기는 연결됐습니다. 송신 보드를 켜면 GPS 시각 전에도 확인 신호가 옵니다."
      : "오른쪽 보드 칸에서 수신 보드를 연결하세요.";
    box.innerHTML = `<div class="fleet-empty"><b>아직 들린 배가 없습니다</b><span>${hint}</span></div>`;
    return;
  }
  box.innerHTML = list.map((b) => {
    const age = now - b.receivedAt;
    const pick = selected[0] === b.boat ? "a" : selected[1] === b.boat ? "b" : "";
    const health = age <= freshMs(b) ? "live" : age <= 20000 ? "late" : "lost";
    const nav = b.timeValid
      ? `${value(b.sogKn, 2, " kn")} · ${value(b.cogDeg, 1, "°T")}`
      : "보드 켜짐 · GPS 시각 대기";
    return `<button class="fleet-boat ${pick ? `pick-${pick}` : ""}" data-boat="${b.boat}">
      <span class="fleet-id">${pick ? pick.toUpperCase() : b.boat}</span>
      <span class="fleet-main"><b>${b.boat}번 배</b><small>${nav}</small></span>
      <span class="fleet-health ${health}"><i></i>${ageText(age)}</span>
    </button>`;
  }).join("");
  box.querySelectorAll<HTMLElement>("[data-boat]").forEach((el) => {
    el.onclick = () => choose(Number(el.dataset.boat));
  });
}

function metric(label: string, text: string, sub = "") {
  return `<div class="fleet-metric"><span>${label}</span><b>${text}</b>${sub ? `<small>${sub}</small>` : ""}</div>`;
}

function boatCard(b: FleetBoat, letter: "A" | "B" | null) {
  const age = Date.now() - b.receivedAt;
  return `<section class="fleet-card ${letter ? `pick-${letter.toLowerCase()}` : ""}">
    <div class="fleet-card-head"><span>${letter ?? b.boat}</span><b>${b.boat}번 배</b><small>${ageText(age)} · ${b.rssi} dBm · SNR ${b.snr}</small></div>
    <div class="fleet-metrics">
      ${metric("SOG", value(b.sogKn, 2, " kn"))}
      ${metric("COG", value(b.cogDeg, 1, "°T"))}
      ${metric("HEEL", value(b.heelDeg, 0, "°"))}
      ${metric("PITCH", value(b.pitchDeg, 0, "°"))}
      ${metric("BAT", `${b.batteryPct}%`)}
      ${metric("REC", b.recording ? "기록 중" : "꺼짐")}
    </div>
    ${!b.timeValid
      ? `<div class="fleet-warn">보드·LoRa 응답 확인 · GPS 시각 대기 — 위치·속도·침로는 보내지 않습니다.</div>`
      : !b.gpsFix ? `<div class="fleet-warn">GPS 위치 없음 — 마지막 위치를 새 위치처럼 쓰지 않습니다.</div>` : ""}
  </section>`;
}

function renderDetail() {
  const box = $("fleetDetailBody");
  const picked = selected.map((n) => boats.get(n)).filter((b): b is FleetBoat => !!b);
  if (!picked.length) {
    box.innerHTML = `<div class="fleet-empty large"><b>전체 함대</b><span>목록이나 지도에서 한 척을 누르면 상세, 두 척을 누르면 비교합니다.</span></div>`;
    return;
  }
  if (picked.length === 1) { box.innerHTML = boatCard(picked[0], null); return; }
  const [a, b] = picked;
  const pos = between(a, b);
  const same = a.frame !== 0 && a.frame === b.frame;
  const cogDelta = a.cogDeg !== null && b.cogDeg !== null ? shortestDeg(a.cogDeg, b.cogDeg) : null;
  const sogDelta = a.sogKn !== null && b.sogKn !== null ? b.sogKn - a.sogKn : null;
  const rate = pos?.rateKn;
  box.innerHTML = `${boatCard(a, "A")}${boatCard(b, "B")}
    <section class="fleet-compare">
      <div class="fleet-compare-head"><b>A ↔ B 비교</b><span class="${same ? "good" : "bad"}">${same ? "같은 GPS 초" : "수신 초가 다름"}</span></div>
      <div class="fleet-metrics">
        ${metric("거리", pos ? (pos.meters < 1000 ? `${Math.round(pos.meters)} m` : `${(pos.meters / 1852).toFixed(2)} nm`) : "—")}
        ${metric("A→B", pos ? `${pos.bearing.toFixed(0)}°T` : "—")}
        ${metric("Δ SOG", sogDelta === null ? "—" : `${sogDelta >= 0 ? "+" : ""}${sogDelta.toFixed(2)} kn`, "B − A")}
        ${metric("Δ COG", cogDelta === null ? "—" : `${cogDelta >= 0 ? "+" : ""}${cogDelta.toFixed(1)}°`, "B − A")}
        ${metric("상대 거리", rate === null || rate === undefined ? "—" : `${Math.abs(rate).toFixed(2)} kn`, rate === null || rate === undefined ? "" : rate < 0 ? "가까워짐" : "멀어짐")}
        ${metric("수신", same ? `프레임 ${a.frame}` : `${a.frame} / ${b.frame}`)}
      </div>
      ${same ? "" : `<div class="fleet-warn">두 배가 같은 GPS 초에 보낸 값이 아니므로 차이를 확정값처럼 보지 마세요.</div>`}
    </section>`;
}

function render() {
  const now = Date.now();
  renderList(now);
  renderDetail();
  const points = markerPoints(now);
  if (document.body.dataset.mode === "fleet" || map) {
    mapUp().setFleet(points);
    if (firstPosition && points.length) { firstPosition = false; map?.fitFleet(); }
  }
  $("fleetLinkName").textContent = linkedName || "수신 보드 연결 안 됨";
  $("fleetLinkName").closest(".fleet-link-row")?.classList.toggle("connected", !!link);
  $("fleetLinkName").closest(".fleet-board-block")?.classList.toggle("connected", !!link);
  $("fleetDisconnect").toggleAttribute("hidden", !link);
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
      boats.set(b.boat, b);
      render();
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
    boats.clear(); selected = []; firstPosition = true;
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
  // 화면을 바꾸는 것만으로 파일 전송용 보드 AP를 끄면 안 된다.
  // 함대 수신 보드는 BLE, 기록 보드는 WiFi라 두 연결을 함께 유지할 수 있다.
  if (mode === "review" && before === "fleet") void disconnectFleet(true, false);
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
    boats.set(boat, { boat, lat, lon, sogKn, cogDeg, heelDeg, pitchDeg, batteryPct,
      gpsFix: true, recording: true, timeValid: true, changed: false, heard: 0, tie: boat,
      frame: 1042, rssi: -62 - boat, snr: 9, receivedAt: now });
  }
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
