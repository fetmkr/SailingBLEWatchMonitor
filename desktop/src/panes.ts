// 칸 크기 조절.
//
// 예전에는 나누개가 하나뿐이었다. 영상과 데이터 사이에 두고 그 둘의 크기만
// 바꿨다. 그런데 지도 칸이 끼어들면서 나누개는 지도와 데이터 사이로 밀렸고,
// 코드는 여전히 영상과 데이터를 건드렸다. 그래서 잡아 끌면 엉뚱한 칸이
// 움직이거나 아무 일도 안 일어났다.
//
// 이제 **붙어 있는 칸 사이마다** 나누개를 그때그때 만든다. 칸을 접거나
// 펴면 나누개도 따라서 다시 놓인다. 어긋날 자리가 없다.
//
// 가로로도 세로로도 쓴다. 어느 쪽인지만 알려주면 된다.

export type Dir = "row" | "col";

const GUTTER = 5;      // 나누개 두께 (px)
const KEY = "panes.v1";

interface Saved { [id: string]: number }   // 칸 id → 몫

let saved: Saved = load();

function load(): Saved {
  try {
    const j = localStorage.getItem(KEY);
    if (j) return JSON.parse(j) as Saved;
  } catch { /* 처음이면 없는 게 정상이다 */ }
  return {};
}

let saveTimer: ReturnType<typeof setTimeout> | null = null;
function save() {
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    try { localStorage.setItem(KEY, JSON.stringify(saved)); } catch { /* 못 써도 그만 */ }
  }, 300);
}

/**
 * 칸 하나가 어떻게 자리를 차지하나.
 *
 *   flex   남는 자리를 몫대로 나눠 갖는다. 가운데 칸들이 이것이다.
 *   fixed  정해진 크기를 지킨다. 양옆 서랍이 이것이다 — 창을 넓혀도
 *          서랍까지 같이 넓어지면 성가시다.
 */
export type Kind = "flex" | "fixed";

export interface PaneSpec {
  el: HTMLElement;
  kind: Kind;
  /** fixed 일 때의 기본 크기 (px) */
  base?: number;
  min: number;
}

interface Ctx {
  box: HTMLElement;
  dir: Dir;
  panes: PaneSpec[];
  onResize?: () => void;
}

const ctxs: Ctx[] = [];

/** 어느 통을 어떻게 나눌지 등록한다. 한 번만 부르면 된다. */
export function register(
  box: HTMLElement, dir: Dir, panes: PaneSpec[], onResize?: () => void,
) {
  const ctx: Ctx = { box, dir, panes, onResize };
  ctxs.push(ctx);
  for (const p of panes) {
    p.el.classList.add("pane");
    const v = saved[p.el.id];
    if (p.kind === "fixed") {
      const px = v ?? p.base ?? 260;
      p.el.style.flex = `0 0 ${px}px`;
    } else {
      p.el.style.flex = `${v ?? 1} 1 0`;
    }
    if (dir === "row") p.el.style.minWidth = `${p.min}px`;
    else p.el.style.minHeight = `${p.min}px`;
  }
  relayout(ctx);
}

/** 접거나 편 뒤에 부른다. 나누개를 다시 놓는다. */
export function refresh() { for (const c of ctxs) relayout(c); }

/** 통의 방향을 바꾼다 (가로 ↔ 세로). */
export function setDir(box: HTMLElement, dir: Dir) {
  const c = ctxs.find((x) => x.box === box);
  if (!c || c.dir === dir) return;
  c.dir = dir;
  c.box.style.flexDirection = dir === "row" ? "row" : "column";
  for (const p of c.panes) {
    p.el.style.minWidth = "";
    p.el.style.minHeight = "";
    if (dir === "row") p.el.style.minWidth = `${p.min}px`;
    else p.el.style.minHeight = `${p.min}px`;
  }
  relayout(c);
}

/**
 * 이 칸이 지금 자리를 차지하고 있나.
 *
 * ★ 접힌 표시를 **한 이름으로** 본다.
 *   예전에는 .mini 만 봤다. 그런데 영상·지도를 담은 줄은 접힐 때 .rowmini 가
 *   붙어서, 여기서는 안 접힌 것으로 셌다. 그 바람에 몫을 나눌 때 그 줄까지
 *   끼워 넣었고, 데이터의 몫이 0.91 로 남아 아래가 60px 비었다 (실측).
 */
function visible(p: PaneSpec): boolean {
  return p.el.style.display !== "none" && !p.el.classList.contains("shut");
}

