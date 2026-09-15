#!/usr/bin/env python3
"""보드 기록 제어 시험. 포트를 열 때마다 보드가 리셋된다 (전원 끊김 흉내로 쓴다)."""
import base64, re, serial, struct, sys, time

PORT = "/dev/cu.usbmodem1101"
KEEP = re.compile(r"\[(REC|LOG|SLEEP|TEST|IMU|GPS|설정|SD|SOG)\]|@DUMP [SEX]|@HASH|방위|평평|버린 줄|닫힘|이어|옛 표시|NAV-PV|panic|Guru|abort")


def open_port():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = PORT, 115200, 0.1
    s.dtr = s.rts = False
    s.open()
    return s


def read_for(s, sec, show=True):
    out, t0, buf = [], time.time(), b""
    while time.time() - t0 < sec:
        buf += s.read(4096)
        while b"\n" in buf:
            ln, buf = buf.split(b"\n", 1)
            t = ln.decode("utf-8", "replace").rstrip()
            out.append(t)
            if show and KEEP.search(t):
                print(f"   {t}")
    return out


def cmd(s, c, wait):
    print(f"> {c}")
    s.write((c + "\n").encode())
    return read_for(s, wait)


def header(s, sess):
    s.reset_input_buffer()
    s.write(f"rec dump {sess} hlg 0 128\n".encode())
    lines = read_for(s, 6, show=False)
    b64 = "".join(l.split("@DUMP B ", 1)[1] for l in lines if "@DUMP B " in l)
    h = base64.b64decode(b64) if b64 else b""
    if len(h) < 128:
        print("   머리글 못 받음:", [l for l in lines if "@DUMP" in l][:3]); return
    utc, = struct.unpack_from("<I", h, 24)
    dur, nav, imu, drop = struct.unpack_from("<IIII", h, 48)
    print(f"   머리글 세션{sess}: utc={utc} dur={dur}s nav={nav} imu={imu} dropped={drop} closed={h[64]}")
    print(f"   방위 설정: formula={h[81]} axisA={h[82]} axisB={h[83]} signA={h[84]} signB={h[85]} "
          f"off={struct.unpack_from('<f',h,86)[0]:.2f} decl={struct.unpack_from('<f',h,90)[0]:.2f} "
          f"magHi={tuple(round(v,2) for v in struct.unpack_from('<3f',h,94))}")


def last_session(lines):
    for l in reversed(lines):
        m = re.search(r"세션 (\d+) 닫힘", l)
        if m: return int(m.group(1))
    return None


step = sys.argv[1] if len(sys.argv) > 1 else "all"
s = open_port()
print("== 켜짐 (리셋)"); read_for(s, 12)

if step in ("all", "basic"):
    print("\n== 1. 방위 표시와 정상 시작·종료")
    cmd(s, "hdg", 2)
    cmd(s, "rec on", 4)
    print("   -- 기록 중 설정 변경은 거절돼야 한다 (검토 4번)")
    cmd(s, "hdg off 10", 1)
    cmd(s, "level", 1)
    cmd(s, "sd", 1)
    out = cmd(s, "rec off", 6)
    n = last_session(out)
    if n:
        header(s, n)
        cmd(s, f"rec hash {n} hlg", 8)

if step in ("all", "gps"):
    print("\n== GPS 입력 5초 끊기")
    cmd(s, "sog", 1)
    cmd(s, "test gps 5", 3)
    cmd(s, "sog", 1)
    read_for(s, 4)
    cmd(s, "sog", 1)

if step in ("all", "fail"):
    print("\n== 2. 쓰기 실패 → 3초 뒤 새 파일로 다시 걸기")
    cmd(s, "rec on", 3)
    out = cmd(s, "rec fail 20", 14)
    cmd(s, "rec", 1)
    out += cmd(s, "rec off", 6)
    cmd(s, "rec fail clear", 1)

if step in ("all", "slow"):
    print("\n== 3. 닫기 20초 지연 — 닫는 동안 루프가 살아 있나, 닫는 중 다시 시작")
    cmd(s, "rec on", 3)
    cmd(s, "rec slow 20000", 1)
    t0 = time.time()
    cmd(s, "rec off", 1)
    cmd(s, "gps", 1)            # 루프가 명령을 받는지
    cmd(s, "rec on", 1)         # 닫는 중 시작 → 닫힌 뒤 시작
    out = read_for(s, 25)
    print(f"   rec off 부터 {time.time()-t0:.1f}초")
    cmd(s, "rec off", 6)

if step in ("all", "imu"):
    print("\n== 4. IMU 5초 끊긴 채 종료 — dropped 정산")
    cmd(s, "rec on", 3)
    cmd(s, "test imu 30", 7)    # 7초째에도 끊겨 있다
    out = cmd(s, "rec off", 6)
    n = last_session(out)
    if n: header(s, n)
    read_for(s, 25)             # 다시 붙는 동안 두 번 안 세는지 (기록 밖이라 0)

if step == "clean":
    print("\n== 3'. 닫기 20초 지연 — 루프가 살아 있나 · 닫는 중 시작하면 닫힌 뒤 시작")
    cmd(s, "rec fail clear", 1)
    cmd(s, "rec on", 4)
    cmd(s, "rec slow 20000", 1)
    t0 = time.time()
    cmd(s, "rec off", 1)
    cmd(s, "rec", 1)                 # 닫는 동안 명령이 먹나
    cmd(s, "rec on", 1)              # → 닫힌 뒤 시작
    out = read_for(s, 26)
    print(f"   rec off 부터 {time.time()-t0:.1f}초")
    print("\n== 4'. 기록 중 IMU 끊긴 채 종료 — dropped 정산")
    cmd(s, "test imu 30", 7)         # 기록 중. 약 6초 끊김
    out = cmd(s, "rec off", 6)
    n = last_session(out)
    if n: header(s, n)
    print("   (IMU 가 돌아올 때까지 기다린다 — 닫힌 뒤라 더 안 센다)")
    read_for(s, 30)

if step in ("all", "resume"):
    print("\n== 5. 기록 중 리셋 → 켤 때 이어 시작")
    cmd(s, "rec on", 4)
    s.close(); time.sleep(0.5)
    s = open_port(); print("   (포트 다시 열어 리셋)"); read_for(s, 14)
    cmd(s, "rec off", 6)
    print("\n== 6. 사람이 멈춘 뒤 리셋 → 이어 시작 안 함")
    s.close(); time.sleep(0.5)
    s = open_port(); print("   (포트 다시 열어 리셋)"); read_for(s, 14)
    cmd(s, "rec", 1)
s.close()
