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
  Map as MlMap, Marker, NavigationControl, ScaleControl, setWorkerUrl, addProtocol,
  type LngLatBoundsLike,
} from "maplibre-gl";
import "maplibre-gl/dist/maplibre-gl.css";
import { layers as pmLayers, namedFlavor } from "@protomaps/basemaps";
import { invoke } from "@tauri-apps/api/core";

// ── 오프라인 지도 (2026-09-15) ────────────────────────────────────────────
//
// 사람이 고른 영역을 Source Cooperative 의 Protomaps 원본(PMTiles)에서 잘라 앱 데이터 폴더에 받아 둔다
// (src-tauri/src/offmap.rs). 지도는 그 파일에서 타일을 꺼내 그린다 — 인터넷을 안 쓴다.
//
//   타일      offmap://{z}/{x}/{y} → Rust offmap_tile (받은 영역 파일들 중 그 타일이 있는 곳, gzip 푼 MVT)
//   스타일    @protomaps/basemaps 5.7.2 layers() · 한국어 이름표 (lang "ko")
//   글꼴      public/basemaps-assets/fonts (Noto Sans 셋, OFL). 한글은 MapLibre 가 컴퓨터 글꼴로 그린다
//             [확인: maplibre-gl-shared-dev.mjs codePointUsesLocalIdeographFontFamily 에 U+AC00~D7C6]
//   아이콘    public/basemaps-assets/sprites/v4 (tangrams/icons, MIT)
//
// ★ 해도(OpenSeaMap)·위성(Esri)은 미리 받기를 허락하지 않아 오프라인이 안 된다. 해도를 켜면 인터넷이 필요하다.
addProtocol("offmap", async (params) => {
  const m = params.url.match(/^offmap:\/\/(\d+)\/(\d+)\/(\d+)/);
  if (!m) throw new Error(`오프라인 타일 주소가 이상합니다: ${params.url}`);
  const data = await invoke<ArrayBuffer>("offmap_tile", { z: +m[1], x: +m[2], y: +m[3] });
  return { data };
});

// ── 워커 주소를 우리가 직접 알려 준다 ────────────────────────────────────
//
// ★ 이게 없으면 **항적이 아예 안 그려진다.** 지도 타일은 멀쩡히 나온다.
//
// MapLibre 6 은 GeoJSON(우리 항적)을 워커에서 만든다. 타일은 워커를 안 거친다.
// 그래서 워커가 죽으면 "지도는 나오는데 선만 없는" 모양이 된다.
//
// 6 부터 워커가 **묶음과 따로 있는 파일**이 됐고, 그 자리를 import.meta.url
// 옆으로 짐작한다. 묶개(Vite)를 거치면 그 짐작이 틀린다. 실측한 것:
//
//   묶음이 부른 주소   /assets/maplibre-gl-worker.mjs
//   서버가 준 것       200 OK · text/html · index.html 내용
//
// 없는 주소라 index.html 이 대신 왔고, 워커가 그걸 자바스크립트로 읽다 죽었다.
// **조용히 죽는다** — MapLibre 는 new Worker() 가 던질 때만 물러서는데
// 이건 워커 안에서 나중에 실패하는 거라 안 던진다. 오류 한 줄 안 남는다.
// 개발 서버에서는 node_modules 의 진짜 파일을 찾아 줘서 멀쩡했다.
// 맥 앱은 화면을 tauri:// 로 띄우는데, 그때는 위 짐작이 빈 문자열이 된다.
//
// **`?url` 이 아니라 `?worker&url` 이어야 한다.** 워커 파일이 옆의
// maplibre-gl-shared.mjs 를 import 하는데, `?url` 은 그 옆 파일을 안 내보낸다.
// [확인: maplibre.org/maplibre-gl-js/docs 의 번들러별 설치 안내]
import mlWorkerUrl from "maplibre-gl/dist/maplibre-gl-worker.mjs?worker&url";
setWorkerUrl(mlWorkerUrl);

/** 눈금이 1해리로 나오는 배율.
 *
 *  눈금 막대는 100픽셀이 덮는 거리를 1852로 나눈 뒤 10·5·3·2·1 로 내림해서
 *  보여준다. [확인: maplibre-gl-dev.mjs 의 27896·27908·27922줄]
 *  그러니 100픽셀이 1852m 이상 3704m 미만을 덮으면 "1 nm" 가 된다.
 *
 *  위도 37.45(인천 앞바다) 기준으로 그 구간이 배율 10.7~11.7 이고,
 *  한가운데인 11.3 을 골랐다. 이때 100픽셀이 2,462m(1.33해리)를 덮는다.
 *  [확인: worldSize = tileSize x scale, tileSize = 512 — 같은 파일 9174줄]
 *
 *  전에는 9 였는데 나라가 다 보이는 배율이라 너무 멀었다. */
