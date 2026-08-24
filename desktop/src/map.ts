// 항적 지도.
//
// gpx.studio 를 따라 만들었다 (MIT, github.com/gpxstudio/gpx.studio).
// 거기서 가져온 것은 두 가지다.
//
//   1) 지도 그리는 부품이 MapLibre GL JS 라는 것
//      [확인: gpx.studio/website/package.json 의 "maplibre-gl": "^6.0.0"]
//   2) 바탕 지도를 어떻게 적어 두는지
//      [확인: gpx.studio/website/src/lib/assets/layers.ts:63-74]
//
// 우리는 배라서 바다 정보를 하나 더 얹는다 — OpenSeaMap 의 해도 기호다.
// 부표, 등대, 수심 같은 것이 나온다. gpx.studio 에는 없다 (자전거·등산용이라).
//
// ── 타일 서버를 함부로 쓰면 안 된다 ──
//
// OpenStreetMap 의 타일 서버는 자원 봉사로 굴러간다. 규칙이 있다.
// [확인: https://operations.osmfoundation.org/policies/tiles/]
//
//   - 사람이 지금 보고 있는 자리만 받는다. 미리 받아 두는 것(offline)은 금지.
//   - 우리 앱 이름을 User-Agent 에 밝혀야 한다. 안 밝히면 막힌다.
//   - 캐시를 끄면 안 된다.
//
// 지금 우리가 하는 일은 "사람이 보는 자리만 받기" 라 규칙 안에 있다.
// 나중에 "지도 미리 받아 두기" 를 넣고 싶으면 다른 타일 서버를 써야 한다.

// MapLibre 6 은 기본 내보내기가 없다. 이름으로 가져온다.
// [확인: node_modules/maplibre-gl/dist/maplibre-gl.d.ts 의 export { … } 목록]
import {
  Map as MlMap, NavigationControl, ScaleControl,
  type LngLatBoundsLike,
} from "maplibre-gl";
import "maplibre-gl/dist/maplibre-gl.css";

/** 항적의 한 점. 타임라인과 같은 시각(ms)을 들고 다닌다. */
export interface TrackPoint {
  ms: number;
  lat: number;
  lon: number;
  sogKn: number | null;
}

interface Base {
  id: string;
  label: string;
  tiles: string[];
  maxzoom: number;
  attribution: string;
}

// 바탕 지도. 열쇠(API key) 없이 되는 것만 골랐다.
// gpx.studio 는 MapTiler 도 쓰는데 그건 열쇠를 받아야 한다.
const BASES: Base[] = [
  {
    id: "osm",
    label: "일반 지도",
    // ★ a./b./c. 로 나누지 않는다. 규칙이 이 한 주소를 쓰라고 한다.
    tiles: ["https://tile.openstreetmap.org/{z}/{x}/{y}.png"],
    maxzoom: 19,
    attribution:
      '&copy; <a href="https://www.openstreetmap.org/copyright" target="_blank">OpenStreetMap</a>',
  },
  {
    id: "topo",
    label: "지형 지도",
    tiles: ["https://tile.opentopomap.org/{z}/{x}/{y}.png"],
    maxzoom: 17,
    attribution:
      '&copy; <a href="https://opentopomap.org" target="_blank">OpenTopoMap</a> (CC-BY-SA)',
  },
  {
    id: "sat",
    label: "위성 사진",
    tiles: [
      "https://services.arcgisonline.com/arcgis/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
    ],
    maxzoom: 19,
    attribution: "&copy; Esri, Maxar, Earthstar Geographics",
  },
];

const SEAMARK = "https://tiles.openseamap.org/seamark/{z}/{x}/{y}.png";

// 속도를 색으로. 느리면 파랑, 빠르면 빨강.
// 요트 계기가 쓰는 방식이다 — 어디서 배가 살아났는지 한눈에 보인다.
const SOG_RAMP: [number, string][] = [
  [0, "#2b6cb0"], [0.25, "#2f855a"], [0.5, "#d69e2e"],
  [0.75, "#dd6b20"], [1, "#c53030"],
];

const EMPTY_LINE = {
  type: "FeatureCollection" as const,
  features: [] as any[],
};

