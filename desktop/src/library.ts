// 훈련 세션 보관함.
//
// 보드에서 받은 파일은 그냥 파일일 뿐이다. 코치가 쓰려면 이만큼이 붙어야 한다.
//
//   - 언제 어디서 한 훈련인가
//   - 누가 탔나 (모듈 MAC 만으로는 모른다)
//   - 조건은 어땠나 (바람·파도)
//   - 이미 받은 것인가 (같은 걸 두 번 받지 않게)
//   - 검사를 통과했나
//
// 원본 파일은 절대 고치지 않는다. 붙이는 값은 전부 따로 둔다. 나중에 규격이
// 바뀌거나 파서를 고쳐도 코치가 적어 둔 것은 안 날아간다.
//
// 어디에 두나
//   <앱 데이터>/logs/<모듈>_<세션번호>.HLG    원본 그대로
//   <앱 데이터>/library.json                  붙인 값들
//
// **원본과 붙인 값을 갈라 두는 게 핵심이다.**

import {
  BaseDirectory, mkdir, readFile, readTextFile, writeFile, writeTextFile, exists,
} from "@tauri-apps/plugin-fs";
import type { Header } from "./hlog";

const DIR = "logs";
const INDEX = "library.json";

// ── 앱 밖(브라우저)에서도 화면이 돌게 ────────────────────────────────────
//
// 만드는 동안 브라우저로 열어 보는 게 훨씬 빠르다. 그런데 브라우저에는
// Tauri 의 파일 기능이 없어서 보관함이 통째로 죽는다.
//
// 그래서 Tauri 가 없으면 목록은 localStorage 에, 파일은 램에 둔다.
// **이건 만드는 동안만 쓰는 대비책이다.** 창을 닫으면 파일은 사라진다.
const inApp = typeof (globalThis as any).__TAURI_INTERNALS__ !== "undefined";
const memFiles = new Map<string, Uint8Array>();

export const persistent = inApp;

/** 한 세션에 대해 우리가 아는 전부. */
export interface Entry {
  /** 보관함 안에서의 이름. 모듈+세션번호라 두 번 받아도 하나다 */
  id: string;
  file: string;               // logs/ 아래 파일 이름
  bytes: number;

  // ── 파일에서 읽은 것 (고치지 않는다) ──
  module: string;
  session: number;
  utcStart: number;           // 0 이면 위성을 못 잡은 세션
  durationS: number;
  navRows: number;
  imuRows: number;
  dropped: number;
  closed: boolean;

  // ── 받은 뒤 검사 결과 ──
  verified: boolean;
  problems: string[];
  fetchedAt: number;          // UNIX 초

  // ── 코치가 붙이는 것 ──
  title: string;              // "화요일 오전 상승풍"
  sailor: string;             // 선수 이름
  boatClass: string;          // ILCA / 470 / 49er / iQFoil
  venue: string;              // "왕산 마리나"
  windKn: string;             // 자유롭게. "8~12" 처럼 적어도 된다
  windDir: string;
  waves: string;              // "잔잔" / "1m 너울"
  rig: string;                // 리그 튜닝 메모
  notes: string;
  /** 훈련 묶음. 같은 날 같은 세션에 여러 척이 나가면 같은 값을 준다 */
  group: string;
  starred: boolean;

  /**
   * 마킹에 코치가 손댄 것.
   *
   * **원본 파일은 안 건드린다.** 배에서 찍힌 마킹은 파일 안에 그대로 있고,
   * 여기에는 "무슨 메모를 붙였나 / 어느 것을 감췄나 / 어느 것을 더했나" 만
   * 둔다. 그래야 나중에 원본을 다시 읽어도 코치가 적은 것이 안 날아간다.
   */
  markNotes?: Record<string, string>;   // 시각(ms) → 메모
  markHidden?: number[];                // 감춘 파일 마킹의 시각(ms)
  markAdded?: { ms: number; note: string }[];
}

export interface Library {
  version: 1;
  entries: Entry[];
  /** 마지막으로 보던 세션. 앱을 껐다 켜면 이걸 다시 연다. 없으면 빈 화면. */
  lastOpen?: string;
}

const EMPTY: Library = { version: 1, entries: [] };

export function entryId(module: string, session: number): string {
  return `${module.replace(/:/g, "")}_${String(session).padStart(5, "0")}`;
}

export async function load(): Promise<Library> {
  if (!inApp) {
    try {
      const txt = localStorage.getItem(INDEX);
      if (txt) {
        const l = JSON.parse(txt) as Library;
        if (l?.version === 1 && Array.isArray(l.entries)) return l;
      }
    } catch { /* 처음이면 없는 게 정상이다 */ }
    return { ...EMPTY, entries: [] };
  }
  try {
    const txt = await readTextFile(INDEX, { baseDir: BaseDirectory.AppData });
    const lib = JSON.parse(txt) as Library;
    if (lib?.version === 1 && Array.isArray(lib.entries)) return lib;
  } catch {
    /* 처음이면 없는 게 정상이다 */
  }
  return { ...EMPTY, entries: [] };
}

export async function save(lib: Library): Promise<void> {
  if (!inApp) { localStorage.setItem(INDEX, JSON.stringify(lib)); return; }
  await mkdir("", { baseDir: BaseDirectory.AppData, recursive: true });
  await writeTextFile(INDEX, JSON.stringify(lib, null, 2), {
    baseDir: BaseDirectory.AppData,
  });
}