const NM1_ZOOM = 11.3;

/** 항적의 한 점. 타임라인과 같은 시각(ms)을 들고 다닌다. */
export interface TrackPoint {
  ms: number;
  lat: number;
  lon: number;
  sogKn: number | null;
}

/** 함대 라이브에서 그릴 배 한 척. select는 A/B 또는 미선택이다. */
export interface FleetPoint {
  boat: number;
  lat: number;
  lon: number;
  sogKn: number | null;
  cogDeg: number | null;
  headingDeg: number | null;
  headingTrue: boolean;
  select: "a" | "b" | null;
  stale: boolean;
}

/** 함대 라이브에서 사용자가 녹화를 켠 뒤 받은 위치. gapBefore면 직전 점까지 LoRa 공백이었다. */
export interface FleetTrailPoint {
  lat: number;
  lon: number;
  gapBefore: boolean;
}

export interface FleetTrail {
  boat: number;
  points: FleetTrailPoint[];
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
  {
    id: "offline",
    label: "오프라인 지도",
    tiles: [],          // style() 이 따로 만든다 (벡터)
    maxzoom: 15,
    attribution:
      '&copy; <a href="https://www.openstreetmap.org/copyright" target="_blank">OpenStreetMap</a> · ' +
      '<a href="https://protomaps.com" target="_blank">Protomaps</a>',
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
  /** 받은 오프라인 영역 중 가장 깊은 확대. 0 이면 받은 영역이 없다 */
  private offlineMax = 0;
  /** 오프라인 지도 색 — 앱 판이 밝으면 light, 아니면 dark */
  private dark = true;
  private ready = false;
  private pending: (() => void)[] = [];
  private fleet: FleetPoint[] = [];
  private fleetTrails: FleetTrail[] = [];
  private fleetMarkers = new Map<number, Marker>();

  /** 지도 위에서 어느 시각을 가리키고 있나. 타임라인이 따라오게 하려고 알린다. */
  onHover: ((ms: number | null) => void) | null = null;
  onPick: ((ms: number) => void) | null = null;
  onFleetPick: ((boat: number) => void) | null = null;

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

      zoom: NM1_ZOOM,
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
    // HTML 표식은 화면 기준으로 그려진다. 지도를 돌리면 진북 방위에서 현재
    // 지도 bearing을 빼야 같은 지리 방향을 계속 가리킨다.
    m.on("rotate", () => this.paintFleetHeadings());
    this.paintFleet();
  }

