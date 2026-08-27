// 보드를 USB 로 찾고, WiFi 를 켜라고 시킨다.
//
// 왜 이게 필요한가
//   보드 WiFi 는 평소 꺼져 있고, 켜라고 시키는 길이 블루투스뿐이었다.
//   블루투스가 없는 컴퓨터(데스크탑, 회사 정책으로 막힌 노트북)에서는
//   보드를 깨울 방법이 아예 없었다. 앱의 "주소로" 는 이미 WiFi 가 켜져
//   있어야 쓸 수 있어서 답이 안 된다.
//
// ★ 보드는 BLE 와 시리얼이 **같은 명령**을 쓴다 (PROTOCOL.md §9).
//   그래서 새로 만들 규격이 없다. 통로만 갈아 끼우면 된다.
//
// ★ 보내는 명령은 `wifi ap` 다.
//   `wifi on`(붙기)은 보드가 WiFi 이름과 비밀번호를 알고 있어야 한다.
//   그걸 넣는 게 제일 어려운 일이다. AP 는 그게 아예 필요 없다 — 보드가
//   스스로 만드니까. 주소도 늘 192.168.4.1 이다.
//
// 파일은 WiFi 로 받는다. USB 로도 보낼 수 있지만 재보니 더 느리다.
//   WiFi AP 직결  574 KB/초      USB 시리얼  266 KB/초  (둘 다 실측)

import { SerialPort } from "tauri-plugin-serialplugin-api";

const inApp = typeof (globalThis as any).__TAURI_INTERNALS__ !== "undefined";

// 이 기기에서 USB 를 쓸 수 있는지는 여기서 정하지 않는다.
// platform.ts 의 caps() 하나만 본다.

/** 꽂혀 있는 것 하나. */
export interface Port {
  path: string;
  label: string;      // 사람에게 보여줄 이름
  likely: boolean;    // 우리 보드일 것 같은가 (VID 로 짐작)
  /** 들어 봐서 알아낸 배 이름. 확인된 것만 들어 있다 */
  boat?: string;
}

/**
 * 우리 보드일 만한 것을 골라낸다.
 *
 * RAK3112 는 칩에 붙은 USB 를 쓴다. 맥에서 이렇게 보인다.
 *   /dev/cu.usbmodem101   "USB JTAG/serial debug unit"   VID 303a
 * [확인: 실기기에서 pyserial 로 읽은 값]
 *
 * VID 만으로는 "ESP32-S3 를 쓰는 무언가" 까지만 안다. 확실히 하려면
 * 들어 봐야 한다 — sniff() 참조.
 */
const ESPRESSIF_VID = "303a";

function score(path: string, info: any): boolean {
  const vid = String(info?.vid ?? info?.vendor_id ?? "").toLowerCase();
  if (vid.includes(ESPRESSIF_VID)) return true;
  const t = `${path} ${info?.product ?? ""} ${info?.manufacturer ?? ""}`.toLowerCase();
  return t.includes("usbmodem") || t.includes("espressif") || t.includes("jtag");
}

/**
 * 꽂혀 있는 것을 늘어놓는다.
 *
 * `all` 이 아니면 **우리 보드일 만한 것만** 준다. 맥에는 블루투스 가짜
 * 포트를 비롯해 늘 여러 개가 떠 있어서, 다 보여주면 고를 수가 없다.
 */
export async function list(all = false): Promise<Port[]> {
  if (!inApp) return [];
  // 한 기기가 여러 이름으로 뜨는 것을 부품 쪽에서 한 번 걸러 준다.
  const raw = await SerialPort.available_ports({ singlePortPerDevice: true });
  const out: Port[] = [];
  for (const [path, info] of Object.entries(raw ?? {})) {
    // 블루투스 가짜 포트는 늘 떠 있는데 우리 것이 아니다.
    if (/bluetooth|debug-console|wlan/i.test(path)) continue;
    // ★ 맥은 시리얼 장치 하나를 두 이름으로 보여준다.
    //     /dev/cu.usbmodem101    이쪽을 쓴다
    //     /dev/tty.usbmodem101   같은 기기다. 목록에 두 줄로 나온다
    //   tty 쪽은 상대가 신호를 줄 때까지 열리지 않고 기다리는 성질이 있어서
    //   우리 쓰임에는 cu 가 맞다.
    if (/^\/dev\/tty\./.test(path)) continue;
    const i = info as any;
    const name = i?.product || i?.manufacturer || "";
    const likely = score(path, i);
    if (!all && !likely) continue;
    out.push({ path, label: name ? `${path} — ${name}` : path, likely });
  }
  out.sort((a, b) => Number(b.likely) - Number(a.likely) || a.path.localeCompare(b.path));
  return out;
}

/**
 * 들어 보고 우리 보드인지 알아낸다. **아무것도 안 보낸다.**
 *
 * 보드가 1초에 한 번 이렇게 뱉는다.
 *   [38459.4s] SAIL-random() | SOG --.-- kn | COG   ---° | HEEL   +1.8° | …
 *
 * 그래서 열고 듣기만 하면 된다. 남의 기기일 수도 있는데 거기에 글자를
 * 써 넣는 건 예의가 아니다 — 듣기만 하면 아무 일도 안 일어난다.
 *
 * 못 알아내면 null. 그래도 목록에서 빼지는 않는다 — 기록 중이거나 말이
 * 없는 상태일 수도 있고, 그럴 때도 [연결] 은 눌러 볼 수 있어야 한다.
 */