export class TrackMap {
  private map: MlMap | null = null;
  private pts: TrackPoint[] = [];
  private baseId = "osm";
  private seamarkOn = false;
  private colorBySog = true;
  private ready = false;
  private pending: (() => void)[] = [];

  /** 지도 위에서 어느 시각을 가리키고 있나. 타임라인이 따라오게 하려고 알린다. */
  onHover: ((ms: number | null) => void) | null = null;
  onPick: ((ms: number) => void) | null = null;

  constructor(private el: HTMLElement) {}

  /**
   * 지도를 띄운다. **처음 필요할 때까지 미룬다.**
   *
   * 지도를 안 보는 사람도 있는데 타일을 받아 오는 건 낭비다. 남의 서버를
   * 쓰는 일이라 더 그렇다.
   */
  start() {
    if (this.map) return;
    const m = this.map = new MlMap({
      container: this.el,
      style: this.style(),
      center: [126.55, 37.45],   // 인천 앞바다. 첫 항적이 오면 곧 옮겨간다
      zoom: 9,
      attributionControl: { compact: false },
    });
    m.addControl(new NavigationControl({ showCompass: true }), "top-left");
    m.addControl(new ScaleControl({ unit: "nautical" }), "bottom-left");

    // ★ "load" 가 아니라 "style.load" 를 쓴다.
    //
    //   "load"       는 스타일을 읽고 **첫 그림까지 그린 뒤** 온다.
    //   "style.load" 는 스타일만 읽으면 온다.
    //   [확인: node_modules/maplibre-gl/dist/maplibre-gl.d.ts:8858]
    //
    // 창이 가려져 있으면 브라우저가 화면 갱신을 멈춘다. 그러면 "load" 가
    // 영영 안 오고 항적도 안 올라간다. 실제로 그 일이 났다.
    m.on("style.load", () => {
      this.ready = true;
      this.addTrackLayers();
      this.pending.forEach((f) => f());
      this.pending = [];
    });

    // 항적 위에 마우스를 올리면 그 시각을 알린다
    m.on("mousemove", "trackHit", (e) => {
      const ms = e.features?.[0]?.properties?.ms;
      if (typeof ms === "number") this.onHover?.(ms);
      m.getCanvas().style.cursor = "pointer";
    });
    m.on("mouseleave", "trackHit", () => {
      this.onHover?.(null);
      m.getCanvas().style.cursor = "";
    });
    m.on("click", "trackHit", (e) => {
      const ms = e.features?.[0]?.properties?.ms;
      if (typeof ms === "number") this.onPick?.(ms);
    });
  }

  private style(): any {
    const b = BASES.find((x) => x.id === this.baseId) ?? BASES[0];
    const sources: any = {
      base: {
        type: "raster", tiles: b.tiles, tileSize: 256,
        maxzoom: b.maxzoom, attribution: b.attribution,
      },
    };
    const layers: any[] = [{ id: "base", type: "raster", source: "base" }];

    if (this.seamarkOn) {
      sources.seamark = {
        type: "raster", tiles: [SEAMARK], tileSize: 256, maxzoom: 18,
        attribution:
          '&copy; <a href="https://www.openseamap.org" target="_blank">OpenSeaMap</a>',
      };
      layers.push({ id: "seamark", type: "raster", source: "seamark" });
    }
    return { version: 8, sources, layers };
  }

  /** 항적·배 표시를 지도에 올린다. 바탕을 갈아끼울 때마다 다시 부른다. */
  private addTrackLayers() {
    const m = this.map!;
    if (!m.getSource("track")) {
      m.addSource("track", { type: "geojson", data: EMPTY_LINE, lineMetrics: true });
      m.addSource("hit",   { type: "geojson", data: EMPTY_LINE });
      m.addSource("boat",  { type: "geojson", data: EMPTY_LINE });
    }
    // 밑에 굵고 어두운 줄을 하나 깔면 어떤 바탕 위에서도 항적이 보인다
    m.addLayer({
      id: "trackHalo", type: "line", source: "track",
      layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": "#000", "line-opacity": 0.45, "line-width": 7 },
    });
    m.addLayer({
      id: "trackLine", type: "line", source: "track",
      layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-width": 4, "line-color": "#38bdf8" },
    });
    // 마우스로 집기 좋으라고 두꺼운 투명 줄을 따로 깐다
    m.addLayer({
      id: "trackHit", type: "line", source: "hit",
      paint: { "line-width": 18, "line-opacity": 0 },
    });
    m.addLayer({
      id: "boatDot", type: "circle", source: "boat",
      paint: {
        "circle-radius": 7, "circle-color": "#f8fafc",
        "circle-stroke-color": "#0f172a", "circle-stroke-width": 3,
      },
    });
    this.paintTrack();
  }

