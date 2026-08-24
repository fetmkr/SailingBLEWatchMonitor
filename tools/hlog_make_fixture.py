#!/usr/bin/env python3
"""시험용 .HLG 파일을 만든다.

보드가 옆에 없어도 데스크탑 앱을 만들 수 있게 하려는 것이다.
펌웨어가 쓰는 것과 **바이트 하나까지 같은** 파일을 만든다.

    python3 tools/hlog_make_fixture.py out.HLG --minutes 20

만든 파일은 tools/hlog_parse.py 로 검사해서 통과해야 한다. 그래야 이 생성기가
규격을 제대로 따르고 있다는 뜻이다.

가짜 항해를 넣는다 — 상승풍 구간을 지그재그로 오르내리고, 태킹할 때마다
힐이 반대로 넘어간다. 타임라인이 제대로 그려지는지 눈으로 보려면 값이
움직여야 한다.
"""

import argparse
import math
import struct
import sys

HEADER_SIZE = 128
TYPE_NAV, NAV_SIZE = 0xA1, 38
TYPE_IMU, IMU_SIZE = 0xB1, 27
KNOTS_PER_MPS = 1.943844


def crc16(b: bytes) -> int:
    crc = 0xFFFF
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def make_header(session, utc_start, dur_s, nav_rows, imu_rows):
    h = bytearray(HEADER_SIZE)
    h[0:4] = b"HHLG"
    h[4] = 1                                     # ver major
    h[5] = 0                                     # ver minor
    struct.pack_into("<H", h, 6, HEADER_SIZE)
    h[8:14] = bytes([0x3C, 0xDC, 0x75, 0x70, 0x2F, 0xB4])
    struct.pack_into("<H", h, 14, 0x0100)        # fw
    h[16] = 1                                    # hw rev
    h[17] = 0xFF                                 # gnss type — 규격에 없는 모듈
    struct.pack_into("<I", h, 18, session)
    struct.pack_into("<H", h, 22, 3)             # boot count
    struct.pack_into("<I", h, 24, utc_start)
    struct.pack_into("<H", h, 28, 0)
    # mount_quat 은 0 (영점 안 잡음)
    h[38] = 0                                    # imu cal status
    h[39] = 10                                   # nav hz
    h[40] = 100                                  # imu hz
    h[41] = 1                                    # imu_type = MPU-9250
    h[42] = 1                                    # time_ref = UTC 환산
    h[43] = 1                                    # mag_scale = 0.1 µT/LSB
    h[44] = 0                                    # gnss_dyn = Portable
    h[45] = 10                                   # gnss_hz
    h[46] = 0                                    # sog_src = 도플러 원본
    h[47] = 1                                    # quat_src = 없음
    struct.pack_into("<I", h, 48, dur_s)
    struct.pack_into("<I", h, 52, nav_rows)
    struct.pack_into("<I", h, 56, imu_rows)
    struct.pack_into("<I", h, 60, 0)             # dropped
    h[64] = 1                                    # closed
    h[65] = 1                                    # heel_axis  = Y
    h[66] = 1                                    # heel_sign  = -
    h[67] = 2                                    # pitch_axis = Z
    h[68] = 0                                    # pitch_sign = +
    struct.pack_into("<f", h, 69, 0.0)           # heel_off
    struct.pack_into("<f", h, 73, 0.0)           # pitch_off
    struct.pack_into("<H", h, 126, crc16(bytes(h[:126])))
    return bytes(h)


def nav_record(ms, itow, week, lat, lon, sog_mms, cog_cdeg, sv, fix,
               hacc_cm, batt_mv, event, mag):
    r = bytearray(NAV_SIZE)
    r[0] = TYPE_NAV
    struct.pack_into("<IIHiiHHBBHHB", r, 1, ms, itow, week,
                     lat, lon, sog_mms, cog_cdeg, sv, fix, hacc_cm, batt_mv, event)
    struct.pack_into("<3h", r, 30, *mag)
    struct.pack_into("<H", r, 36, crc16(bytes(r[:36])))
    return bytes(r)


