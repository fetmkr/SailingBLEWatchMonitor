/**
 * 세션 내보내기 — zip 한 묶음 (2026-09-15, 사용자 결정: 원본 · CSV · GPX · 코치 메모 전부).
 *
 *   <이름>.HLG            보드가 쓴 원본 그대로 (SHA-256 을 notes.json 에 적는다)
 *   <이름>.TXT            붙어 있으면 그대로
 *   <이름>_nav.csv        NAV 한 줄에 한 줄 (10 Hz)
 *   <이름>_imu.csv        IMU 한 줄에 한 줄 (100 Hz)
 *   <이름>.gpx            위치가 있는 줄만. 첫 fix UTC 가 없으면 시각 없이
 *   <이름>_notes.json     코치가 적은 것 · 마킹 · 방위를 무엇으로 그렸나 · 해시
 *   README.txt            열마다 무엇인지, 무엇으로 계산했는지
 *
 * ★ 분석 앱 원칙 — 값이 없으면 빈칸이다. 0 을 넣지 않는다. 계산한 열은 이름과 README 에 출처를 적는다.
 * ★ 화면에 그린 값(series)을 그대로 쓴다. 내보내기용으로 식을 따로 두면 화면과 파일이 어긋난다.
 */

import { zip, strToU8 } from "fflate";
import type * as hlog from "./hlog";
import type * as tl from "./timeline";
import type * as lib from "./library";
import * as btxt from "./boardtxt";

export interface ExportInput {
  base: string;                       // 파일 이름 앞부분
  session: hlog.Session;
  series: tl.Series[];
  entry: lib.Entry;
  marks: { ms: number; note: string; from: string }[];   // 화면의 마킹 (세션 시작부터 ms)
  hlgBytes: Uint8Array;
  txtText: string | null;
  openTxt: btxt.BoardTxt | null;
  hdgNote: string;                    // heading.describe 결과
  hdgFitText: string;                 // 보드 기록 HDG · 추정 설정 설명 (글자만)
  utcMs: (localMs: number) => number | null;   // 앱이 화면에 쓰는 것과 같은 환산
}

/** 줄에 적힌 GPS 시각 (원본). fix 없거나 주차·주중시각이 없으면 null. main.cpp logWriteNav 와 같은 식, 윤초 안 뺌 (time_ref=1) */
export function rowGpsUtcMs(r: hlog.NavRecord): number | null {
  if (!r.fix || r.week === null || r.itow === null) return null;
  return (315964800 + r.week * 604800) * 1000 + r.itow;
}

/**
 * 이 기록의 UTC (계산값) = local_ms + 기준. 기준은 **10분 구간마다, GPS 시각이 막 바뀐 줄들의 (UTC − local_ms) 최댓값.**
 *
 * 왜 줄에 적힌 시각을 그대로 안 쓰나 — 보드는 줄을 쓸 때 TinyGPS 가 마지막으로 읽은 NMEA 시각을 넣는다
 * (main.cpp gpsWeekTow). 세션 46 (104,757줄) 에서 이웃 줄과 같은 시각이 76,863번, 바뀔 때는 300~500 ms 씩 뛰었다.
 * 옛 값을 들고 있는 만큼 늦으므로, 막 바뀐 줄 중 가장 늦지 않은 값(최댓값)이 참에 가장 가깝다.
 * 10분마다 따로 잡는 이유 — 같은 세션에서 그 값이 80분 뒤 100 ms 한 번 옮겨 갔다 (보드 시계가 3시간에 약 100 ms 느림).
 * ★ 막 바뀐 줄도 NMEA 가 나와서 읽히기까지의 지연만큼은 늦다. 그 양은 모른다 (PPS 로 재야 안다).
 */
