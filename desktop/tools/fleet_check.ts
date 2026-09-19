import { decodeFleet, FleetTracker, FleetTransportCounter, type FleetBoat } from "../src/fleet_model";

let failures = 0;
function check(ok: unknown, name: string) {
  if (ok) console.log(`  [ OK ] ${name}`);
  else { console.error(`  [FAIL] ${name}`); failures++; }
}

function put16(a: number[], at: number, v: number) { a[at] = v & 255; a[at + 1] = (v >>> 8) & 255; }
function put32(a: number[], at: number, v: number) {
  a[at] = v & 255; a[at + 1] = (v >>> 8) & 255; a[at + 2] = (v >>> 16) & 255; a[at + 3] = (v >>> 24) & 255;
}

function packet(version = 3, seq = 10) {
  const a = Array(version === 3 ? 34 : version === 2 ? 32 : 30).fill(0);
  a[0] = version; a[1] = (version === 3 ? 0x80 : 0x40) | 7;
  put32(a, 2, 375512345); put32(a, 6, 1269887654);
  put16(a, 10, 123); put16(a, 12, 3599);
  a[14] = 0xf4; a[15] = 9; a[16] = 0xcf;
  put32(a, 17, 0x80000005); put16(a, 21, 0xa3f2);
  const frameAt = version === 3 ? 25 : 23;
  if (version === 3) put16(a, 23, 1654);
  put32(a, frameAt, 42); put16(a, frameAt + 4, 0xffc4); a[frameAt + 6] = 9;
  if (version >= 2) put16(a, frameAt + 7, seq);
  return a;
}

const v3 = decodeFleet(packet(), 1000)!;
check(!!v3 && v3.radioVersion === 2 && v3.boat === 7 && v3.lat === 37.5512345 && v3.lon === 126.9887654,
      "v3 위치와 무선 버전·배 번호 디코드");
check(v3.sogKn === 1.23 && v3.cogDeg === 359.9 && v3.headingDeg === 165.4 && v3.heelDeg === -12 && v3.pitchDeg === 9,
      "v3 항해값·HDG M·자세 부호 디코드");
check(v3.notifySeq === 10 && v3.frame === 42 && v3.rssi === -60 && v3.snr === 9,
      "v3 BLE 순번·프레임·신호 품질 디코드");
check(decodeFleet(packet(2), 1000)?.headingDeg === null, "v2 32바이트도 읽되 HDG는 없음");
check(decodeFleet(packet(1), 1000)?.notifySeq === null, "v1 30바이트도 읽되 BLE 순번은 없음");
check(decodeFleet(packet(3).slice(0, 32)) === null, "v3를 32바이트로 잘라 보내면 거절");
const badBoat = packet(); badBoat[1] = 0;
check(decodeFleet(badBoat) === null, "송신 배 번호 0은 거절");
const legacyRadio = packet(); legacyRadio[1] = 7;
check(decodeFleet(legacyRadio)?.radioVersion === 0, "교체 기간에는 무버전 LoRa 패킷도 읽음");
const futureRadio = packet(); futureRadio[1] = 0xc0 | 7;
check(decodeFleet(futureRadio) === null, "모르는 미래 LoRa 버전은 거절");
const halfPosition = packet(); put32(halfPosition, 2, 0x80000000);
check(decodeFleet(halfPosition) === null, "위·경도 중 한쪽만 없는 손상 패킷은 거절");

function boat(at: number, frame: number, tie: number, overrides: Partial<FleetBoat> = {}): FleetBoat {
  return { ...v3, receivedAt: at, frame, tie, notifySeq: frame & 0xffff, ...overrides };
}

const tracker = new FleetTracker();
tracker.ingest(boat(1000, 10, 0x1111));
tracker.ingest(boat(2000, 12, 0x1111));
const rx = tracker.reception(tracker.boats.get(7)!)!;
check(rx.received === 2 && rx.missed === 1 && Math.abs(rx.percent - 66.6667) < 0.01,
      "배별 PPS 프레임 하나 누락 계산");
tracker.ingest(boat(2500, 12, 0x2222));
check(tracker.conflicted(7, 2500), "같은 배 번호의 다른 MAC tie를 충돌로 표시");
check(!tracker.conflicted(7, 22501), "마지막 충돌 뒤 20초가 지나면 충돌 해제");

const heard = new FleetTracker();
heard.ingest(boat(1000, 1, 0x1111, { boat: 3, heard: (1 << 11) >>> 0 }));
check(heard.listedBoats(1000).includes(12) && heard.heardByOthers(12, 1000),
      "본부가 직접 못 들은 배도 다른 배의 heard 비트로 확인");

const transport = new FleetTransportCounter();
transport.ingest(65535); transport.ingest(1);
check(transport.received === 2 && transport.missed === 1 && Math.abs(transport.percent - 66.6667) < 0.01,
      "BLE 순번 wrap을 지나도 알림 누락 계산");
const legacy = new FleetTransportCounter(); legacy.ingest(null);
check(legacy.legacySeen && !legacy.ready, "v1 수신 보드는 BLE 누락 측정 불가로 구분");

if (failures) process.exit(1);
console.log("\n함대 통신 회귀시험 통과");