function relayout(ctx: Ctx) {
  // 있던 나누개를 걷어낸다. 매번 새로 놓는 게 어긋나는 것보다 싸다.
  ctx.box.querySelectorAll(":scope > .gutter").forEach((g) => g.remove());

  // 보이는 칸만 골라 그 사이에 넣는다. 접힌 칸 옆에 나누개가 있으면
  // 잡아 끌어도 움직일 게 없어서 고장 난 것처럼 보인다.
  const on = ctx.panes.filter(visible);

  // ★ 몫을 다시 고르게 편다.
  //
  // CSS 규칙이 이렇다 — 남는 자리를 나눠 가질 칸들의 몫을 **다 더해서 1보다
  // 작으면, 그 비율만큼만 가져가고 나머지는 빈 채로 남는다.**
  //
  // 실제로 그 일이 났다. 위아래로 놓고 나누개를 끌어 영상·지도 쪽을 넓히면
  // 데이터의 몫이 0.35 쯤으로 떨어진다. 그 상태에서 영상·지도를 접으면
  // 데이터 혼자 남는데, 몫이 0.35 라서 남은 자리의 35% 만 쓰고 아래가
  // 검게 비었다.
  //
  // 그래서 접거나 펼 때마다 보이는 칸들의 몫을 다시 편다. 서로의 비율은
  // 그대로 두고 합만 칸 수에 맞춘다 (평균 1). 합이 1 밑으로 안 내려간다.
  const flexOn = on.filter((p) => p.kind === "flex");
  if (flexOn.length) {
    const gs = flexOn.map((p) => parseFloat(getComputedStyle(p.el).flexGrow) || 1);
    const sum = gs.reduce((a, b) => a + b, 0) || flexOn.length;
    flexOn.forEach((p, i) => {
      const gv = (gs[i] / sum) * flexOn.length;
      p.el.style.flex = `${gv} 1 0`;
      saved[p.el.id] = gv;
    });
  }
  for (let i = 0; i + 1 < on.length; i++) {
    const g = document.createElement("div");
    g.className = `gutter ${ctx.dir}`;
    g.style.flex = `0 0 ${GUTTER}px`;
    // 두 칸 사이에 끼워 넣는다
    on[i + 1].el.parentElement?.insertBefore(g, on[i + 1].el);
    arm(g, ctx, on[i], on[i + 1]);
  }
}

function arm(g: HTMLElement, ctx: Ctx, a: PaneSpec, b: PaneSpec) {
  let drag: {
    at: number; aSize: number; bSize: number; aGrow: number; bGrow: number;
  } | null = null;

  const pos = (e: PointerEvent) => (ctx.dir === "row" ? e.clientX : e.clientY);
  const size = (el: HTMLElement) =>
    ctx.dir === "row" ? el.getBoundingClientRect().width
                      : el.getBoundingClientRect().height;
  const grow = (p: PaneSpec) => parseFloat(getComputedStyle(p.el).flexGrow) || 1;

  g.addEventListener("pointerdown", (e) => {
    const ev = e as PointerEvent;
    drag = {
      at: pos(ev),
      aSize: size(a.el), bSize: size(b.el),
      aGrow: grow(a), bGrow: grow(b),
    };
    g.setPointerCapture(ev.pointerId);
    g.classList.add("on");
    ev.preventDefault();
  });

  g.addEventListener("pointermove", (e) => {
    if (!drag) return;
    const d = pos(e as PointerEvent) - drag.at;

    let aNew = drag.aSize + d;
    let bNew = drag.bSize - d;
    // 너무 좁아지면 더 안 준다. 칸이 사라져 버리면 되돌릴 수가 없다.
    if (aNew < a.min) { bNew -= a.min - aNew; aNew = a.min; }
    if (bNew < b.min) { aNew -= b.min - bNew; bNew = b.min; }
    if (aNew < a.min || bNew < b.min) return;

    apply(a, aNew, drag.aGrow + drag.bGrow, drag.aSize + drag.bSize);
    apply(b, bNew, drag.aGrow + drag.bGrow, drag.aSize + drag.bSize);
    ctx.onResize?.();
  });

  const end = () => {
    if (!drag) return;
    drag = null;
    g.classList.remove("on");
    save();
    ctx.onResize?.();
  };
  g.addEventListener("pointerup", end);
  g.addEventListener("pointercancel", end);
  // 두 번 누르면 두 칸을 반반으로 되돌린다
  g.addEventListener("dblclick", () => {
    if (a.kind === "flex" && b.kind === "flex") {
      const both = grow(a) + grow(b);
      a.el.style.flex = `${both / 2} 1 0`;
      b.el.style.flex = `${both / 2} 1 0`;
      saved[a.el.id] = both / 2;
      saved[b.el.id] = both / 2;
      save();
      ctx.onResize?.();
    }
  });
}

/**
 * 새 크기를 넣는다.
 *
 * 정해진 크기(fixed)는 px 로 그대로 박는다.
 * 나눠 갖는 칸(flex)은 **몫**으로 바꿔서 넣는다. px 로 박아 두면 창 크기를
 * 바꿨을 때 칸이 따라 늘지 않는다.
 */
function apply(p: PaneSpec, px: number, growSum: number, sizeSum: number) {
  if (p.kind === "fixed") {
    p.el.style.flex = `0 0 ${Math.round(px)}px`;
    saved[p.el.id] = Math.round(px);
  } else {
    const gv = (px / sizeSum) * growSum;
    p.el.style.flex = `${gv} 1 0`;
    saved[p.el.id] = gv;
  }
}