  setBase(id: string) {
    if (id === this.baseId) return;
    this.baseId = id;
    this.reskin();
  }

  setSeamark(on: boolean) {
    if (on === this.seamarkOn) return;
    this.seamarkOn = on;
    this.reskin();
  }

  setColorBySog(on: boolean) {
    this.colorBySog = on;
    if (this.ready) this.paintTrack();
  }

  /** 바탕을 갈면 우리 줄도 같이 날아간다. 다시 깐다. */
  private reskin() {
    if (!this.map) return;
    this.ready = false;
    this.map.setStyle(this.style());
    this.map.once("style.load", () => {
      this.ready = true;
      this.addTrackLayers();
    });
  }

  /** 항적을 넣는다. 빈 배열이면 지운다. */
  setTrack(pts: TrackPoint[]) {
    this.pts = pts;
    const run = () => { this.paintTrack(); if (pts.length) this.fit(); };
    if (this.ready) run(); else this.pending.push(run);
  }

  private paintTrack() {
    const m = this.map;
    if (!m || !m.getSource("track")) return;

    const coords = this.pts.map((p) => [p.lon, p.lat]);
    const line = coords.length >= 2
      ? { type: "FeatureCollection", features: [
          { type: "Feature", geometry: { type: "LineString", coordinates: coords }, properties: {} },
        ] }
      : EMPTY_LINE;
    (m.getSource("track") as any).setData(line);

    // 집기용은 토막으로 나눈다. 토막마다 그 자리의 시각을 붙여 둔다.
    // 줄 하나로 두면 어디를 짚었는지 알 수가 없다.
    const step = Math.max(1, Math.floor(this.pts.length / 800));
    const hits: any[] = [];
    for (let i = 0; i + step < this.pts.length; i += step) {
      const a = this.pts[i], b = this.pts[i + step];
      hits.push({
        type: "Feature",
        geometry: { type: "LineString", coordinates: [[a.lon, a.lat], [b.lon, b.lat]] },
        properties: { ms: a.ms },
      });
    }
    (m.getSource("hit") as any).setData({ type: "FeatureCollection", features: hits });

    m.setPaintProperty("trackLine", "line-gradient", this.gradient());
  }

  /**
   * 속도로 색칠하기.
   *
   * MapLibre 의 line-gradient 는 줄을 따라 0~1 로 훑으면서 색을 준다.
   * 그래서 점마다의 속도를 "줄 길이의 몇 퍼센트 자리" 로 바꿔서 넣는다.
   * 이걸 쓰려면 소스에 lineMetrics: true 가 있어야 한다.
   */
  private gradient(): any {
    if (!this.colorBySog || this.pts.length < 2) {
      return ["interpolate", ["linear"], ["line-progress"], 0, "#38bdf8", 1, "#38bdf8"];
    }
    const sogs = this.pts.map((p) => p.sogKn).filter((v): v is number => v !== null);
    if (sogs.length < 2) {
      return ["interpolate", ["linear"], ["line-progress"], 0, "#38bdf8", 1, "#38bdf8"];
    }
    const lo = Math.min(...sogs), hi = Math.max(...sogs);
    const span = hi - lo || 1;

    // 줄을 따라 몇 군데만 찍는다. 점 3만 개를 다 넣으면 식이 터무니없이 커진다.
    const N = Math.min(120, this.pts.length);
    const stops: any[] = [];
    let last = -1;
    for (let k = 0; k < N; k++) {
      const t = k / (N - 1);
      const p = this.pts[Math.round(t * (this.pts.length - 1))];
      const v = p.sogKn === null ? lo : p.sogKn;
      const u = (v - lo) / span;
      // line-progress 는 반드시 커지기만 해야 한다. 같은 값이 겹치면 지도가 죽는다.
      const at = Math.min(1, Math.max(last + 1e-4, t));
      last = at;
      stops.push(at, rampColor(u));
    }
    return ["interpolate", ["linear"], ["line-progress"], ...stops];
  }

