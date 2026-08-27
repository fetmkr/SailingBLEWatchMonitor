// 보드를 BLE 로 찾고, WiFi 를 켜라고 시킨다.
//
// 왜 이게 필요한가
//   보드 WiFi 는 평소에 꺼져 있다. 켜 두면 전기를 먹으니까. 그런데 꺼져 있으면
//   앱이 보드를 찾을 길이 없다 — IP 로는 안 보인다.
//
//   BLE 광고는 늘 나가고 있다. 그래서 BLE 로 찾고, BLE 로 "WiFi 켜" 를 시키고,
//   그 다음부터 WiFi 로 파일을 받는다.
//
// ★ WiFi 가 켜지는 순간 BLE 는 내려간다.
//   칩이 둘을 같이 켜면 죽는다 (PROTOCOL.md §9). 그래서 보드는 **답을 먼저
//   보내고 나서** WiFi 를 켠다. 그 답에 "어디로 찾아오라" 가 다 적혀 있다.
//   앱은 그 한 줄만 챙기면 된다.
//
// 규격    ../../PROTOCOL.md §9
// 부품    @mnlphlp/plugin-blec (btleplug 을 Tauri 에 맞춰 싼 것)

import {
  startScan, stopScan, connect, disconnect, subscribeString, sendString,
  getAdapterState, checkPermissions,
  type BleDevice,
} from "@mnlphlp/plugin-blec";
import { SERVICE_UUID, CONTROL_UUID, NAME_PREFIX } from "./protocol";

/** 주변에서 찾은 보드 하나. */
export interface Board {
  address: string;
  name: string;        // "SAIL-random()"
  rssi: number;        // dBm. -50 이면 아주 가깝고 -85 면 겨우 잡힌다
}

const inApp = typeof (globalThis as any).__TAURI_INTERNALS__ !== "undefined";

// 이 기기에서 BLE 를 쓸 수 있는지는 여기서 정하지 않는다.
// platform.ts 의 caps() 하나만 본다.

export async function ready(): Promise<{ ok: boolean; why: string }> {
  if (!inApp) return { ok: false, why: "앱에서만 됩니다 (브라우저로 열려 있습니다)" };
  try {
    if (!(await checkPermissions(true))) {
      return { ok: false, why: "블루투스 권한이 없습니다" };
    }
    const st = await getAdapterState();
    if (st !== "On") return { ok: false, why: `블루투스가 ${st === "Off" ? "꺼져" : "확인 안"} 있습니다` };
    return { ok: true, why: "" };
  } catch (e) {
    return { ok: false, why: String(e) };
  }
}

/**
 * 주변 보드를 찾는다.
 *
 * 우리 것만 고른다. 이름이 "SAIL-" 로 시작하는 것이다. 광고에 서비스 UUID 도
 * 실려 있지만, 폰·맥이 그 값을 늘 넘겨주지는 않아서 이름으로 거른다.
 */
export async function scan(
  ms: number,
  onList: (boards: Board[]) => void,
): Promise<void> {
  const seen = new Map<string, Board>();
  await startScan((devs: BleDevice[]) => {
    for (const d of devs) {
      const nm = d.name ?? "";
      const ours = nm.startsWith(NAME_PREFIX) ||
                   d.services?.some((s) => s.toLowerCase() === SERVICE_UUID.toLowerCase());
      if (!ours) continue;
      seen.set(d.address, { address: d.address, name: nm || "(이름 없음)", rssi: d.rssi });
    }
    // 가까운 것부터. 코치는 대개 자기 앞의 배를 고른다.
    onList([...seen.values()].sort((a, b) => b.rssi - a.rssi));
  }, ms);
}

export async function scanStop(): Promise<void> {
  try { await stopScan(); } catch { /* 이미 멈췄으면 그만이다 */ }
}

/**
 * 보드 한 대에 붙어서 말을 주고받는다.
 *
 * 한 번에 한 대만 붙는다. 부품이 그렇게 돼 있고, 우리도 한 대씩 받는다.
 */
export class Link {
  private lines: string[] = [];
  private waiters: ((l: string) => void)[] = [];
  private gone = false;

  private constructor(public readonly board: Board) {}

  static async open(board: Board, onGone?: () => void): Promise<Link> {
    const link = new Link(board);
    await connect(board.address, () => { link.gone = true; onGone?.(); });
    await subscribeString(CONTROL_UUID, SERVICE_UUID, (text) => {
      // 한 번에 여러 줄이 올 수 있다
      for (const l of text.split("\n")) {
        const line = l.trim();
        if (!line) continue;
        const w = link.waiters.shift();
        if (w) w(line); else link.lines.push(line);
      }
    });
    return link;
  }

  async close() {
    if (this.gone) return;
    try { await disconnect(); } catch { /* 이미 끊겼으면 그만이다 */ }
  }

  /** 한 줄 보낸다. 답은 안 기다린다. */
  async say(line: string) {
    await sendString(CONTROL_UUID, line + "\n", "withResponse", SERVICE_UUID);
  }

  /** 다음 한 줄을 기다린다. 안 오면 null. */
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