export function utcOfSession(s: hlog.Session): (localMs: number) => number | null {
  const W = 600000;
  const fresh: [number, number][] = [];               // [local_ms, UTC − local_ms] 막 바뀐 줄만
  let prev: number | null = null;
  for (const r of s.nav) {
    const u = rowGpsUtcMs(r);
    if (u === null) continue;
    if (prev !== null && u !== prev) fresh.push([r.ms, u - r.ms]);
    prev = u;
  }
  if (!fresh.length) {
    const fx = s.nav.find((r) => rowGpsUtcMs(r) !== null);
    if (!fx) return () => null;
    const base = rowGpsUtcMs(fx)! - fx.ms;               // 바뀐 줄이 없으면 첫 fix 줄 하나로
    return (localMs) => base + localMs;
  }
  const t0 = fresh[0][0];
  const win = new Map<number, number>();              // 구간 번호 → 최댓값
  for (const [m, v] of fresh) {
    const k = Math.floor((m - t0) / W);
    win.set(k, Math.max(win.get(k) ?? -Infinity, v));
  }
  // ★ 구간마다 계단으로 바꾸면 경계에서 시각이 뒤로 간다 (세션 46: 80분 경계에서 100 ms 뒤로 한 번).
  //   구간 가운데 점들을 직선으로 이어 기준이 조금씩 옮겨 가게 한다. 양 끝 밖은 끝 값 그대로.
  const pts = [...win.entries()].sort((a, b) => a[0] - b[0]).map(([k, v]) => [t0 + (k + 0.5) * W, v] as const);
  return (localMs) => {
    if (localMs <= pts[0][0]) return localMs + pts[0][1];
    const last = pts[pts.length - 1];
    if (localMs >= last[0]) return localMs + last[1];
    let i = 1;
    while (pts[i][0] < localMs) i++;
    const [xa, ya] = pts[i - 1], [xb, yb] = pts[i];
    return localMs + ya + (yb - ya) * (localMs - xa) / (xb - xa);
  };
}

const num = (v: number | null | undefined, d: number) =>
  v === null || v === undefined || !Number.isFinite(v) ? "" : v.toFixed(d);
