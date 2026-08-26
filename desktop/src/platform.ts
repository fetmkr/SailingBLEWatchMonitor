/**
 * 이 기기에서 무엇이 되는가.
 *
 * 같은 코드로 맥·윈도·아이패드·안드로이드 태블릿을 다 만든다. 그러려면
 * 화면 코드가 **"지금 무슨 기기냐" 를 절대 묻지 않아야 한다.** 대신
 * "USB 를 쓸 수 있냐" 를 묻는다. 그래야 기기가 하나 늘어도 화면 코드를
 * 안 건드린다.
 *
 *     나쁜 방식   if (아이패드) { USB 버튼 숨기기 }
 *     좋은 방식   if (!caps().usb) { USB 버튼 숨기기 }
 *
 * 답은 러스트가 준다. **어떤 부품이 실제로 들어갔는지는 컴파일할 때
 * 정해지고, 그걸 아는 건 러스트뿐이다.** 화면에서 기기 이름을 보고
 * 짐작하면 언젠가 어긋난다. 진실을 한 군데에만 둔다.
 */

import { invoke } from "@tauri-apps/api/core";

export interface Caps {
  /** "macos" "windows" "linux" "ios" "android", 브라우저로 열었으면 "browser" */
  os: string;
  /** USB 시리얼로 보드를 깨울 수 있나 */
  usb: boolean;
  /** 블루투스로 보드를 찾을 수 있나 */
  ble: boolean;
  /** WiFi 로 보드에서 파일을 받을 수 있나 */
  wifi: boolean;
  /** 아무 경로나 직접 읽을 수 있나.
   *  맥·윈도는 된다. 아이패드·안드로이드는 고른 파일만 읽을 수 있다. */
  localFiles: boolean;
}

const inApp = typeof (globalThis as any).__TAURI_INTERNALS__ !== "undefined";

/** 브라우저로 열어 봤을 때의 답. 기기에 붙는 건 아무것도 못 한다. */
const BROWSER: Caps = {
  os: "browser", usb: false, ble: false, wifi: true, localFiles: false,
};

let cur: Caps = BROWSER;

/**
 * 앱이 뜰 때 한 번 부른다. 이 뒤로는 caps() 를 그냥 쓰면 된다.
 * 못 물어봐도 앱은 뜬다 — 브라우저로 열어 본 것과 같이 취급한다.
 */
export async function load(): Promise<Caps> {
  if (!inApp) return cur;
  try {
    cur = await invoke<Caps>("caps");
  } catch (e) {
    console.warn("[platform] 기기에 못 물어봤습니다:", e);
  }
  return cur;
}

/** 지금 기기에서 되는 것들. load() 를 부르기 전에는 브라우저 취급이다. */
export function caps(): Caps {
  return cur;
}
