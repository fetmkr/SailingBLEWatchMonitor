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

/** 이 판에서 USB 를 쓸 수 있나. 브라우저로 열어 보는 중이면 못 쓴다. */
export const usable = inApp;

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
  const raw = await SerialPort.available_ports();
  const out: Port[] = [];
  for (const [path, info] of Object.entries(raw ?? {})) {
    // 블루투스 가짜 포트는 늘 떠 있는데 우리 것이 아니다.
    if (/bluetooth|debug-console|wlan/i.test(path)) continue;
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
export async function sniff(path: string, ms = 1800): Promise<string | null> {
  if (!inApp) return null;
  let port: SerialPort | null = null;
  let handle: { unwatch: () => Promise<void> } | null = null;
  try {
    port = new SerialPort({ path, baudRate: 115200 });
    await port.open();
    let seen = "";
    let name: string | null = null;
    handle = await port.watch({
      onData: (d) => {
        seen += typeof d === "string" ? d : new TextDecoder().decode(d);
        const m = /\b(SAIL-[^\s|]+)/.exec(seen);
        if (m) name = m[1];
      },
    });
    const until = Date.now() + ms;
    while (!name && Date.now() < until) {
      await new Promise((r) => setTimeout(r, 120));
    }
    return name;
  } catch {
    return null;                 // 다른 프로그램이 잡고 있을 수도 있다
  } finally {
    try { await handle?.unwatch(); } catch { /* 이미 끝났으면 그만 */ }
    try { await port?.close(); } catch { /* 같음 */ }
  }
}

/**
 * 시리얼로 한 줄씩 주고받는다.
 *
 * 보드는 1초에 한 번 상태 줄을 뱉는다. 그래서 우리가 물은 것과 상관없는
 * 줄이 섞여 들어온다. 답을 기다릴 때 그것들을 걸러야 한다.
 */
export class Link {
  private lines: string[] = [];
  private waiters: ((l: string) => void)[] = [];
  private handle: { unwatch: () => Promise<void> } | null = null;
  private buf = "";

  private constructor(private port: SerialPort, public readonly path: string) {}

  static async open(path: string): Promise<Link> {
    // 이 보드의 USB 는 UART 다리가 아니라 칩에 붙은 USB 라, 속도 설정은
    // 뜻이 없다. 그래도 부품이 값을 요구해서 넣어 준다.
    const port = new SerialPort({ path, baudRate: 115200 });
    await port.open();
    const link = new Link(port, path);
    link.handle = await port.watch({
      onData: (d) => link.feed(typeof d === "string" ? d : new TextDecoder().decode(d)),
    });
    return link;
  }

  private feed(text: string) {
    this.buf += text;
    for (;;) {
      const i = this.buf.indexOf("\n");
      if (i < 0) break;
      const line = this.buf.slice(0, i).trim();
      this.buf = this.buf.slice(i + 1);
      if (!line) continue;
      const w = this.waiters.shift();
      if (w) w(line); else this.lines.push(line);
    }
  }

  async close() {
    try { await this.handle?.unwatch(); } catch { /* 이미 끝났으면 그만 */ }
    try { await this.port.close(); } catch { /* 같음 */ }
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
    await this.say(line);
    const until = Date.now() + ms;
    for (;;) {
      const l = await this.hear(Math.max(300, until - Date.now()));
      if (l === null) return null;
      const m = /\[CTL\]\s*→\s*(.*)$/.exec(l);
      if (m) return m[1].trim();
      if (Date.now() > until) return null;
    }
  }
}
