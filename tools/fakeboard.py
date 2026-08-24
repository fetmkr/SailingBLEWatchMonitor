#!/usr/bin/env python3
"""보드 흉내를 내는 작은 서버. 데스크탑 앱의 회수 화면을 보드 없이 시험한다.

    python3 tools/fakeboard.py
    앱에서 주소를 127.0.0.1:8099 로 두고 [보드] 칸 → [목록]

일부러 험한 것을 섞어 둔다 — 버린 줄이 있는 세션, 제대로 안 닫힌 파일,
위성을 못 잡은 세션. 화면이 그걸 제대로 알려 주는지 봐야 한다.
"""
import http.server, json, os, socketserver
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HLG = os.path.join(ROOT, "desktop", "public", "sample.HLG")
HLG8 = os.path.join(ROOT, "desktop", "public", "sample8.HLG")
if not os.path.exists(HLG):
    raise SystemExit("먼저 시험용 파일을 만드세요:\n"
                     "  python3 tools/hlog_make_fixture.py desktop/public/sample.HLG --minutes 6 --session 7\n"
                     "  python3 tools/hlog_make_fixture.py desktop/public/sample8.HLG --minutes 12 --session 8 --utc 1787540000")
FILES = [
  {"name":"S00007.HLG","size":os.path.getsize(HLG),"ok":True,"closed":True,"session":7,
   "module":"3C:DC:75:70:2F:B4","utc_start":1787500000,"utc_start_ms":0,"duration_s":360,
   "nav_rows":3600,"imu_rows":36000,"dropped":0,"imu_type":1,"gnss_dyn":0,
   "nav_hz":10,"imu_hz":100,"fixed":True},
  {"name":"S00008.HLG","size":(os.path.getsize(HLG8) if os.path.exists(HLG8) else 66000000),"ok":True,"closed":True,"session":8,
   "module":"3C:DC:75:70:2F:B4","utc_start":1787540000,"utc_start_ms":0,"duration_s":7200,
   "nav_rows":72000,"imu_rows":720000,"dropped":0,"imu_type":1,"gnss_dyn":0,
   "nav_hz":10,"imu_hz":100,"fixed":True},
  {"name":"S00009.HLG","size":1200000,"ok":True,"closed":False,"session":9,
   "module":"3C:DC:75:70:2F:AA","utc_start":0,"utc_start_ms":0,"duration_s":52,
   "nav_rows":0,"imu_rows":0,"dropped":37,"imu_type":1,"gnss_dyn":255,
   "nav_hz":10,"imu_hz":100,"fixed":False},
]
class H(http.server.BaseHTTPRequestHandler):
    def _h(self, code, ctype, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
    def do_GET(self):
        if self.path == "/api/files":
            b = json.dumps({"ok": True, "files": FILES}).encode()
            self._h(200, "application/json"); self.wfile.write(b)
        elif self.path.startswith("/file/"):
            name = self.path[6:]
            # 이름마다 다른 파일을 준다. 그래야 보관함이 늘어나는지 볼 수 있다.
            path = HLG8 if ("00008" in name and os.path.exists(HLG8)) else HLG
            data = open(path, "rb").read()
            self._h(200, "application/octet-stream",
                    {"Content-Length": str(len(data)), "Accept-Ranges": "bytes"})
            self.wfile.write(data)
        else:
            self._h(404, "application/json"); self.wfile.write(b'{"ok":false}')
    def log_message(self, *a): pass
socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("127.0.0.1", 8099), H) as s:
    print("가짜 보드 http://127.0.0.1:8099/")
    s.serve_forever()