/**
 * 받은 파일을 보관함에 넣는다.
 *
 * 같은 모듈·같은 세션이 이미 있으면 **덮어쓰지 않고** 코치가 적어 둔 것을
 * 그대로 두면서 파일만 갈아 끼운다. 받다 만 것을 다시 받은 경우다.
 */
export async function put(
  lib: Library,
  bytes: Uint8Array,
  header: Header,
  verified: boolean,
  problems: string[],
): Promise<{ lib: Library; entry: Entry }> {
  const id = entryId(header.module, header.session);
  const file = `${DIR}/${id}.HLG`;

  if (inApp) {
    await mkdir(DIR, { baseDir: BaseDirectory.AppData, recursive: true });
    await writeFile(file, bytes, { baseDir: BaseDirectory.AppData });
  } else {
    memFiles.set(file, bytes);
  }

  const before = lib.entries.find((e) => e.id === id);
  const entry: Entry = {
    ...(before ?? {
      title: "", sailor: "", boatClass: "", venue: "",
      windKn: "", windDir: "", waves: "", rig: "", notes: "",
      group: "", starred: false,
      markNotes: {}, markHidden: [], markAdded: [],
    }),
    id, file, bytes: bytes.length,
    module: header.module,
    session: header.session,
    utcStart: header.utcStart,
    durationS: header.durationS,
    navRows: header.navRows,
    imuRows: header.imuRows,
    dropped: header.dropped,
    closed: header.closed,
    verified, problems,
    fetchedAt: Math.floor(Date.now() / 1000),
  };

  const entries = lib.entries.filter((e) => e.id !== id);
  entries.push(entry);
  entries.sort((a, b) => (b.utcStart || b.fetchedAt) - (a.utcStart || a.fetchedAt));

  // 새로 받은 것이 곧 보고 있는 것이다. lastOpen 을 여기서 옮겨 둔다.
  const next: Library = { version: 1, entries, lastOpen: id };
  await save(next);
  return { lib: next, entry };
}

export async function readEntry(e: Entry): Promise<Uint8Array> {
  if (!inApp) {
    const b = memFiles.get(e.file);
    if (!b) throw new Error("이 판에서는 파일이 램에만 있습니다 (앱에서 열어야 남습니다)");
    return b;
  }
  const b = await readFile(e.file, { baseDir: BaseDirectory.AppData });
  return new Uint8Array(b);
}

export async function hasFile(e: Entry): Promise<boolean> {
  if (!inApp) return memFiles.has(e.file);
  try {
    return await exists(e.file, { baseDir: BaseDirectory.AppData });
  } catch {
    return false;
  }
}

/**
 * 코치가 적은 것을 고친다. 원본 파일은 안 건드린다.
 *
 * ★ 그 자리에서 고치고 저장은 미뤄서 한 번에 한다.
 *
 *   예전에는 새 Library 를 만들어 돌려주고 부르는 쪽이 갈아 끼웠는데,
 *   저장이 비동기라 여러 칸을 빠르게 고치면 서로 덮어썼다. 다섯 칸을
 *   연달아 고쳤더니 마지막 하나만 남았다 (실측).
 *
 *   지금은 배열을 그 자리에서 고치므로 겹칠 수가 없다. 디스크 쓰기는
 *   0.3초 뒤에 한 번만 한다.
 */
let saveTimer: ReturnType<typeof setTimeout> | null = null;

/** 지금 보고 있는 세션을 적어 둔다. 앱을 껐다 켜면 이걸 다시 연다. */
export function noteOpen(lib: Library, id: string | null): Library {
  if (lib.lastOpen === (id ?? undefined)) return lib;
  lib.lastOpen = id ?? undefined;
  void save(lib);
  return lib;
}

export function update(lib: Library, id: string, patch: Partial<Entry>): Library {
  const e = lib.entries.find((x) => x.id === id);
  if (!e) return lib;
  Object.assign(e, patch);
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => { saveTimer = null; void save(lib); }, 300);
  return lib;
}

export async function remove(lib: Library, id: string): Promise<Library> {
  // 파일은 남긴다. 목록에서만 뺀다 — 실수로 훈련 기록이 날아가면 안 된다.
  const i = lib.entries.findIndex((e) => e.id === id);
  if (i >= 0) lib.entries.splice(i, 1);
  await save(lib);
  return lib;
}

/** 날짜별로 묶는다. 위성을 못 잡은 세션은 받은 날로 묶는다. */
export function byDay(entries: Entry[]): { day: string; items: Entry[] }[] {
  const map = new Map<string, Entry[]>();
  for (const e of entries) {
    const t = (e.utcStart || e.fetchedAt) * 1000;
    const d = new Date(t);
    const key = `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
    (map.get(key) ?? map.set(key, []).get(key)!).push(e);
  }
  return [...map.entries()]
    .sort((a, b) => (a[0] < b[0] ? 1 : -1))
    .map(([day, items]) => ({ day, items }));
}

/** 찾기. 제목·선수·장소·메모를 다 뒤진다. */
export function search(entries: Entry[], q: string): Entry[] {
  const s = q.trim().toLowerCase();
  if (!s) return entries;
  return entries.filter((e) =>
    [e.title, e.sailor, e.boatClass, e.venue, e.notes, e.group, e.module,
     String(e.session)]
      .join(" ").toLowerCase().includes(s));
}