  /** 지금 배가 어디 있는지 점 하나. 시각이 없으면 지운다. */
  setBoat(ms: number | null) {
    const m = this.map;
    if (!m || !m.getSource("boat")) return;
    const p = ms === null ? null : this.at(ms);
    (m.getSource("boat") as any).setData(
      p ? { type: "FeatureCollection", features: [
            { type: "Feature", geometry: { type: "Point", coordinates: [p.lon, p.lat] },
              properties: {} }] }
        : EMPTY_LINE);
  }

  /** 그 시각에 배가 있던 자리. 사이 값은 두 점 사이를 갈라 쓴다. */
  at(ms: number): TrackPoint | null {
    const a = this.pts;
    if (!a.length) return null;
    if (ms <= a[0].ms) return a[0];
    if (ms >= a[a.length - 1].ms) return a[a.length - 1];
    let lo = 0, hi = a.length - 1;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (a[mid].ms <= ms) lo = mid; else hi = mid;
    }
    const t = (ms - a[lo].ms) / Math.max(1, a[hi].ms - a[lo].ms);
    return {
      ms,
      lat: a[lo].lat + (a[hi].lat - a[lo].lat) * t,
      lon: a[lo].lon + (a[hi].lon - a[lo].lon) * t,
      sogKn: a[lo].sogKn,
    };
  }

  /** 항적 전체가 보이게 맞춘다. */
  fit() {
    if (!this.map || this.pts.length === 0) return;
    let w = 180, s = 90, e = -180, n = -90;
    for (const p of this.pts) {
      if (p.lon < w) w = p.lon;
      if (p.lon > e) e = p.lon;
      if (p.lat < s) s = p.lat;
      if (p.lat > n) n = p.lat;
    }
    // 한 자리에 서 있었으면 넓이가 0 이다. 조금 벌려 준다.
    if (e - w < 1e-4) { w -= 5e-5; e += 5e-5; }
    if (n - s < 1e-4) { s -= 5e-5; n += 5e-5; }
    this.map.fitBounds([[w, s], [e, n]] as LngLatBoundsLike,
                       { padding: 40, duration: 400 });
  }

  resize() { this.map?.resize(); }

  /** 만드는 중에 안을 들여다보려고. */
  peek() {
    const m = this.map;
    if (!m) return { 지도: "안 떴음" };
    return {
      스타일읽음: m.isStyleLoaded(),
      항적점수: this.pts.length,
      항적줄: (m.getSource("track") as any)?._data?.features?.length ?? 0,
      집기토막: (m.getSource("hit") as any)?._data?.features?.length ?? 0,
      배: (m.getSource("boat") as any)?._data?.features?.length ?? 0,
      가운데: m.getCenter().toArray().map((v: number) => +v.toFixed(4)),
      배율: +m.getZoom().toFixed(2),
      바탕: this.baseId,
    };
  }

  static bases() { return BASES.map((b) => ({ id: b.id, label: b.label })); }
}

function rampColor(u: number): string {
  const x = Math.min(1, Math.max(0, u));
  for (let i = 1; i < SOG_RAMP.length; i++) {
    const [t1, c1] = SOG_RAMP[i];
    if (x <= t1) {
      const [t0, c0] = SOG_RAMP[i - 1];
      return mix(c0, c1, (x - t0) / (t1 - t0));
    }
  }
  return SOG_RAMP[SOG_RAMP.length - 1][1];
}

function mix(a: string, b: string, t: number): string {
  const p = (h: string) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
  const [r1, g1, b1] = p(a), [r2, g2, b2] = p(b);
  const q = (x: number, y: number) =>
    Math.round(x + (y - x) * t).toString(16).padStart(2, "0");
  return `#${q(r1, r2)}${q(g1, g2)}${q(b1, b2)}`;
}
