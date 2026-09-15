#!/usr/bin/env python3
"""USB 시리얼로 세션 파일을 받는다. 맥 WiFi 를 갈아타지 않는 회수 길이다.

    python3 tools/serial_dump.py <세션번호> <받을 폴더> [포트]

보드에 `rec dump <번호> <hlg|txt> <시작> <바이트>` 를 조각마다 보내고,
조각의 CRC32 가 맞을 때만 이어 붙인다. 틀리면 그 조각만 다시 청한다.
받다 끊기면 같은 명령을 다시 치면 된다 — 이미 받은 크기부터 잇는다.
포트를 열면 보드가 다시 켜진다.
"""
import base64, glob, os, sys, time, zlib

import serial

CHUNK = 262144


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False
    s.open()
    time.sleep(12)            # 켜지는 동안 나오는 로그를 흘려보낸다
    s.reset_input_buffer()
    return s


def request(s, sess, kind, off, n):
    s.reset_input_buffer()
    s.write(f"rec dump {sess} {kind} {off} {n}\n".encode())
    size, parts, t0 = None, [], time.time()
    # ★ 이 요청의 S 줄(맞는 파일 종류)을 본 뒤에만 B·E 를 받는다.
    #   앞 요청(이름 묻기 `rec dump <n> txt 0 1`)의 B·E 가 reset_input_buffer 뒤에 늦게 도착해서,
    #   HLG 요청이 그걸 제 첫 조각으로 받고 S 를 못 본 채 끝났다 → "크기를 못 받음" (2026-09-15, 줄마다 찍어서 확인).
    ext = "." + kind.upper()
    while time.time() - t0 < 60:
        ln = s.readline().decode("ascii", "replace").strip()
        if not ln:
            continue
        i = ln.find("@DUMP ")
        if i < 0:
            continue
        ln = ln[i + 6:]
        tag, _, rest = ln.partition(" ")
        if tag == "X":
            raise RuntimeError(rest)
        if tag == "S":
            path, _, sz = rest.rpartition(" ")
            if not path.upper().endswith(ext):
                continue                              # 다른 파일의 늦게 온 줄
            size, parts = int(sz), []
            continue
        if size is None:
            continue                                  # S 전에 온 B·E 는 앞 요청의 것이다
        if tag == "B":
            try:
                parts.append(base64.b64decode(rest, validate=True))
            except Exception:
                return size, None                     # 줄이 깨졌다 → 다시
        elif tag == "E":
            o, cnt, crc = rest.split()
            data = b"".join(parts)
            if int(o) != off or int(cnt) != len(data) or zlib.crc32(data) != int(crc, 16):
                return size, None
            return size, data
    return size, None


def fetch(s, sess, kind, dst_dir):
    size, _ = request(s, sess, kind, 0, 1)
    if size is None:
        raise RuntimeError("크기를 못 받음")
    tmp = os.path.join(dst_dir, f".S{sess:05d}.{kind}.part")
    have = os.path.getsize(tmp) if os.path.exists(tmp) else 0
    t0 = time.time()
    with open(tmp, "ab") as f:
        while have < size:
            for attempt in range(5):
                _, data = request(s, sess, kind, have, min(CHUNK, size - have))
                if data:
                    break
                print(f"  {have} 조각 CRC 틀림, 다시 ({attempt + 1})")
            else:
                raise RuntimeError(f"{have} 에서 5번 실패")
            f.write(data); f.flush()
            have += len(data)
            el = time.time() - t0
            print(f"  {kind} {have}/{size} ({100*have/size:.0f}%)  {have/1024/max(el,1e-3):.0f} KB/s", flush=True)
    return tmp, size


def main():
    sess, dst_dir = int(sys.argv[1]), sys.argv[2]
    port = sys.argv[3] if len(sys.argv) > 3 else sorted(glob.glob("/dev/cu.usbmodem*"))[0]
    os.makedirs(dst_dir, exist_ok=True)
    s = open_port(port)
    for kind in ("txt", "hlg"):
        tmp, size = fetch(s, sess, kind, dst_dir)
        name = None
        # 카드 위 이름을 그대로 쓴다
        s.reset_input_buffer()
        s.write(f"rec dump {sess} {kind} 0 1\n".encode())
        t0 = time.time()
        while time.time() - t0 < 10 and not name:
            ln = s.readline().decode("utf-8", "replace")
            if "@DUMP S " in ln:
                name = os.path.basename(ln.split("@DUMP S ", 1)[1].rsplit(" ", 1)[0])
        final = os.path.join(dst_dir, name or f"S{sess:05d}.{kind.upper()}")
        os.replace(tmp, final)
        print(f"저장 {final}  {size} 바이트")


if __name__ == "__main__":
    main()
