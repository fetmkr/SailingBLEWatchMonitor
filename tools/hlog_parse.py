#!/usr/bin/env python3
"""경기정 모듈 바이너리 로그(.HLG) 파서 — 포맷 v1.0 참조 구현.

규격: docs/spec/로그포맷_v1.0_draft_2026-08-24.md
펌웨어 쪽 구현: firmware-rak/include/hlog.h

    python3 tools/hlog_parse.py S00005.HLG            검사 결과만
    python3 tools/hlog_parse.py S00005.HLG --csv out  NAV/IMU 를 CSV 로 뽑기
    python3 tools/hlog_parse.py S00005.HLG --head 20  앞 몇 줄 눈으로 보기

깨진 자리를 만나면 한 바이트씩 밀면서 다시 맞춘다 (규격 §2 의 슬라이딩 재동기).
type 바이트만으로는 우연히 맞을 수 있어서, CRC 와 **local_ms 가 뒤로 가지
않는다**는 두 조건을 같이 본다.
"""

import argparse
import struct
import sys
from collections import Counter

MAGIC = b"HHLG"
HEADER_SIZE = 128
TYPE_NAV, NAV_SIZE = 0xA1, 38
TYPE_IMU = 0xB1
# v1.0 은 27바이트(쿼터니언 8칸 포함), v1.1 부터 19바이트.
# 자세는 가속·자이로 원본에서 후처리로 뽑는다.
IMU_SIZE_V0, IMU_SIZE_V1 = 27, 19

# 값 없음 표식
LATLON_INVALID = -0x80000000
SOG_INVALID = 0xFFFF
COG_INVALID = 0xFFFF
ACC_INVALID = 0xFFFF
ITOW_INVALID = 0xFFFFFFFF
WEEK_INVALID = 0xFFFF

KNOTS_PER_MPS = 1.943844


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def parse_header(buf: bytes) -> dict:
    if len(buf) < HEADER_SIZE:
        raise ValueError("파일이 헤더보다 짧습니다")
    if buf[:4] != MAGIC:
        raise ValueError(f"magic 이 HHLG 가 아닙니다: {buf[:4]!r}")

    want = struct.unpack_from("<H", buf, 126)[0]
    got = crc16(buf[:126])
    h = {
        "ver": f"{buf[4]}.{buf[5]}",
        "header_size": struct.unpack_from("<H", buf, 6)[0],
        "module_id": ":".join(f"{b:02X}" for b in buf[8:14]),
        "fw_version": f"{buf[15]:x}.{buf[14]:x}",
        "hw_rev": buf[16],
        "gnss_type": buf[17],
        "session_id": struct.unpack_from("<I", buf, 18)[0],
        "boot_count": struct.unpack_from("<H", buf, 22)[0],
        "utc_start": struct.unpack_from("<I", buf, 24)[0],
        "utc_start_ms": struct.unpack_from("<H", buf, 28)[0],
        "mount_quat": struct.unpack_from("<4h", buf, 30),
        "imu_cal_status": buf[38],
        "log_rate_a": buf[39],
        "log_rate_b": buf[40],
        # reserved 에 우리가 채운 것 (hlog.h 의 표)
        "imu_type": buf[41],
        "time_ref": buf[42],
        "mag_scale": buf[43],
        "gnss_dyn": buf[44],
        "gnss_hz": buf[45],
        "sog_src": buf[46],
        "quat_src": buf[47],
        "crc_ok": want == got,
        "crc_want": want,
        "crc_got": got,
    }
    return h


def parse_nav(r: bytes) -> dict:
    (localMs, itow, week, lat, lon, sog, cog,
     numSv, fix, hAcc, batt, event) = struct.unpack_from("<IIHiiHHBBHHB", r, 1)
    mag = struct.unpack_from("<3h", r, 30)
    return {
        "type": "NAV", "local_ms": localMs,
        "itow": None if itow == ITOW_INVALID else itow,
        "week": None if week == WEEK_INVALID else week,
        "lat": None if lat == LATLON_INVALID else lat / 1e7,
        "lon": None if lon == LATLON_INVALID else lon / 1e7,
        "sog_kn": None if sog == SOG_INVALID else sog / 1000.0 * KNOTS_PER_MPS,
        "cog_deg": None if cog == COG_INVALID else cog / 100.0,
        "num_sv": numSv, "fix": fix,
        "h_acc_m": None if hAcc == ACC_INVALID else hAcc / 100.0,
        "batt_mv": batt, "event": event, "mag": mag,
    }