def imu_record(ms, acc, gyr, quat=(0, 0, 0, 0)):
    r = bytearray(IMU_SIZE)
    r[0] = TYPE_IMU
    struct.pack_into("<I", r, 1, ms)
    struct.pack_into("<3h", r, 5, *acc)
    struct.pack_into("<3h", r, 11, *gyr)
    struct.pack_into("<4h", r, 17, *quat)
    struct.pack_into("<H", r, 25, crc16(bytes(r[:25])))
    return bytes(r)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--minutes", type=float, default=20.0)
    ap.add_argument("--session", type=int, default=1)
    ap.add_argument("--utc", type=int, default=1787500000,
                    help="첫 fix 의 UNIX 초")
    ap.add_argument("--no-fix", action="store_true", help="위성을 못 잡은 세션")
    args = ap.parse_args()

    dur_s = args.minutes * 60
    n_imu = int(dur_s * 100)
    n_nav = int(dur_s * 10)

    # 인천 앞바다쯤에서 시작한다
    lat0, lon0 = 37.4500, 126.5500
    week = 2400
    tow0 = 3 * 86400 * 1000        # 수요일 0시

    body = bytearray()
    # 레코드는 생긴 순서대로 섞인다. IMU 10개마다 NAV 하나.
    lat, lon = lat0, lon0
    for i in range(n_imu):
        ms = 1000 + i * 10
        t = i / 100.0

        # 태킹: 90초마다 좌우가 바뀐다
        leg = int(t // 90)
        tack = 1 if leg % 2 == 0 else -1
        # 태킹 직후 5초 동안은 힐이 넘어가는 중
        into = t - leg * 90
        turn = math.tanh(into / 2.0) if into < 10 else 1.0

        heel = tack * turn * 18.0 + 2.0 * math.sin(t * 1.7)      # 도
        pitch = 2.0 * math.sin(t * 0.9) - 1.0
        # 파도. 100 Hz 로 남기는 이유가 이거다 — 10 Hz 로는 뭉개진다
        wave = 0.25 * math.sin(t * 6.5) + 0.12 * math.sin(t * 11.3)

        # ★ 축은 실제 장착 방향과 맞춘다 (firmware-rak/src/main.cpp 의 "힐과 피치").
        #     힐   = asin(-가속Y / 크기)      → ay = -sin(heel)
        #     피치 = asin(+가속Z / 크기)      → az =  sin(pitch)
        #     남은 X 가 위아래를 향한다        → ax = -cos(heel)cos(pitch)
        #   파도는 위아래 축에 얹는다.
        ay = -math.sin(math.radians(heel))
        az = math.sin(math.radians(pitch))
        ax = -math.cos(math.radians(heel)) * math.cos(math.radians(pitch)) + wave
        gx = 30.0 * math.cos(t * 6.5)
        gy = 8.0 * math.sin(t * 3.1)
        gz = tack * (25.0 if into < 8 else 0.0) + 3.0 * math.sin(t * 2.2)

        body += imu_record(ms,
                           (int(ax * 1000), int(ay * 1000), int(az * 1000)),
                           (int(gx * 32), int(gy * 32), int(gz * 32)))

        if i % 10 == 0:
            k = i // 10
            sog_kn = 5.4 + 1.3 * math.sin(t * 0.35) - (0.9 if into < 6 else 0.0)
            cog = (40.0 if tack > 0 else 320.0) + 4.0 * math.sin(t * 0.8)
            # 대충 그 방향으로 나아간다
            mps = sog_kn / KNOTS_PER_MPS
            lat += mps * math.cos(math.radians(cog)) * 0.1 / 111320.0
            lon += mps * math.sin(math.radians(cog)) * 0.1 / (111320.0 * math.cos(math.radians(lat)))

            fix = 0 if args.no_fix else 1
            ev = 0
            if k > 0 and k % 1800 == 0:
                ev = 0x01                          # 3분마다 마킹 한 번
            if k == 0:
                ev |= 0x02                         # 세션 첫 레코드

            if fix:
                body += nav_record(
                    ms, tow0 + k * 100, week,
                    int(lat * 1e7), int(lon * 1e7),
                    int(mps * 1000), int(cog * 100) % 36000,
                    11, 1, 80,
                    int(4100 - 200 * (t / dur_s)), ev,
                    (int(-3.0 * 10), int(-21.0 * 10), int(-17.0 * 10)))
            else:
                body += nav_record(
                    ms, 0xFFFFFFFF, 0xFFFF,
                    -0x80000000, -0x80000000,
                    0xFFFF, 0xFFFF, 0, 0, 0xFFFF,
                    int(4100 - 200 * (t / dur_s)), ev,
                    (int(-3.0 * 10), int(-21.0 * 10), int(-17.0 * 10)))

    hdr = make_header(args.session,
                      0 if args.no_fix else args.utc,
                      int(dur_s), n_nav, n_imu)
    with open(args.out, "wb") as f:
        f.write(hdr)
        f.write(body)

    total = HEADER_SIZE + len(body)
    print(f"{args.out}  {total:,} 바이트   NAV {n_nav:,} / IMU {n_imu:,}   "
          f"{args.minutes:.0f}분")
    print("검사:  python3 tools/hlog_parse.py " + args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