const iso = (ms: number | null) => (ms === null ? "" : new Date(ms).toISOString());
const csvCell = (s: string) => (/[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s);

function seriesOf(series: tl.Series[], code: string) {
  return series.find((s) => s.code === code);
}

export async function buildZip(x: ExportInput, onStep?: (what: string) => void): Promise<Uint8Array> {
  const s = x.session;
  const t0 = s.imu.length ? s.imu[0].ms : s.nav.length ? s.nav[0].ms : 0;   // main.ts buildSeries 와 같은 0점

  // ── 방위 열: 화면의 HDG 줄이 무엇인지 이름으로 가른다 ──
  const hdgRow = seriesOf(x.series, "HDG");
  const hdgIsBoard = !!hdgRow && hdgRow.name.includes("보드 기록 HDG");
  const hdgRecomp = hdgRow ? (hdgIsBoard ? hdgRow.alt?.ys ?? null : hdgRow.ys) : null;
  const hdgRecompName = hdgRow ? (hdgIsBoard ? hdgRow.alt?.name ?? "" : hdgRow.name) : "";
  const sog = seriesOf(x.series, "SOG"), cog = seriesOf(x.series, "COG");
  const heel = seriesOf(x.series, "HEEL"), trim = seriesOf(x.series, "TRIM");

  // TXT 보드 방위 → NAV 줄 번호 (화면과 같은 붙이기)
  const txtAt = new Map<number, number>();
  if (x.openTxt) {
    const p = btxt.placeRows(x.openTxt, s, t0);
    p.nav.forEach((i, k) => txtAt.set(i, p.ys[k]));
  }

  onStep?.("NAV 표");
  const nav: string[] = [
    "local_ms,session_ms,utc,gps_utc_row,lat_deg,lon_deg,sog_kn_doppler,cog_deg,sog_kn_pos5s,cog_deg_pos5s," +
    "num_sv,fix,hacc_m,batt_v,event,mag_x_uT,mag_y_uT,mag_z_uT," +
    "hdg_board_hlg_deg,hdg_board_txt_deg,hdg_recomputed_deg",
  ];
  for (let i = 0; i < s.nav.length; i++) {
    const r = s.nav[i];
    nav.push([
      r.ms, r.ms - t0, iso(x.utcMs(r.ms)), iso(rowGpsUtcMs(r)),
      num(r.lat, 7), num(r.lon, 7), num(r.sogKn, 3), num(r.cogDeg, 2),
      num(sog?.alt?.ys[i], 3), num(cog?.alt?.ys[i], 2),
      r.numSv, r.fix, num(r.hAccM, 2), r.battMv ? (r.battMv / 1000).toFixed(3) : "", r.event,
      num(r.mag[0], 1), num(r.mag[1], 1), num(r.mag[2], 1),
      num(r.boardHdgDeg, 2), num(txtAt.get(i), 0), num(hdgRecomp?.[i], 2),
    ].join(","));
  }

  onStep?.("IMU 표");
  const imu: string[] = ["local_ms,session_ms,acc_x_g,acc_y_g,acc_z_g,gyr_x_dps,gyr_y_dps,gyr_z_dps,heel_deg,trim_deg"];
  for (let i = 0; i < s.imu.length; i++) {
    const r = s.imu[i];
    imu.push([
      r.ms, r.ms - t0,
      num(r.acc[0], 3), num(r.acc[1], 3), num(r.acc[2], 3),
      num(r.gyr[0], 3), num(r.gyr[1], 3), num(r.gyr[2], 3),
      num(heel?.ys[i], 2), num(trim?.ys[i], 2),
    ].join(","));
  }

  onStep?.("GPX");
  const hasUtc = !!s.header.utcStart;
  const trkpts: string[] = [];
  for (const r of s.nav) {
    if (r.lat === null || r.lon === null) continue;
    const u = x.utcMs(r.ms);
    trkpts.push(`      <trkpt lat="${r.lat.toFixed(7)}" lon="${r.lon.toFixed(7)}">` +
      (u !== null ? `<time>${iso(u)}</time>` : "") +
      (r.sogKn !== null ? `<extensions><speed>${(r.sogKn / 1.943844).toFixed(3)}</speed></extensions>` : "") +
      `</trkpt>`);
  }
  const title = x.entry.title || x.entry.sailor || `세션 ${x.entry.session}`;
  const gpx = `<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="Sail Analyzer" xmlns="http://www.topografix.com/GPX/1/1">
  <metadata><name>${escXml(title)}</name>${hasUtc ? `<time>${iso(s.header.utcStart * 1000)}</time>` : ""}</metadata>
  <trk><name>${escXml(title)}</name>
    <trkseg>
${trkpts.join("\n")}
    </trkseg>
  </trk>
</gpx>
`;

  onStep?.("해시");
  const hash = await sha256Hex(x.hlgBytes);
  const e = x.entry;
  const notes = {
    exportedAt: new Date().toISOString(),
    app: "Sail Analyzer",
    hlg: { file: `${x.base}.HLG`, bytes: x.hlgBytes.length, sha256: hash, format: `${s.header.verMajor}.${s.header.verMinor}` },
    txtAttached: x.txtText !== null,
    session: { module: e.module, session: e.session, utcStart: e.utcStart || null, durationS: e.durationS },
    coach: {
      title: e.title, sailor: e.sailor, boatClass: e.boatClass, venue: e.venue, group: e.group,
      windKn: e.windKn, windDir: e.windDir, waves: e.waves, rig: e.rig, notes: e.notes, starred: e.starred,
    },
    marks: x.marks.map((m) => ({ sessionMs: m.ms, utc: iso(x.utcMs(m.ms + t0)) || null, note: m.note, from: m.from })),
    marksHiddenFromFile: e.markHidden ?? [],
    heading: { formula: x.hdgNote, board: x.hdgFitText, recomputedColumn: hdgRecompName },
  };

  const readme = `Sail Analyzer 세션 내보내기 — ${title}
만든 때 ${notes.exportedAt}

■ 파일
  ${x.base}.HLG        보드가 SD 카드에 쓴 원본 그대로. SHA-256 ${hash}
  ${x.base}.TXT        ${x.txtText !== null ? "보드가 10초마다 쓴 사본 그대로" : "(이 세션에 붙인 TXT 없음)"}
  ${x.base}_nav.csv    NAV 줄마다 한 줄 (${s.nav.length.toLocaleString()}줄)
  ${x.base}_imu.csv    IMU 줄마다 한 줄 (${s.imu.length.toLocaleString()}줄)
  ${x.base}.gpx        위치가 있는 줄만 (${trkpts.length.toLocaleString()}점)${hasUtc ? "" : " · 첫 fix UTC 가 없어 시각 없음"}
  ${x.base}_notes.json 코치가 적은 것 · 마킹 · 방위 설명 · 해시

■ 빈칸은 값이 없다는 뜻이다. 0 으로 채우지 않았다.

■ nav.csv 열
  local_ms            보드 켠 뒤 ms (HLG 원본)
  session_ms          앱 화면의 0점부터 ms (첫 IMU 줄, 없으면 첫 NAV 줄)
  utc                 계산값 — local_ms + 기준. 기준은 10분 구간마다 "GPS 시각이 막 바뀐 줄" 의 (UTC − local_ms) 최댓값.
                      줄마다 고르게 늘어난다. NMEA 가 읽히기까지의 지연만큼은 늦을 수 있다(양은 모름). 위성을 못 잡은 세션은 빈칸
  gps_utc_row         원본 — 그 줄에 보드가 적은 GPS 시각. 마지막으로 읽은 NMEA 시각이라 이웃 줄과 같거나 최대 약 0.5초 늦다
                      (세션 46: 이웃 줄과 같은 값 73%, 바뀔 때 300~500 ms 씩 뜀). fix 없는 줄은 빈칸
  lat_deg lon_deg     원본. 위성 못 잡은 줄은 빈칸
  sog_kn_doppler      원본 도플러 속도 (다듬지 않음)
  cog_deg             원본 침로 (저속에서 수신기가 옛 값을 들고 있을 수 있음)
  sog_kn_pos5s        계산값 — 5초 창 위치 차분 속도 (앱 SOG cal 과 같음)
  cog_deg_pos5s       계산값 — 5초 창 위치 차분 방향, 움직인 거리 3 m 넘을 때만 (앱 COG cal)
  num_sv fix hacc_m batt_v event   원본
  mag_*_uT            HLG 에 적힌 자력 (식 5는 센서 원본, 이전 식은 당시 보드가 기록한 값)
  hdg_board_hlg_deg   보드가 그 줄을 만들 때 화면·BLE·TXT 에 보여준 방위 (HLG 1.2 부터, 옛 파일은 빈칸)
  hdg_board_txt_deg   붙인 TXT 의 보드 방위를 TXT 끝 NAV 줄 번호로 붙인 것 (10초마다, 정수)
  hdg_recomputed_deg  계산값 — ${hdgRecompName || "없음"}

■ imu.csv 열
  acc_*_g gyr_*_dps   원본 (가속도계 축)
  heel_deg trim_deg   계산값 — 머리글의 힐·피치 축·부호·기준각으로 asin (앱 HEEL · TRIM 과 같음)

■ 방위
  ${x.hdgNote}
  ${x.hdgFitText.replace(/\n/g, "\n  ")}
`;

  onStep?.("압축");
  const files: Record<string, Uint8Array> = {
    [`${x.base}/${x.base}.HLG`]: x.hlgBytes,
    [`${x.base}/${x.base}_nav.csv`]: strToU8(nav.join("\n") + "\n"),
    [`${x.base}/${x.base}_imu.csv`]: strToU8(imu.join("\n") + "\n"),
    [`${x.base}/${x.base}.gpx`]: strToU8(gpx),
    [`${x.base}/${x.base}_notes.json`]: strToU8(JSON.stringify(notes, null, 2)),
    [`${x.base}/README.txt`]: strToU8(readme),
  };
  if (x.txtText !== null) files[`${x.base}/${x.base}.TXT`] = strToU8(x.txtText);

  return await new Promise<Uint8Array>((resolve, reject) => {
    zip(files, { level: 6 }, (err, data) => (err ? reject(err) : resolve(data)));
  });
}

function escXml(v: string) {
  return v.replace(/[<>&"]/g, (c) => ({ "<": "&lt;", ">": "&gt;", "&": "&amp;", '"': "&quot;" }[c]!));
}

async function sha256Hex(b: Uint8Array): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", b.slice().buffer);
  return [...new Uint8Array(d)].map((v) => v.toString(16).padStart(2, "0")).join("");
}

export { csvCell };
