// 경기정 모듈 바이너리 로그(.HLG) 읽기 — 포맷 v1.0
//
// 규격      ../../docs/spec/로그포맷_v1.0_draft_2026-08-24.md
// 펌웨어    ../../firmware-rak/include/hlog.h
// 파이썬    ../../tools/hlog_parse.py   ← 같은 것을 본다. 고칠 때 같이 고칠 것
//
// 전부 리틀엔디언. 정렬이 안 맞아서(local_ms 가 오프셋 1) DataView 로 읽는다.

export const HEADER_SIZE = 128;
export const TYPE_NAV = 0xa1;
export const NAV_SIZE = 38;
export const TYPE_IMU = 0xb1;
// v1.0 은 27바이트(쿼터니언 8칸 포함), v1.1 부터 19바이트.
// 자세는 가속·자이로 원본에서 후처리로 뽑는다 — 원본이 남아 있으면 계산법을
// 나중에 고쳐도 예전 데이터까지 다시 계산된다.
export const IMU_SIZE_V0 = 27;
export const IMU_SIZE_V1 = 19;

// 값 없음 표식. 0 을 쓰면 "정박 중 0노트" 와 "위성 못 잡음" 이 구별되지 않는다.
const LATLON_INVALID = -0x80000000;
const U16_INVALID = 0xffff;
const U32_INVALID = 0xffffffff;

const KNOTS_PER_MPS = 1.943844;

/** CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) */
export function crc16(data: Uint8Array, from: number, len: number): number {
  let crc = 0xffff;
  for (let i = from; i < from + len; i++) {
    crc ^= data[i] << 8;
    for (let b = 0; b < 8; b++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

export interface Header {
  verMajor: number;
  verMinor: number;
  module: string;          // "3C:DC:75:70:2F:B4"
  fwVersion: string;
  hwRev: number;
  gnssType: number;
  session: number;
  bootCount: number;
  utcStart: number;        // UNIX 초. 0 이면 위성을 못 잡은 세션
  utcStartMs: number;
  mountQuat: [number, number, number, number];
  navHz: number;
  imuHz: number;
  // reserved 에 우리가 채운 것 (hlog.h 의 표)
  imuType: number;         // 0 BNO085 / 1 MPU-9250
  timeRef: number;         // 0 GPS / 1 UTC 환산
  magScale: number;        // 0 raw LSB / 1 0.1 µT/LSB
  gnssDyn: number;
  gnssHz: number;
  sogSrc: number;          // 0 도플러 원본 ← 항상 0 이어야 한다
  quatSrc: number;         // 0 융합 / 1 없음
  durationS: number;       // 세션을 닫으면서 채운다. 0 이면 못 닫힌 파일
  navRows: number;
  imuRows: number;
  dropped: number;
  closed: boolean;
  // 힐·피치를 어느 가속도 축에서 봤나. 이게 없으면 이 파일로 힐을 못 구한다.
  heelAxis: number;    // 0=X 1=Y 2=Z
  heelSign: number;    // 0=+ 1=-
  pitchAxis: number;
  pitchSign: number;
  heelOff: number;     // 기준각 (도)
  pitchOff: number;
  crcOk: boolean;
}

export interface NavRecord {
  ms: number;
  itow: number | null;
  week: number | null;
  lat: number | null;
  lon: number | null;
  sogKn: number | null;    // 다듬기 전 도플러 원본
  cogDeg: number | null;
  numSv: number;
  fix: number;
  hAccM: number | null;
  battMv: number;
  event: number;
  mag: [number, number, number];   // µT
}

/**
 * 못 닫힌 파일의 머리글을 **줄에서 되찾는다.**
 *
 * 세션 길이·줄 수·첫 fix 시각은 `stop()` 이 세션을 닫으면서 채운다. 전원이
 * 갑자기 끊기면 그 자리까지 못 가서 전부 0 으로 남는다. 그러면 보관함 목록에
 * "NAV 0줄 · 시각 없음" 으로 뜨고 빈 세션처럼 보인다.
 *
 * 그런데 **값은 파일 안에 다 있다.** 2026-08-30 세션 27 이 그랬다 —
 * 머리글은 0 인데 안에는 NAV 17,441줄이 fix 까지 붙어 들어 있었다.
 *
 * 그래서 비어 있는 칸만 줄에서 채운다. 채워져 있는 칸은 안 건드린다.
 * `closed` 는 그대로 false 로 둔다 — 정말로 못 닫힌 파일이고, 그건 사실이다.
 *
 * 시각은 보드의 `rec check` 와 같은 규칙이다. **fix 가 선 줄만 믿는다** —
 * 위성을 못 잡아도 수신기가 시각 칸을 채워 보내는 때가 있고, 그 값을 쓰면
 * 1999년 같은 지어낸 날짜가 나온다.
 */
export function recoverHeader(s: Session): boolean {
  const h = s.header;
  let did = false;

  if (!h.navRows && s.nav.length) { h.navRows = s.nav.length; did = true; }
  if (!h.imuRows && s.imu.length) { h.imuRows = s.imu.length; did = true; }

  if (!h.durationS) {
    const first = s.imu[0]?.ms ?? s.nav[0]?.ms;
    const last = s.imu[s.imu.length - 1]?.ms ?? s.nav[s.nav.length - 1]?.ms;
    if (first !== undefined && last !== undefined && last > first) {
      h.durationS = Math.round((last - first) / 1000);
      did = true;
    }
  }

  if (!h.utcStart) {
    for (const r of s.nav) {
      if (!r.fix || r.week === null || r.itow === null) continue;
      // GPS 원점 1980-01-06 = UNIX 315964800. 윤초는 안 뺀다 (time_ref=1 과 같은 규칙).
      h.utcStart = 315964800 + r.week * 604800 + Math.floor(r.itow / 1000);
      h.utcStartMs = r.itow % 1000;
      did = true;
      break;
    }
  }
  return did;
}

export interface ImuRecord {
  ms: number;
  acc: [number, number, number];   // g
  gyr: [number, number, number];   // °/s
}

export interface Session {
  header: Header;
  nav: NavRecord[];
  imu: ImuRecord[];
  /** 한 바이트씩 밀며 다시 맞춘 횟수. 0 이어야 정상 */
  resyncs: number;
  /** 어느 레코드에도 못 들어간 바이트 수 */
  lostBytes: number;
}

export function parseHeader(buf: Uint8Array): Header {
  if (buf.length < HEADER_SIZE) throw new Error("파일이 머리글보다 짧습니다");
  const d = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  if (buf[0] !== 0x48 || buf[1] !== 0x48 || buf[2] !== 0x4c || buf[3] !== 0x47) {
    throw new Error("HHLG 로 시작하지 않습니다 — 우리 파일이 아닙니다");
  }
  const hex = (n: number) => buf[n].toString(16).toUpperCase().padStart(2, "0");
  return {
    verMajor: buf[4],
    verMinor: buf[5],
    module: [8, 9, 10, 11, 12, 13].map(hex).join(":"),
    fwVersion: `${buf[15].toString(16)}.${buf[14].toString(16)}`,
    hwRev: buf[16],
    gnssType: buf[17],
    session: d.getUint32(18, true),
    bootCount: d.getUint16(22, true),
    utcStart: d.getUint32(24, true),
    utcStartMs: d.getUint16(28, true),
    mountQuat: [
      d.getInt16(30, true), d.getInt16(32, true),
      d.getInt16(34, true), d.getInt16(36, true),
    ],
    navHz: buf[39],
    imuHz: buf[40],
    imuType: buf[41],
    timeRef: buf[42],
    magScale: buf[43],
    gnssDyn: buf[44],
    gnssHz: buf[45],
    sogSrc: buf[46],
    quatSrc: buf[47],
    durationS: d.getUint32(48, true),
    navRows: d.getUint32(52, true),
    imuRows: d.getUint32(56, true),
    dropped: d.getUint32(60, true),
    closed: buf[64] === 1,
    heelAxis: buf[65],
    heelSign: buf[66],
    pitchAxis: buf[67],
    pitchSign: buf[68],
    heelOff: d.getFloat32(69, true),
    pitchOff: d.getFloat32(73, true),
    crcOk: d.getUint16(126, true) === crc16(buf, 0, 126),
  };
}

function readNav(d: DataView, o: number): NavRecord {
  const lat = d.getInt32(o + 11, true);
  const lon = d.getInt32(o + 15, true);
  const sog = d.getUint16(o + 19, true);
  const cog = d.getUint16(o + 21, true);
  const hAcc = d.getUint16(o + 25, true);
  const itow = d.getUint32(o + 5, true);
  const week = d.getUint16(o + 9, true);
  return {
    ms: d.getUint32(o + 1, true),
    itow: itow === U32_INVALID ? null : itow,
    week: week === U16_INVALID ? null : week,
    lat: lat === LATLON_INVALID ? null : lat / 1e7,
    lon: lon === LATLON_INVALID ? null : lon / 1e7,
    sogKn: sog === U16_INVALID ? null : (sog / 1000) * KNOTS_PER_MPS,
    cogDeg: cog === U16_INVALID ? null : cog / 100,
    numSv: d.getUint8(o + 23),
    fix: d.getUint8(o + 24),
    hAccM: hAcc === U16_INVALID ? null : hAcc / 100,
    battMv: d.getUint16(o + 27, true),
    event: d.getUint8(o + 29),
    mag: [
      d.getInt16(o + 30, true) / 10,
      d.getInt16(o + 32, true) / 10,
      d.getInt16(o + 34, true) / 10,
    ],
  };
}

function readImu(d: DataView, o: number): ImuRecord {
  return {
    ms: d.getUint32(o + 1, true),
    acc: [
      d.getInt16(o + 5, true) / 1000,
      d.getInt16(o + 7, true) / 1000,
      d.getInt16(o + 9, true) / 1000,
    ],
    gyr: [
      d.getInt16(o + 11, true) / 32,
      d.getInt16(o + 13, true) / 32,
      d.getInt16(o + 15, true) / 32,
    ],
  };
}

/**
 * 파일 전체를 읽는다.
 *
 * 깨진 자리를 만나면 한 바이트씩 밀면서 다시 맞춘다 (규격 §2). type 바이트만
 * 으로는 우연히 맞을 수 있어서 **CRC 와 "시각이 뒤로 가지 않는다" 두 조건**을
 * 같이 본다.
 */
export function parse(buf: Uint8Array): Session {
  const header = parseHeader(buf);
  const d = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);

  // 옛 파일은 IMU 레코드가 27바이트다. 머리글의 판 번호를 보고 고른다.
  const imuSize =
    header.verMajor > 1 || (header.verMajor === 1 && header.verMinor >= 1)
      ? IMU_SIZE_V1 : IMU_SIZE_V0;

  const nav: NavRecord[] = [];
  const imu: ImuRecord[] = [];
  let resyncs = 0;
  let lost = 0;
  let lastMs = -1;

  let i = HEADER_SIZE;
  const n = buf.length;
  while (i < n) {
    const t = buf[i];
    const size = t === TYPE_NAV ? NAV_SIZE : t === TYPE_IMU ? imuSize : 0;
    let ok = false;

    if (size && i + size <= n) {
      if (d.getUint16(i + size - 2, true) === crc16(buf, i, size - 2)) {
        const ms = d.getUint32(i + 1, true);
        // 시각이 크게 뒤로 가면 우연히 CRC 가 맞은 가짜다.
        if (lastMs < 0 || ms + 5000 >= lastMs) {
          if (t === TYPE_NAV) nav.push(readNav(d, i));
          else imu.push(readImu(d, i));
          if (ms > lastMs) lastMs = ms;
          i += size;
          ok = true;
        }
      }
    }
    if (!ok) {
      i += 1;
      lost += 1;
      resyncs += 1;
    }
  }

  return { header, nav, imu, resyncs, lostBytes: lost };
}

/** 받은 파일이 멀쩡한지. TRANSFER.md §4 순서 그대로. */
export interface Check {
  ok: boolean;
  problems: string[];
  navHz: number | null;
  imuHz: number | null;
  /** IMU 이웃 간격이 정확히 기대값인 비율 (%) */
  evenPct: number | null;
}

export function check(s: Session): Check {
  const problems: string[] = [];
  if (!s.header.crcOk) problems.push("머리글 CRC 가 틀립니다");
  if (s.lostBytes > 0) problems.push(`못 읽은 바이트 ${s.lostBytes}개`);
  if (s.header.dropped > 0) problems.push(`보드가 ${s.header.dropped}줄 버렸습니다`);
  if (s.header.sogSrc !== 0) problems.push("속도가 원본이 아니라 다듬은 값입니다");
  if (!s.header.closed) problems.push("제대로 닫히지 않은 파일입니다 (전원이 끊겼을 수 있음)");

  const hz = (a: { ms: number }[]) => {
    if (a.length < 2) return null;
    const span = (a[a.length - 1].ms - a[0].ms) / 1000;
    return span > 0 ? (a.length - 1) / span : null;
  };
  const navHz = hz(s.nav);
  const imuHz = hz(s.imu);

  let evenPct: number | null = null;
  if (s.imu.length > 2 && s.header.imuHz > 0) {
    const want = Math.round(1000 / s.header.imuHz);
    let good = 0;
    for (let i = 1; i < s.imu.length; i++) {
      if (s.imu[i].ms - s.imu[i - 1].ms === want) good++;
    }
    evenPct = (good / (s.imu.length - 1)) * 100;
  }

  return { ok: problems.length === 0, problems, navHz, imuHz, evenPct };
}

/** 세션 시작으로부터 지난 밀리초 → UTC 밀리초. 못 구하면 null */
export function toUtcMs(h: Header, localMs: number, firstLocalMs: number): number | null {
  if (!h.utcStart) return null;
  return h.utcStart * 1000 + h.utcStartMs + (localMs - firstLocalMs);
}