  /** 보내고 한 줄 받는다. */
  async ask(line: string, ms = 6000): Promise<string | null> {
    await this.say(line);
    return this.hear(ms);
  }

  /** 주변 WiFi 목록. 보드가 훑어서 알려준다 (BLE 는 안 끊긴다). */
  async wifiScan(ms = 12000): Promise<{ ssid: string; rssi: number; locked: boolean }[]> {
    await this.say("wifi scan");
    const out: { ssid: string; rssi: number; locked: boolean }[] = [];
    const until = Date.now() + ms;
    for (;;) {
      const l = await this.hear(Math.max(500, until - Date.now()));
      if (l === null || l === "scan end") break;
      // "scan 0 -34 lock 우리WiFi"
      const m = /^scan (\d+) (-?\d+) (lock|open) (.*)$/.exec(l);
      if (m) out.push({ ssid: m[4], rssi: +m[2], locked: m[3] === "lock" });
      if (Date.now() > until) break;
    }
    return out;
  }
}

/**
 * WiFi 를 켜라고 시키고, 어디로 찾아갈지 돌려준다.
 *
 * 보드는 답을 먼저 보내고 나서 WiFi 를 켠다. 붙는 데 몇 초 걸려서다.
 * WiFi 를 켜도 BLE 는 그대로 있다 — 파일을 보내는 동안에만 잠깐 내려간다.
 *
 *   AP    ok wifi ap SAIL-random() pass sailing1234 ip 192.168.4.1 users 0 by -
 *   접속   ok wifi joining 우리WiFi mdns sail-random.local last 192.168.0.76 users 1 by 192.168.0.5
 *
 * 끝의 users 는 지금 이 보드를 쓰고 있는 기기 수, by 는 그 기기의 주소다.
 * 보드는 한 번에 한 대만 상대하므로, 남이 쓰고 있으면 아예 안 붙는다.
 * by 가 내 주소면 내가 앱을 껐다 켠 것이니 그냥 이어서 쓴다.
 */
export interface WifiUp {
  kind: "ap" | "join";
  /**
   * 앱이 두드려 볼 주소들. 먼저 답하는 쪽을 쓴다.
   *
   * 접속 모드에서는 새 주소를 미리 알 수가 없다 — 공유기가 정하는데
   * 그때는 이미 BLE 가 끊겨 있다. 그래서 둘을 준다.
   *   이름(mDNS)   늘 맞지만 늦다. 실측 2.6초. 권한이 없으면 아예 안 된다.
   *   지난번 주소   대개 같은 주소가 다시 온다. 빠르다.
   */
  hosts: string[];
  ssid: string;       // 사람이 붙어야 할 WiFi 이름 (AP 일 때만 뜻이 있다)
  pass: string;
  /** 지금 이 보드를 쓰고 있는 기기 수. 옛 펌웨어는 안 보내니 0 이다. */
  users: number;
  /**
   * 그 기기의 주소. 없으면 빈 글자.
   *
   * 이게 필요한 이유 — users 만 보면 **내 앱을 껐다 켠 경우까지 남으로
   * 몰린다.** 주소를 견줘서 나면 그냥 이어서 쓴다.
   */
  by: string;
}

export function parseWifiUp(line: string): WifiUp | null {
  // ★ 줄 끝을 딱 맞춰서 읽지 않는다.
  //
  //   예전에는 정규식 하나로 끝까지 맞췄다. 그래서 보드에 `users`/`by` 를
  //   하나 붙였더니 옛 앱이 줄 전체를 못 읽고 "알 수 없는 답" 을 띄웠다.
  //   실제로 그렇게 깨졌다 (2026-08-27).
  //
  //   앞부분만 맞추고, 뒤는 "이름 값" 짝으로 훑는다. 모르는 이름은 지나간다.
  //   그래야 보드가 앞서 나가도 옛 앱이 계속 돈다.
  const tail = (rest: string) => {
    const t: Record<string, string> = {};
    const w = rest.trim().split(/\s+/).filter(Boolean);
    for (let i = 0; i + 1 < w.length; i += 2) t[w[i]] = w[i + 1];
    return t;
  };
  const real = (v?: string) => (v && v !== "-" ? v : "");

  //   ok wifi ap <이름…> pass <비번> ip <주소> [users N] [by 이름]
  let m = /^ok wifi ap (.+) pass (\S+) ip (\S+)(.*)$/.exec(line);
  if (m) {
    const t = tail(m[4]);
    return { kind: "ap", hosts: [m[3]], ssid: m[1], pass: m[2],
             users: Number(t.users ?? 0) || 0, by: real(t.by) };
  }

  //   ok wifi joining <이름…> mdns <이름.local> [last <주소>] [users N] [by 이름]
  m = /^ok wifi joining (.+) mdns (\S+)(.*)$/.exec(line);
  if (m) {
    const t = tail(m[3]);
    const hosts = [m[2]];
    if (real(t.last)) hosts.unshift(t.last);   // 빠른 쪽을 먼저
    return { kind: "join", hosts, ssid: m[1], pass: "",
             users: Number(t.users ?? 0) || 0, by: real(t.by) };
  }
  return null;
}