  private style(): any {
    const b = BASES.find((x) => x.id === this.baseId) ?? BASES[0];
    if (b.id === "offline") return this.offlineStyle(b);
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

  /** 받아 둔 영역으로 그리는 벡터 스타일. 글꼴·아이콘 주소는 앱 안(public)이다. */
  private offlineStyle(b: Base): any {
    const flavor = this.dark ? "dark" : "light";
    // ★ 절대 주소로 준다. 맥은 tauri://localhost, 윈도는 http://tauri.localhost, 개발은 http://localhost:1420
    const here = location.origin;
    const style: any = {
      version: 8,
      glyphs: `${here}/basemaps-assets/fonts/{fontstack}/{range}.pbf`,
      sprite: `${here}/basemaps-assets/sprites/v4/${flavor}`,
      sources: {
        protomaps: {
          type: "vector", tiles: ["offmap://{z}/{x}/{y}"],
          // 받은 영역보다 더 당기면 가장 깊은 타일을 늘려서 보여준다
          maxzoom: this.offlineMax || b.maxzoom,
          attribution: b.attribution,
        },
      },
      layers: pmLayers("protomaps", namedFlavor(flavor), { lang: "ko" }),
    };
    if (this.seamarkOn) {
      style.sources.seamark = {
        type: "raster", tiles: [SEAMARK], tileSize: 256, maxzoom: 18,
        attribution: '&copy; <a href="https://www.openseamap.org" target="_blank">OpenSeaMap</a>',
      };
      style.layers.push({ id: "seamark", type: "raster", source: "seamark" });
    }
    return style;
  }

  /** 받은 영역이 바뀌었을 때. 오프라인 바탕을 보고 있으면 다시 그린다. */
  setOffline(maxzoom: number) {
    if (maxzoom === this.offlineMax) return;
    this.offlineMax = maxzoom;
    if (this.baseId === "offline") this.reskin();
  }

  setDark(dark: boolean) {
    if (dark === this.dark) return;
    this.dark = dark;
    if (this.baseId === "offline") this.reskin();
  }

  /** 영역 [서, 남, 동, 북] 이 보이게 맞춘다 */
  fitBounds(bb: [number, number, number, number]) {
    this.map?.fitBounds([[bb[0], bb[1]], [bb[2], bb[3]]], { padding: 20, duration: 400 });
  }

  /** 지금 보이는 영역 [서, 남, 동, 북]. 지도가 안 떴으면 null */
  viewBounds(): [number, number, number, number] | null {
    const g = this.map?.getBounds();
    return g ? [g.getWest(), g.getSouth(), g.getEast(), g.getNorth()] : null;
  }

  /** 항적·배 표시를 지도에 올린다. 바탕을 갈아끼울 때마다 다시 부른다. */
  private addTrackLayers() {
    const m = this.map!;
    if (!m.getSource("track")) {
      m.addSource("track", { type: "geojson", data: EMPTY_LINE, lineMetrics: true });
      m.addSource("hit",   { type: "geojson", data: EMPTY_LINE });
      m.addSource("boat",  { type: "geojson", data: EMPTY_LINE });
      m.addSource("fleetTrails", { type: "geojson", data: EMPTY_LINE });
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
    // 함대 실시간 궤적. 정상 구간은 배 색 실선, 패킷 공백을 건넌 구간은
    // 위치를 지어내지 않고 두 실측점을 주황 점선으로 곧게 잇는다.
    m.addLayer({
      id: "fleetTrailHalo", type: "line", source: "fleetTrails",
      layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": "#000", "line-opacity": 0.48, "line-width": 7 },
    });
    m.addLayer({
      id: "fleetTrailNormal", type: "line", source: "fleetTrails",
      filter: ["==", ["get", "gap"], false],
      layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": ["get", "color"], "line-width": 4 },
    });
    m.addLayer({
      id: "fleetTrailGap", type: "line", source: "fleetTrails",
      filter: ["==", ["get", "gap"], true],
      layout: { "line-cap": "round", "line-join": "round" },
      paint: { "line-color": "#f59e0b", "line-width": 4, "line-dasharray": [1.5, 1.5] },
    });
    this.paintTrack();
    this.paintFleetTrails();
  }

  setBase(id: string) {
    if (id === this.baseId) return;
    this.baseId = id;
    this.reskin();
  }

  /** 네트워크가 돌아온 뒤 실패했던 온라인 타일을 다시 요청한다. */
  reloadBase() {
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

  /** 함대의 최신 위치를 번호·진행 방향과 함께 그린다. */
  setFleet(points: FleetPoint[]) {
    this.fleet = points;
    this.paintFleet();
  }

  setFleetTrails(trails: FleetTrail[]) {
    this.fleetTrails = trails;
    this.paintFleetTrails();
  }

  private paintFleetTrails() {
    const m = this.map;
    if (!m || !m.getSource("fleetTrails")) return;
    const colors = ["#38bdf8", "#fb923c", "#34d399", "#c084fc", "#f472b6", "#facc15"];
    const features: any[] = [];
    for (const trail of this.fleetTrails) {
      const color = colors[(trail.boat - 1) % colors.length];
      for (let i = 1; i < trail.points.length; i++) {
        const a = trail.points[i - 1], b = trail.points[i];
        features.push({
          type: "Feature",
          geometry: { type: "LineString", coordinates: [[a.lon, a.lat], [b.lon, b.lat]] },
          properties: { boat: trail.boat, color, gap: b.gapBefore },
        });
      }
    }
    (m.getSource("fleetTrails") as any).setData({ type: "FeatureCollection", features });
  }

  private paintFleet() {
    if (!this.map) return;
    const alive = new Set(this.fleet.map((p) => p.boat));
    for (const [boat, marker] of this.fleetMarkers) {
      if (!alive.has(boat)) { marker.remove(); this.fleetMarkers.delete(boat); }
    }
    for (const p of this.fleet) {
      let marker = this.fleetMarkers.get(p.boat);
      if (!marker) {
        const el = document.createElement("button");
        el.type = "button";
        el.className = "fleet-marker";
        el.innerHTML = `<span class="fleet-hull" aria-hidden="true"><svg viewBox="0 0 30 56"><path d="M15 2 28 48Q15 56 2 48Z"/></svg></span><span class="fleet-no-heading" aria-hidden="true"></span><b>${p.boat}</b><span class="fleet-marker-data"></span>`;
        el.title = `${p.boat}번 배 선택`;
        el.onclick = () => this.onFleetPick?.(p.boat);
        marker = new Marker({ element: el, anchor: "center" }).setLngLat([p.lon, p.lat]).addTo(this.map);
        this.fleetMarkers.set(p.boat, marker);
      }
      marker.setLngLat([p.lon, p.lat]);
      const el = marker.getElement();
      const canPoint = p.headingDeg !== null && p.headingTrue;
      el.className = `fleet-marker${p.select ? ` pick-${p.select}` : ""}${p.stale ? " stale" : ""}${canPoint ? "" : " no-heading"}`;
      if (canPoint) el.dataset.mapHeading = String(p.headingDeg);
      else delete el.dataset.mapHeading;
      const data = el.querySelector<HTMLElement>(".fleet-marker-data");
      if (data) {
        const sog = p.sogKn === null ? "SOG —" : `${p.sogKn.toFixed(2)} kn`;
        const hdg = p.headingDeg === null ? "HDG —" : `HDG ${Math.round(p.headingDeg).toString().padStart(3, "0")}°${p.headingTrue ? "T" : "M"}`;
        data.textContent = `${sog}  ${hdg}`;
      }
      el.title = `${p.boat}번 배 · ${data?.textContent ?? ""}${canPoint ? " · 선수 방향 HDG(T)" : ""}`;
    }
    this.paintFleetHeadings();
  }

  private paintFleetHeadings() {
    if (!this.map) return;
    const mapBearing = this.map.getBearing();
    for (const marker of this.fleetMarkers.values()) {
      const el = marker.getElement();
      const raw = el.dataset.mapHeading;
      const hull = el.querySelector<HTMLElement>(".fleet-hull");
      if (!hull || raw === undefined) continue;
      const headingTrue = Number(raw);
      if (!Number.isFinite(headingTrue)) continue;
      // SVG의 뾰족한 끝은 화면 위쪽 0°다. HDG T에서 지도 회전을 빼면
      // 북쪽 고정 지도와 회전한 지도 모두 같은 실제 선수 방향을 가리킨다.
      hull.style.transform = `rotate(${headingTrue - mapBearing}deg)`;
    }
  }

  fitFleet() {
    if (!this.map) return;
    const pts = this.fleet.filter((p) => Number.isFinite(p.lat) && Number.isFinite(p.lon));
    if (!pts.length) return;
    let w = pts[0].lon, e = pts[0].lon, s = pts[0].lat, n = pts[0].lat;
    for (const p of pts) { w = Math.min(w, p.lon); e = Math.max(e, p.lon); s = Math.min(s, p.lat); n = Math.max(n, p.lat); }
    const midLat = (s + n) / 2;
    const minLat = 500 / 111320;
    const minLon = 500 / (111320 * Math.cos(midLat * Math.PI / 180));
    if (n - s < minLat) { const d = (minLat - n + s) / 2; s -= d; n += d; }
    if (e - w < minLon) { const d = (minLon - e + w) / 2; w -= d; e += d; }
    this.map.fitBounds([[w, s], [e, n]], { padding: 60, duration: 400 });
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
    // ── 짧은 항적도 보이게 벌린다 ──
    //
    // 전에는 `maxZoom: NM1_ZOOM` 으로 1해리보다 더 당기지 못하게 막았다.
    // 이유는 "너무 당기면 주변이 안 보여 어디서 탄 건지 모른다" 였는데,
    // 반대쪽으로 너무 갔다. **실측**:
    //
    //   2026-08-30 세션 27 의 항적   488 m x 449 m
    //   1해리 배율에서 지도 칸이 덮는 거리   약 15,000 m
    //   항적이 차지하는 넓이            가로의 3%     ← 점으로 보인다
    //
    // 그래서 배율을 막는 대신 **넓이의 아래쪽만 잡는다.** 항적이 이보다
    // 작아도 이만큼은 담는다. 항적이 이보다 크면 항적에 그대로 맞춘다.
    // 한 자리에 떠 있던 세션도 300 m 짜리 그림이 되지 0.2해리로 안 당겨진다.
    const MIN_SPAN_M = 300;
    const midLat = (s + n) / 2;
    const minLatSpan = MIN_SPAN_M / 111320;
    const minLonSpan = MIN_SPAN_M / (111320 * Math.cos(midLat * Math.PI / 180));
    if (n - s < minLatSpan) {
      const pad = (minLatSpan - (n - s)) / 2; s -= pad; n += pad;
    }
    if (e - w < minLonSpan) {
      const pad = (minLonSpan - (e - w)) / 2; w -= pad; e += pad;
    }
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
