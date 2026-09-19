// LoRa 수신 보드의 BLE 알림을 검증하고, 배 번호 충돌·프레임 누락을 관리한다.
// DOM이나 지도에 기대지 않아 호스트 회귀시험에서 그대로 돌릴 수 있다.

import { FLEET_LENGTH, FLEET_VERSION, LORA_WIRE_VERSION } from "./protocol";

export interface FleetBoat {
  radioVersion: number;
  boat: number;
  lat: number | null;
  lon: number | null;
  sogKn: number | null;
  cogDeg: number | null;
  headingDeg: number | null;
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
  notifySeq: number | null;
  receivedAt: number;
}

const INVALID_POS = -2147483648;
const u16 = (d: DataView, at: number) => d.getUint16(at, true);
const DUPLICATE_WINDOW_MS = 12000;
const DUPLICATE_HOLD_MS = 20000;
export const MAP_KEEP_MS = 20000;

export function freshMs(b: FleetBoat) { return b.timeValid ? 3000 : 12000; }

/** PROTOCOL.md §10.13. v1/v2도 받아 앱과 수신 보드 교체 중 화면이 비지 않게 한다. */
export function decodeFleet(bytes: number[], now = Date.now()): FleetBoat | null {
  if (bytes.length !== 30 && bytes.length !== 32 && bytes.length !== FLEET_LENGTH) return null;
  const a = Uint8Array.from(bytes);
  const d = new DataView(a.buffer);
  const envelopeVersion = a[0];
  if ((envelopeVersion === 1 && bytes.length !== 30) ||
      (envelopeVersion === 2 && bytes.length !== 32) ||
      (envelopeVersion === FLEET_VERSION && bytes.length !== FLEET_LENGTH)) return null;
  if (envelopeVersion !== 1 && envelopeVersion !== 2 && envelopeVersion !== FLEET_VERSION) return null;
  const radioVersion = a[1] >> 6;
  const boat = a[1] & 0x3f;
  // LoRa v2는 끝의 HDG 두 바이트가 필수라 34바이트 BLE 봉투에서만 유효하다.
  if (radioVersion > LORA_WIRE_VERSION || (radioVersion >= 2 && envelopeVersion < 3) || boat < 1 || boat > 32) return null;
  const latRaw = d.getInt32(2, true);
  const lonRaw = d.getInt32(6, true);
  const sog = u16(d, 10);
  const cog = u16(d, 12);
  const heading = envelopeVersion >= 3 ? u16(d, 23) : 0xffff;
  const heel = d.getInt8(14);
  const pitch = d.getInt8(15);
  const flags = a[16];
  const frameAt = envelopeVersion >= 3 ? 25 : 23;
  if (latRaw !== INVALID_POS && (latRaw < -900000000 || latRaw > 900000000)) return null;
  if (lonRaw !== INVALID_POS && (lonRaw < -1800000000 || lonRaw > 1800000000)) return null;
  // 위치는 위·경도가 한 쌍이다. 한쪽만 '없음'인 손상 패킷을 정상 위치로 넘기지 않는다.
  if ((latRaw === INVALID_POS) !== (lonRaw === INVALID_POS)) return null;
  if (cog !== 0xffff && cog > 3599) return null;
  if (heading !== 0xffff && heading > 3599) return null;
  return {
    radioVersion, boat,
    lat: latRaw === INVALID_POS ? null : latRaw / 1e7,
    lon: lonRaw === INVALID_POS ? null : lonRaw / 1e7,
    sogKn: sog === 0xffff ? null : sog / 100,
    cogDeg: cog === 0xffff ? null : cog / 10,
    headingDeg: heading === 0xffff ? null : heading / 10,
    heelDeg: heel === -128 ? null : heel,
    pitchDeg: pitch === -128 ? null : pitch,
    batteryPct: Math.round(((flags >> 4) & 0x0f) * 100 / 15),
    gpsFix: !!(flags & 0x01),
    recording: !!(flags & 0x02),
    timeValid: !!(flags & 0x04),
    changed: !!(flags & 0x08),
    heard: d.getUint32(17, true),
    tie: u16(d, 21),
    frame: d.getUint32(frameAt, true),
    rssi: d.getInt16(frameAt + 4, true),
    snr: d.getInt8(frameAt + 6),
    notifySeq: envelopeVersion >= 2 ? u16(d, frameAt + 7) : null,
    receivedAt: now,
  };
}

interface Reception { received: number; missed: number; lastFrame: number | null }

/** v2 BLE 알림의 전역 순번으로 보드→앱 사이에서 빠진 알림을 센다. */
export class FleetTransportCounter {
  received = 0;
  missed = 0;
  legacySeen = false;
  private last: number | null = null;

  reset() { this.received = 0; this.missed = 0; this.legacySeen = false; this.last = null; }

  ingest(seq: number | null) {
    if (seq === null) { this.legacySeen = true; return; }
    if (this.last !== null) {
      const step = (seq - this.last + 65536) & 0xffff;
      if (step > 0 && step <= 1000) this.missed += step - 1;
    }
    this.last = seq;
    this.received++;
  }

  get ready() { return this.last !== null; }
  get percent() {
    const total = this.received + this.missed;
    return total ? this.received * 100 / total : 100;
  }
}

export class FleetTracker {
  readonly boats = new Map<number, FleetBoat>();
  private readonly conflicts = new Map<number, number>();
  private readonly receptionByRadio = new Map<string, Reception>();

  reset() {
    this.boats.clear();
    this.conflicts.clear();
    this.receptionByRadio.clear();
  }

  ingest(b: FleetBoat) {
    const prev = this.boats.get(b.boat);
    if (prev && prev.tie !== b.tie && b.receivedAt - prev.receivedAt <= DUPLICATE_WINDOW_MS) {
      this.conflicts.set(b.boat, b.receivedAt + DUPLICATE_HOLD_MS);
    }
    this.boats.set(b.boat, b);
    if (!b.timeValid || b.frame === 0) return;

    const key = `${b.boat}:${b.tie}`;
    const rx = this.receptionByRadio.get(key) ?? { received: 0, missed: 0, lastFrame: null };
    if (rx.lastFrame === null) {
      rx.received++;
    } else {
      const step = (b.frame - rx.lastFrame) >>> 0;
      if (step > 0 && step <= 120) {
        rx.received++;
        rx.missed += step - 1;
      } else if (step > 120) {
        rx.received++;
        rx.missed = 0;
      }
    }
    rx.lastFrame = b.frame;
    this.receptionByRadio.set(key, rx);
  }

  conflicted(boat: number, now = Date.now()) {
    const until = this.conflicts.get(boat) ?? 0;
    if (until <= now) { this.conflicts.delete(boat); return false; }
    return true;
  }

  reception(b: FleetBoat) {
    const rx = this.receptionByRadio.get(`${b.boat}:${b.tie}`);
    if (!rx) return null;
    const total = rx.received + rx.missed;
    return { ...rx, percent: total ? rx.received * 100 / total : 100 };
  }

  heardByOthers(boat: number, now = Date.now()) {
    const bit = 1 << (boat - 1);
    return [...this.boats.values()].some((b) => b.boat !== boat &&
      now - b.receivedAt <= freshMs(b) && (b.heard & bit) !== 0);
  }

  listedBoats(now = Date.now()) {
    const ids = new Set(this.boats.keys());
    for (const b of this.boats.values()) {
      if (now - b.receivedAt > freshMs(b)) continue;
      for (let i = 0; i < 32; i++) if ((b.heard & (1 << i)) !== 0) ids.add(i + 1);
    }
    return [...ids].sort((a, b) => a - b);
  }
}