def parse_imu(r: bytes) -> dict:
    acc = struct.unpack_from("<3h", r, 5)
    gyr = struct.unpack_from("<3h", r, 11)
    return {
        "type": "IMU", "local_ms": struct.unpack_from("<I", r, 1)[0],
        "acc_g": tuple(v / 1000.0 for v in acc),
        "gyr_dps": tuple(v / 32.0 for v in gyr),
    }


def walk(buf: bytes, imu_size: int = IMU_SIZE_V1):
    """레코드를 훑는다. (레코드, 파일오프셋, 재동기했나) 를 내놓는다."""
    i = HEADER_SIZE
    n = len(buf)
    last_ms = None
    resynced = False
    while i < n:
        t = buf[i]
        size = NAV_SIZE if t == TYPE_NAV else (imu_size if t == TYPE_IMU else 0)
        ok = False
        if size and i + size <= n:
            want = struct.unpack_from("<H", buf, i + size - 2)[0]
            if want == crc16(buf[i:i + size - 2]):
                rec = parse_nav(buf[i:i + size]) if t == TYPE_NAV else parse_imu(buf[i:i + size])
                # 시각이 뒤로 가면 우연히 CRC 가 맞은 가짜다.
                # (한 바퀴 도는 49.7일은 한 세션에서 안 나온다)
                if last_ms is None or rec["local_ms"] + 5000 >= last_ms:
                    last_ms = max(last_ms or 0, rec["local_ms"])
                    yield rec, i, resynced
                    resynced = False
                    i += size
                    ok = True
        if not ok:
            i += 1              # 한 바이트 밀면서 다시 맞춘다
            resynced = True


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--csv", metavar="접두사", help="<접두사>_nav.csv / _imu.csv 로 뽑기")
    ap.add_argument("--head", type=int, default=0, help="앞 몇 줄 보여주기")
    args = ap.parse_args()

    buf = open(args.path, "rb").read()
    h = parse_header(buf)

    print("═" * 60)
    print(f"  {args.path}   {len(buf):,} 바이트")
    print("═" * 60)
    print(f"  포맷          v{h['ver']}   헤더 CRC {'맞음' if h['crc_ok'] else '★틀림★'}")
    print(f"  모듈          {h['module_id']}   세션 {h['session_id']}   부팅 {h['boot_count']}회")
    print(f"  펌웨어        v{h['fw_version']}   hw{h['hw_rev']}")
    print(f"  기록 주기     NAV {h['log_rate_a']} Hz / IMU {h['log_rate_b']} Hz")
    imu_name = {0: "BNO085", 1: "MPU-9250"}.get(h["imu_type"], f"?{h['imu_type']}")
    print(f"  IMU           {imu_name}   자세 {'융합값' if h['quat_src'] == 0 else '없음(가속·자이로 원본만)'}")
    gnss_name = {0: "RYS8839", 1: "SE868SY-D", 2: "RTK(예약)", 0xFF: "모름(규격에 없는 모듈)"}
    print(f"  GNSS 모듈     {gnss_name.get(h['gnss_type'], '?' + str(h['gnss_type']))}")
    print(f"  GNSS          동역학모델 {h['gnss_dyn']}, {h['gnss_hz']} Hz, "
          f"시각기준 {'GPS' if h['time_ref'] == 0 else 'UTC 환산'}")
    print(f"  속도 출처     {'도플러 원본' if h['sog_src'] == 0 else '★다듬은 값★'}")
    if h["utc_start"]:
        import datetime as dt
        t = dt.datetime.fromtimestamp(h["utc_start"], dt.timezone.utc)
        print(f"  첫 fix        {t.isoformat()} +{h['utc_start_ms']}ms")
    else:
        print("  첫 fix        없음 (세션 내내 위성을 못 잡았거나 아직 안 채움)")

    # 옛 파일은 IMU 레코드가 27바이트다. 머리글의 판 번호를 보고 고른다.
    imu_size = IMU_SIZE_V1 if (buf[4], buf[5]) >= (1, 1) else IMU_SIZE_V0

    navs, imus, resyncs = [], [], 0
    for rec, _off, was_resync in walk(buf, imu_size):
        if was_resync:
            resyncs += 1
        (navs if rec["type"] == "NAV" else imus).append(rec)

    used = HEADER_SIZE + len(navs) * NAV_SIZE + len(imus) * imu_size
    lost = len(buf) - used

    print("─" * 60)
    print(f"  NAV 레코드    {len(navs):,}")
    print(f"  IMU 레코드    {len(imus):,}")
    print(f"  재동기        {resyncs}회   {'(깨진 자리가 있습니다)' if resyncs else '(깨끗함)'}")
    print(f"  못 읽은 바이트 {lost:,}   {'★' if lost else ''}")

    def rate(recs, name, want):
        if len(recs) < 2:
            return
        span = (recs[-1]["local_ms"] - recs[0]["local_ms"]) / 1000.0
        if span <= 0:
            return
        hz = (len(recs) - 1) / span
        mark = "✓" if abs(hz - want) / want < 0.05 else "★"
        print(f"  {name} 실제 주기  {hz:6.2f} Hz  (규격 {want} Hz)  {mark}   {span:.1f}초")

    print("─" * 60)
    rate(navs, "NAV", h["log_rate_a"])
    rate(imus, "IMU", h["log_rate_b"])

    # 등간격인지 — 이게 이 포맷의 핵심이다
    if len(imus) > 2:
        gaps = Counter(imus[i + 1]["local_ms"] - imus[i]["local_ms"] for i in range(len(imus) - 1))
        print("  IMU 이웃 간격 (ms 별 개수):")
        for g, c in sorted(gaps.items())[:8]:
            bar = "█" * min(40, c * 40 // max(gaps.values()))
            pct = 100.0 * c / (len(imus) - 1)
            print(f"     {g:5d} ms  {c:7,}  {pct:5.1f}%  {bar}")
        even = gaps.get(10, 0) / (len(imus) - 1) * 100
        print(f"  → 10 ms 정확히 지킨 비율  {even:.2f} %")

    fixed = [r for r in navs if r["fix"]]
    print("─" * 60)
    print(f"  fix 잡힌 NAV  {len(fixed):,} / {len(navs):,}")
    if fixed:
        sog = [r["sog_kn"] for r in fixed if r["sog_kn"] is not None]
        if sog:
            print(f"  속도          최대 {max(sog):.2f} kn  평균 {sum(sog)/len(sog):.2f} kn")
    if navs:
        mv = [r["batt_mv"] for r in navs]
        print(f"  배터리        {mv[0]} → {mv[-1]} mV")
        marks = [r for r in navs if r["event"] & 0x01]
        print(f"  마킹          {len(marks)}회" +
              (f"  (local_ms {', '.join(str(m['local_ms']) for m in marks[:5])})" if marks else ""))

    if args.head:
        print("─" * 60)
        for r in (navs[:args.head]):
            print("  NAV", r)
        for r in (imus[:args.head]):
            print("  IMU", r)

    if args.csv:
        with open(args.csv + "_nav.csv", "w") as f:
            f.write("local_ms,week,itow,lat,lon,sog_kn,cog_deg,num_sv,fix,h_acc_m,batt_mv,event,mx,my,mz\n")
            for r in navs:
                f.write(",".join("" if v is None else str(v) for v in [
                    r["local_ms"], r["week"], r["itow"], r["lat"], r["lon"],
                    r["sog_kn"], r["cog_deg"], r["num_sv"], r["fix"],
                    r["h_acc_m"], r["batt_mv"], r["event"], *r["mag"]]) + "\n")
        with open(args.csv + "_imu.csv", "w") as f:
            f.write("local_ms,ax,ay,az,gx,gy,gz\n")
            for r in imus:
                f.write(",".join(str(v) for v in
                                 [r["local_ms"], *r["acc_g"], *r["gyr_dps"]]) + "\n")
        print(f"─ CSV 로 뽑았습니다: {args.csv}_nav.csv / {args.csv}_imu.csv")

    print("═" * 60)
    bad = (not h["crc_ok"]) or resyncs or lost
    print("  ❌ 문제가 있습니다" if bad else "  ✅ 깨끗합니다")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