export async function sniff(path: string, ms = 2500): Promise<string | null> {
  if (!inApp) return null;
  let port: SerialPort | null = null;
  try {
    port = new SerialPort({ path, baudRate: 115200 });
    await port.open();
    const dec = new TextDecoder();
    let seen = "";
    const until = Date.now() + ms;
    while (Date.now() < until) {
      let got: Uint8Array | null = null;
      try {
        got = await port.readBinary({ timeout: 200, size: 2048 });
      } catch {
        got = null;            // 읽을 게 없으면 부품이 오류로 알린다
      }
      if (got && got.length) {
        seen += dec.decode(got, { stream: true });
        const m = /\b(SAIL-[^\s|]+)/.exec(seen);
        if (m) return m[1];
      } else {
        await new Promise((r) => setTimeout(r, 40));
      }
    }
    return null;
  } catch {
    return null;                 // 다른 프로그램이 잡고 있을 수도 있다
  } finally {
    try { await port?.close(); } catch { /* 이미 닫혔으면 그만 */ }
  }
}

/**
 * 시리얼로 한 줄씩 주고받는다.
 *
 * ★ **watch() 를 안 쓴다. readBinary() 로 직접 읽는다.**
 *
 *   watch() 는 줄 단위로 잘라 주면서 줄 끝의 \n 을 떼어 버린다. 그러면
 *   여러 줄이 한 덩어리로 붙어서 나눌 수가 없다. 실측으로 이렇게 왔다.
 *
 *     scan begin 7[CTL] → scan 0 -35 lock 우리WiFi[CTL] → scan 1 …
 *
 *   날바이트로 달라고 해도(decode: false) 같았다. 같은 포트를 pyserial 로
 *   읽으면 \n 이 멀쩡하다 — 부품이 없애는 것이다.
 *
 *   readBinary() 는 있는 그대로 준다. 대신 우리가 주기적으로 물어봐야 한다.
 */
export class Link {
  private lines: string[] = [];
  private waiters: ((l: string) => void)[] = [];
  private buf = "";
  private raw = "";
  private stop = false;
  private pump: Promise<void> | null = null;

  private constructor(private port: SerialPort, public readonly path: string) {}

  static async open(path: string): Promise<Link> {
    // 이 보드의 USB 는 UART 다리가 아니라 칩에 붙은 USB 라, 속도 설정은
    // 뜻이 없다. 그래도 부품이 값을 요구해서 넣어 준다.
    const port = new SerialPort({ path, baudRate: 115200 });
    await port.open();
    const link = new Link(port, path);
    link.pump = link.run();
    return link;
  }

  /** 계속 읽어서 줄로 나눈다. close() 할 때까지 돈다. */
  private async run() {
    const dec = new TextDecoder();
    while (!this.stop) {
      let got: Uint8Array | null = null;
      try {
        got = await this.port.readBinary({ timeout: 200, size: 4096 });
      } catch {
        // 읽을 게 없으면 부품이 오류로 알린다. 그건 정상이다.
        got = null;
      }
      if (got && got.length) this.feed(dec.decode(got, { stream: true }));
      else await new Promise((r) => setTimeout(r, 40));
    }
  }

  private emit(line: string) {
    const t = line.trim();
    if (!t) return;
    const w = this.waiters.shift();
    if (w) w(t); else this.lines.push(t);
  }

  private feed(text: string) {
    this.raw = (this.raw + text).slice(-600);
    this.buf += text;
    for (;;) {
      const i = this.buf.search(/\r?\n/);
      if (i < 0) break;
      const line = this.buf.slice(0, i);
      this.buf = this.buf.slice(i).replace(/^\r?\n/, "");
      this.emit(line);
    }
    // 줄바꿈 없이 오래 머물면 그것도 한 줄로 친다
    if (this.buf.length > 400) { this.emit(this.buf); this.buf = ""; }
  }

  /** 왜 답이 없는지 보여줄 때 쓴다. 마지막에 들린 것. */
  peek(): string[] { return this.raw ? [this.raw.slice(-120)] : []; }

  async close() {
    this.stop = true;
    try { await this.pump; } catch { /* 돌다 끝나면 그만 */ }
    try { await this.port.close(); } catch { /* 이미 닫혔으면 그만 */ }
  }

  async say(line: string) { await this.port.write(line + "\n"); }

  /** 다음 한 줄. 안 오면 null. */
  async hear(ms = 6000): Promise<string | null> {
    const buffered = this.lines.shift();
    if (buffered !== undefined) return buffered;
    return new Promise((res) => {
      const t = setTimeout(() => {
        const i = this.waiters.indexOf(fn);
        if (i >= 0) this.waiters.splice(i, 1);
        res(null);
      }, ms);
      const fn = (l: string) => { clearTimeout(t); res(l); };
      this.waiters.push(fn);
    });
  }

  /**
   * 보내고, **우리 답만** 골라 받는다.
   *
   * 보드가 1초에 한 번 뱉는 상태 줄이 섞인다. 시리얼에서는 우리 답 앞에
   * `[CTL] → ` 가 붙으므로 그걸로 가른다.
   */
  async ask(line: string, ms = 8000): Promise<string | null> {
    // 한 번 보내고 답이 없으면 절반 지점에서 한 번 더 보낸다.
    let resent = false;
    await this.say(line);
    const until = Date.now() + ms;
    for (;;) {
      const left = until - Date.now();
      if (left <= 0) return null;
      if (!resent && left < ms / 2) { resent = true; await this.say(line); }
      const l = await this.hear(Math.max(300, Math.min(left, 800)));
      if (l === null) continue;              // 상태 줄만 오는 중일 수 있다
      const m = /\[CTL\]\s*→\s*(.*)$/.exec(l);
      if (m) return m[1].trim();
    }
  }
}
