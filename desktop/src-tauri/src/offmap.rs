// 오프라인 지도 — 고른 영역만 받아 앱 데이터 폴더에 둔다.
//
// 원본은 Protomaps 가 OpenStreetMap 으로 만든 세계 지도 한 파일(PMTiles v3, 134 GB)이다.
// 통째로는 못 받는다. 사람이 고른 네모(bbox)와 확대 한도(maxzoom)에 드는 타일만 떼어
// 작은 PMTiles 파일로 새로 쓴다.
//
// ── 어떻게 떼어 내나 ─────────────────────────────────────────────────────────
//
// go-pmtiles 의 `pmtiles extract` 를 그대로 옮겼다 (v1.31.2 pmtiles/extract.go).
// [확인: 그 소스를 받아 읽음 — RelevantEntries · reencodeEntries · mergeRanges · Extract]
//
//   1. 머리글(127바이트) → 뿌리 목록(root directory) → 네모에 걸리는 잎 목록(leaf directory)
//   2. 네모에 드는 타일 줄만 남긴다. 한 줄이 여러 타일을 가리키면(run length) 네모 안쪽만 남긴다
//   3. 같은 내용(원본 offset 같음)은 한 번만 저장한다 — 바다 타일이 수십만 번 겹친다
//   4. 가까운 조각은 묶어서 한 번에 받는다 (overfetch 5% 까지 버리는 바이트를 허용)
//   5. 목록·메타데이터·타일을 쓰고, **머리글은 맨 마지막에** 쓴다
//      (중간에 끊긴 파일이 멀쩡한 파일로 보이지 않게 — Extract 의 주석과 같은 이유)
//
// 규격 원문: github.com/protomaps/PMTiles/blob/main/spec/v3/spec.md
//
// ── 타일 번호 ────────────────────────────────────────────────────────────────
//
// 네모에 드는 타일은 **확대 한도에서** 네모가 걸친 x·y 칸 전부이고, 그보다 낮은 확대에서는
// 그 부모들이다 (extract 의 bitmapMultiPolygon + generalizeOr 와 같은 뜻).
// 번호는 확대 0 부터 이어지는 힐베르트 곡선 번호다 (tile_id.go ZxyToID).

use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};

/// 원본. 주소가 고정이고 조각 받기(Range)가 된다.
/// [확인: curl -I — 134,812,420,554 바이트, accept-ranges: bytes, 2026-09-15]
pub const SOURCE_URL: &str = "https://data.source.coop/protomaps/openstreetmap/v4.pmtiles";

const HEADER_LEN: usize = 127;
/// 묶어 받을 때 버려도 되는 바이트 비율. pmtiles CLI 기본값과 같다.
const OVERFETCH: f64 = 0.05;
/// 한 요청의 최대 크기. 묶다가 이보다 커지면 더 안 묶는다 (CLI 에는 없는 한도 — 끊겼을 때 다시 받는 양을 줄이려고).
const MAX_REQUEST: u64 = 16 * 1024 * 1024;
/// 한 요청이 실패하면 다시 해 보는 횟수.
const RETRIES: u32 = 3;
/// 동시에 받는 요청 수. CLI 기본값과 같다.
const WORKERS: usize = 4;
const UA: &str = "SailAnalyzer/0.1 (+https://github.com/fetmkr/SailingBLEWatchMonitor)";

// ── 밖으로 내보내는 모양 ─────────────────────────────────────────────────────

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct Region {
    pub id: String,
    pub name: String,
    /// [서, 남, 동, 북] 도
    pub bbox: [f64; 4],
    pub maxzoom: u8,
    /// 파일 크기 (바이트)
    pub bytes: u64,
    /// 가리키는 타일 수 (같은 내용이 여러 번 가리켜도 각각 센다)
    pub tiles: u64,
    /// 받은 시각 (UNIX 초)
    pub created: u64,
    pub source_url: String,
    /// 원본이 담은 OSM 자료 시각. 원본 메타데이터 planetiler:osm:osmosisreplicationtime. 없으면 ""
    pub source_osm_time: String,
}

#[derive(Serialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct Estimate {
    /// 가리키는 타일 수
    pub tiles: u64,
    /// 받을 타일 본문 크기 (같은 내용은 한 번). 목록·메타데이터는 뺀 값 — CLI dry-run 의 "archive size" 와 같은 뜻
    pub bytes: u64,
    /// 원본에 보낼 요청 수 (머리글·뿌리·잎·메타데이터·타일 묶음)
    pub requests: u64,
}

#[derive(Serialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct Progress {
    pub id: String,
    /// "dir" | "tiles" | "write" | "done" | "error" | "cancelled"
    pub phase: &'static str,
    pub done_bytes: u64,
    pub total_bytes: u64,
    pub done_tiles: u64,
    pub total_tiles: u64,
    pub error: Option<String>,
}

// ── 힐베르트 타일 번호 (tile_id.go 그대로) ───────────────────────────────────

// ★ x·y 는 칸 안의 값이 아니라 전체 좌표라 n−1−x 가 음수로 넘어간다. Go 는 uint32 로 감아 넘기고,
//   다음 자리에서 s & x 로 아래 비트만 보므로 결과가 맞다. 여기서도 감아 넘긴다 (wrapping_sub).
fn rotate(n: u64, x: u64, y: u64, rx: u64, ry: u64) -> (u64, u64) {
    if ry == 0 {
        if rx != 0 {
            return (n.wrapping_sub(1).wrapping_sub(y), n.wrapping_sub(1).wrapping_sub(x));
        }
        return (y, x);
    }
    (x, y)
}

pub fn zxy_to_id(z: u8, x: u32, y: u32) -> u64 {
    let mut acc: u64 = ((1u64 << (z as u64 * 2)) - 1) / 3;
    if z == 0 {
        return acc;
    }
    let (mut x, mut y) = (x as u64, y as u64);
    let mut n = z as u64 - 1;
    let mut s: u64 = 1 << n;
    while s > 0 {
        let rx = s & x;
        let ry = s & y;
        acc += ((3 * rx) ^ ry) << n;
        let r = rotate(s, x, y, rx, ry);
        x = r.0;
        y = r.1;
        s >>= 1;
        n = n.wrapping_sub(1);
    }
    acc
}

#[allow(dead_code)] // 시험에서 번호 ↔ 좌표가 서로 되돌아오는지 본다
pub fn id_to_zxy(i: u64) -> (u8, u32, u32) {
    let z = ((64 - (3 * i + 1).leading_zeros()) - 1) / 2;
    let acc = ((1u64 << (z as u64 * 2)) - 1) / 3;
    let mut t = i - acc;
    let (mut tx, mut ty) = (0u64, 0u64);
    for a in 0..z {
        let s = 1u64 << a;
        let rx = 1 & (t >> 1);
        let ry = 1 & (t ^ rx);
        let r = rotate(s, tx, ty, rx, ry);
        tx = r.0 + (rx << a);
        ty = r.1 + (ry << a);
        t >>= 2;
    }
    (z as u8, tx as u32, ty as u32)
}

// ── 머리글·목록 (directory.go 그대로) ────────────────────────────────────────

#[derive(Clone, Debug)]
struct Header {
    root_offset: u64,
    root_length: u64,
    metadata_offset: u64,
    metadata_length: u64,
    leaf_offset: u64,
    leaf_length: u64,
    tile_data_offset: u64,
    tile_data_length: u64,
    addressed_tiles: u64,
    tile_entries: u64,
    tile_contents: u64,
    clustered: bool,
    internal_compression: u8,
    tile_compression: u8,
    tile_type: u8,
    min_zoom: u8,
    max_zoom: u8,
    min_lon_e7: i32,
    min_lat_e7: i32,
    max_lon_e7: i32,
    max_lat_e7: i32,
    center_zoom: u8,
    center_lon_e7: i32,
    center_lat_e7: i32,
}

const GZIP: u8 = 2;
const NONE: u8 = 1;

fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
fn i32le(b: &[u8], o: usize) -> i32 {
    i32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

fn parse_header(b: &[u8]) -> Result<Header, String> {
    if b.len() < HEADER_LEN || &b[0..7] != b"PMTiles" {
        return Err("PMTiles 파일이 아닙니다 (머리 7바이트가 PMTiles 가 아님)".into());
    }
    if b[7] != 3 {
        return Err(format!("PMTiles 판이 {} 입니다. 3 만 읽습니다", b[7]));
    }
    Ok(Header {
        root_offset: u64le(b, 8),
        root_length: u64le(b, 16),
        metadata_offset: u64le(b, 24),
        metadata_length: u64le(b, 32),
        leaf_offset: u64le(b, 40),
        leaf_length: u64le(b, 48),
        tile_data_offset: u64le(b, 56),
        tile_data_length: u64le(b, 64),
        addressed_tiles: u64le(b, 72),
        tile_entries: u64le(b, 80),
        tile_contents: u64le(b, 88),
        clustered: b[96] == 1,
        internal_compression: b[97],
        tile_compression: b[98],
        tile_type: b[99],
        min_zoom: b[100],
        max_zoom: b[101],
        min_lon_e7: i32le(b, 102),
        min_lat_e7: i32le(b, 106),
        max_lon_e7: i32le(b, 110),
        max_lat_e7: i32le(b, 114),
        center_zoom: b[118],
        center_lon_e7: i32le(b, 119),
        center_lat_e7: i32le(b, 123),
    })
}

fn serialize_header(h: &Header) -> Vec<u8> {
    let mut b = vec![0u8; HEADER_LEN];
    b[0..7].copy_from_slice(b"PMTiles");
    b[7] = 3;
    let mut put64 = |o: usize, v: u64| b[o..o + 8].copy_from_slice(&v.to_le_bytes());
    put64(8, h.root_offset);
    put64(16, h.root_length);
    put64(24, h.metadata_offset);
    put64(32, h.metadata_length);
    put64(40, h.leaf_offset);
    put64(48, h.leaf_length);
    put64(56, h.tile_data_offset);
    put64(64, h.tile_data_length);
    put64(72, h.addressed_tiles);
    put64(80, h.tile_entries);
    put64(88, h.tile_contents);
    b[96] = h.clustered as u8;
    b[97] = h.internal_compression;
    b[98] = h.tile_compression;
    b[99] = h.tile_type;
    b[100] = h.min_zoom;
    b[101] = h.max_zoom;
    b[102..106].copy_from_slice(&h.min_lon_e7.to_le_bytes());
    b[106..110].copy_from_slice(&h.min_lat_e7.to_le_bytes());
    b[110..114].copy_from_slice(&h.max_lon_e7.to_le_bytes());
    b[114..118].copy_from_slice(&h.max_lat_e7.to_le_bytes());
    b[118] = h.center_zoom;
    b[119..123].copy_from_slice(&h.center_lon_e7.to_le_bytes());
    b[123..127].copy_from_slice(&h.center_lat_e7.to_le_bytes());
    b
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Entry {
    tile_id: u64,
    offset: u64,
    length: u32,
    run_length: u32,
}

fn decompress(data: &[u8], compression: u8) -> Result<Vec<u8>, String> {
    match compression {
        NONE => Ok(data.to_vec()),
        GZIP => {
            let mut out = Vec::new();
            flate2::read::GzDecoder::new(data)
                .read_to_end(&mut out)
                .map_err(|e| format!("gzip 을 못 풀었습니다 — {e}"))?;
            Ok(out)
        }
        c => Err(format!("모르는 압축 방식 {c} 입니다")),
    }
}

fn read_varint(b: &[u8], pos: &mut usize) -> Result<u64, String> {
    let mut v: u64 = 0;
    let mut shift = 0;
    loop {
        let byte = *b.get(*pos).ok_or("목록이 중간에 끝났습니다 (varint)")?;
        *pos += 1;
        v |= ((byte & 0x7f) as u64) << shift;
        if byte & 0x80 == 0 {
            return Ok(v);
        }
        shift += 7;
        if shift > 63 {
            return Err("목록의 varint 가 너무 깁니다".into());
        }
    }
}

fn write_varint(out: &mut Vec<u8>, mut v: u64) {
    while v >= 0x80 {
        out.push((v as u8) | 0x80);
        v >>= 7;
    }
    out.push(v as u8);
}

fn deserialize_entries(data: &[u8], compression: u8) -> Result<Vec<Entry>, String> {
    let b = decompress(data, compression)?;
    let mut p = 0usize;
    let n = read_varint(&b, &mut p)? as usize;
    let mut e = Vec::with_capacity(n);
    let mut last = 0u64;
    for _ in 0..n {
        last += read_varint(&b, &mut p)?;
        e.push(Entry { tile_id: last, offset: 0, length: 0, run_length: 0 });
    }
    for x in e.iter_mut() {
        x.run_length = read_varint(&b, &mut p)? as u32;
    }
    for x in e.iter_mut() {
        x.length = read_varint(&b, &mut p)? as u32;
    }
    for i in 0..n {
        let v = read_varint(&b, &mut p)?;
        e[i].offset = if i > 0 && v == 0 { e[i - 1].offset + e[i - 1].length as u64 } else { v - 1 };
    }
    Ok(e)
}

fn serialize_entries(entries: &[Entry]) -> Vec<u8> {
    let mut raw = Vec::new();
    write_varint(&mut raw, entries.len() as u64);
    let mut last = 0u64;
    for x in entries {
        write_varint(&mut raw, x.tile_id - last);
        last = x.tile_id;
    }
    for x in entries {
        write_varint(&mut raw, x.run_length as u64);
    }
    for x in entries {
        write_varint(&mut raw, x.length as u64);
    }
    for (i, x) in entries.iter().enumerate() {
        if i > 0 && x.offset == entries[i - 1].offset + entries[i - 1].length as u64 {
            write_varint(&mut raw, 0);
        } else {
            write_varint(&mut raw, x.offset + 1);
        }
    }
    // 목록은 gzip 으로 쓴다 (CLI 도 Gzip · BestCompression)
    let mut enc = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::best());
    enc.write_all(&raw).expect("메모리에 쓰기");
    enc.finish().expect("메모리에 쓰기")
}

fn build_roots_leaves(entries: &[Entry], leaf_size: usize) -> (Vec<u8>, Vec<u8>) {
    let mut roots = Vec::new();
    let mut leaves = Vec::new();
    let mut idx = 0;
    while idx < entries.len() {
        let end = (idx + leaf_size).min(entries.len());
        let s = serialize_entries(&entries[idx..end]);
        roots.push(Entry {
            tile_id: entries[idx].tile_id,
            offset: leaves.len() as u64,
            length: s.len() as u32,
            run_length: 0,
        });
        leaves.extend_from_slice(&s);
        idx += leaf_size;
    }
    (serialize_entries(&roots), leaves)
}

/// 뿌리 목록이 16,384 − 127 바이트 안에 들게 만든다. 넘치면 잎 목록으로 나눈다 (BuildDirectories).
fn build_directories(entries: &[Entry]) -> (Vec<u8>, Vec<u8>) {
    let target = 16384 - HEADER_LEN;
    if entries.len() < 16384 {
        let root = serialize_entries(entries);
        if root.len() <= target {
            return (root, Vec::new());
        }
    }
    let mut leaf_size = (entries.len() as f32 / 3500.0).max(4096.0);
    loop {
        let (root, leaves) = build_roots_leaves(entries, leaf_size as usize);
        if root.len() <= target {
            return (root, leaves);
        }
        leaf_size *= 1.2;
    }
}

/// 목록에서 타일 하나를 찾는다. 잎 목록을 가리키면 그 줄을 돌려준다 (FindTile).
fn find_tile(entries: &[Entry], tile_id: u64) -> Option<Entry> {
    let (mut m, mut n) = (0i64, entries.len() as i64 - 1);
    while m <= n {
        let k = (n + m) >> 1;
        let e = entries[k as usize];
        if tile_id > e.tile_id {
            m = k + 1;
        } else if tile_id < e.tile_id {
            n = k - 1;
        } else {
            return Some(e);
        }
    }
    if n >= 0 {
        let e = entries[n as usize];
        if e.run_length == 0 || tile_id - e.tile_id < e.run_length as u64 {
            return Some(e);
        }
    }
    None
}

// ── 네모에 드는 타일 ─────────────────────────────────────────────────────────

/// 네모가 걸친 타일 번호, 오름차순. 확대 한도에서는 걸친 칸 전부, 그 아래는 부모.
fn relevant_ids(bbox: [f64; 4], maxzoom: u8) -> Vec<u64> {
    let [w, s, e, n] = bbox;
    let size = 1u64 << maxzoom;
    let tx = |lon: f64| -> u64 {
        let v = ((lon + 180.0) / 360.0 * size as f64).floor();
        v.clamp(0.0, (size - 1) as f64) as u64
    };
    let ty = |lat: f64| -> u64 {
        let r = lat.to_radians();
        let v = ((1.0 - (r.tan() + 1.0 / r.cos()).ln() / std::f64::consts::PI) / 2.0 * size as f64).floor();
        v.clamp(0.0, (size - 1) as f64) as u64
    };
    let (x0, x1) = (tx(w), tx(e));
    let (y0, y1) = (ty(n), ty(s)); // 북쪽이 y 가 작다
    let mut ids = Vec::new();
    for z in 0..=maxzoom {
        let sh = maxzoom - z;
        for x in (x0 >> sh)..=(x1 >> sh) {
            for y in (y0 >> sh)..=(y1 >> sh) {
                ids.push(zxy_to_id(z, x as u32, y as u32));
            }
        }
    }
    ids.sort_unstable();
    ids.dedup();
    ids
}

/// 오름차순 번호 목록에서 [from, to) 에 드는 첫 자리.
fn lower_bound(ids: &[u64], v: u64) -> usize {
    ids.partition_point(|&x| x < v)
}

/// RelevantEntries — 네모에 드는 타일 줄과, 네모에 걸리는 잎 목록 줄.
fn relevant_entries(ids: &[u64], maxzoom: u8, dir: &[Entry]) -> (Vec<Entry>, Vec<Entry>) {
    let last_tile = zxy_to_id(maxzoom + 1, 0, 0);
    let mut tiles = Vec::new();
    let mut leaves = Vec::new();
    for (idx, e) in dir.iter().enumerate() {
        if e.run_length == 0 {
            let end = if idx == dir.len() - 1 { last_tile } else { dir[idx + 1].tile_id };
            let i = lower_bound(ids, e.tile_id);
            if i < ids.len() && ids[i] < end {
                leaves.push(*e);
            }
        } else {
            // 한 줄이 여러 타일을 가리키면 네모 안쪽만 이어진 토막으로 남긴다
            let end = e.tile_id + e.run_length as u64;
            let mut i = lower_bound(ids, e.tile_id);
            let mut cur: Option<Entry> = None;
            while i < ids.len() && ids[i] < end {
                let id = ids[i];
                match cur.as_mut() {
                    Some(c) if c.tile_id + c.run_length as u64 == id => c.run_length += 1,
                    _ => {
                        if let Some(c) = cur.take() {
                            tiles.push(c);
                        }
                        cur = Some(Entry { tile_id: id, offset: e.offset, length: e.length, run_length: 1 });
                    }
                }
                i += 1;
            }
            if let Some(c) = cur {
                tiles.push(c);
            }
        }
    }
    (tiles, leaves)
}

#[derive(Clone, Copy, Debug)]
struct SrcDst {
    src: u64,
    dst: u64,
    len: u64,
}

/// reencodeEntries — 새 파일 안의 자리로 옮기고, 같은 내용은 한 번만.
fn reencode(dir: &[Entry]) -> (Vec<Entry>, Vec<SrcDst>, u64, u64, u64) {
    let mut out = Vec::with_capacity(dir.len());
    let mut seen: HashMap<u64, u64> = HashMap::new();
    let mut ranges: Vec<SrcDst> = Vec::new();
    let mut addressed = 0u64;
    let mut dst = 0u64;
    for e in dir {
        if let Some(&v) = seen.get(&e.offset) {
            out.push(Entry { offset: v, ..*e });
        } else {
            match ranges.last_mut() {
                Some(r) if r.src + r.len == e.offset => r.len += e.length as u64,
                _ => ranges.push(SrcDst { src: e.offset, dst, len: e.length as u64 }),
            }
            out.push(Entry { offset: dst, ..*e });
            seen.insert(e.offset, dst);
            dst += e.length as u64;
        }
        addressed += e.run_length as u64;
    }
    let contents = seen.len() as u64;
    (out, ranges, dst, addressed, contents)
}

#[derive(Clone, Debug)]
struct Overfetch {
    rng: SrcDst,
    /// (원하는 바이트, 그 뒤 버릴 바이트) 차례
    copy_discards: Vec<(u64, u64)>,
}

/// mergeRanges — 틈이 짧은 것부터 묶는다. 버리는 바이트 합이 전체의 OVERFETCH 를 넘지 않게.
fn merge_ranges(ranges: &[SrcDst], overfetch: f64) -> Vec<Overfetch> {
    struct Item {
        rng: SrcDst,
        cds: Vec<(u64, u64)>,
        to_next: u64,
        prev: Option<usize>,
        next: Option<usize>,
        alive: bool,
    }
    let mut total = 0u64;
    let mut items: Vec<Item> = ranges
        .iter()
        .enumerate()
        .map(|(i, r)| {
            let to_next = if i == ranges.len() - 1 {
                u64::MAX
            } else {
                let n = ranges[i + 1].src as i128 - (r.src as i128 + r.len as i128);
                if n < 0 { u64::MAX } else { n as u64 }
            };
            total += r.len;
            Item {
                rng: *r,
                cds: vec![(r.len, 0)],
                to_next,
                prev: if i > 0 { Some(i - 1) } else { None },
                next: if i + 1 < ranges.len() { Some(i + 1) } else { None },
                alive: true,
            }
        })
        .collect();
    let mut budget = (total as f64 * overfetch) as i128;
    let mut order: Vec<usize> = (0..items.len()).collect();
    order.sort_by_key(|&i| items[i].to_next);
    let mut alive = items.len();
    for &i in &order {
        if alive <= 1 || budget < items[i].to_next as i128 {
            break;
        }
        let Some(nx) = items[i].next else { continue };
        let new_len = items[i].rng.len + items[i].to_next + items[nx].rng.len;
        if new_len > MAX_REQUEST {
            continue;
        }
        let (src, dst, gap, prev) = (items[i].rng.src, items[i].rng.dst, items[i].to_next, items[i].prev);
        let mut cds = std::mem::take(&mut items[i].cds);
        if let Some(last) = cds.last_mut() {
            last.1 = gap;
        }
        cds.extend_from_slice(&items[nx].cds);
        items[nx].cds = cds;
        items[nx].rng = SrcDst { src, dst, len: new_len };
        items[nx].prev = prev;
        if let Some(p) = prev {
            items[p].next = Some(nx);
        }
        items[i].alive = false;
        alive -= 1;
        budget -= gap as i128;
    }
    let mut out: Vec<Overfetch> = items
        .into_iter()
        .filter(|x| x.alive)
        .map(|x| Overfetch { rng: x.rng, copy_discards: x.cds })
        .collect();
    out.sort_by(|a, b| b.rng.len.cmp(&a.rng.len));
    out
}

// ── 원본에 조각 요청 ─────────────────────────────────────────────────────────

struct Remote {
    client: reqwest::Client,
    url: String,
}

impl Remote {
    fn new(url: &str) -> Result<Self, String> {
        let client = reqwest::Client::builder()
            .user_agent(UA)
            .connect_timeout(Duration::from_secs(15))
            .timeout(Duration::from_secs(300))
            .build()
            .map_err(|e| format!("인터넷 연결을 만들지 못했습니다 — {e}"))?;
        Ok(Self { client, url: url.to_string() })
    }

    /// [offset, offset+len) 를 받는다. 206 과 길이가 맞아야 한다. 실패하면 RETRIES 번 다시.
    async fn range(&self, offset: u64, len: u64, cancel: &AtomicBool) -> Result<Vec<u8>, String> {
        let mut last_err = String::new();
        for attempt in 0..=RETRIES {
            if cancel.load(Ordering::Relaxed) {
                return Err(CANCELLED.into());
            }
            if attempt > 0 {
                tokio::time::sleep(Duration::from_secs(1 << (attempt - 1))).await;
            }
            let res = self
                .client
                .get(&self.url)
                .header("Range", format!("bytes={}-{}", offset, offset + len - 1))
                .send()
                .await;
            match res {
                Ok(r) if r.status().as_u16() == 206 => match r.bytes().await {
                    Ok(b) if b.len() as u64 == len => return Ok(b.to_vec()),
                    Ok(b) => last_err = format!("받은 길이가 다릅니다 (원한 것 {len}, 온 것 {})", b.len()),
                    Err(e) => last_err = format!("받다가 끊겼습니다 — {e}"),
                },
                Ok(r) => last_err = format!("원본이 조각을 안 줬습니다 — HTTP {}", r.status()),
                Err(e) => last_err = format!("원본에 못 붙었습니다 — {e}"),
            }
        }
        Err(format!("{} 번 다시 해도 안 됩니다: {last_err}", RETRIES))
    }
}

const CANCELLED: &str = "취소했습니다";

// ── 받을 계획 ────────────────────────────────────────────────────────────────

#[allow(dead_code)] // contents · entry_count 는 CLI 와 대 보는 시험이 찍는다
struct Plan {
    header: Header,
    /// 새 파일의 타일 줄 (자리 옮김 끝)
    entries: Vec<Entry>,
    tile_ranges: Vec<Overfetch>,
    tile_data_len: u64,
    addressed: u64,
    contents: u64,
    requests: u64,
    /// 타일 줄 수 (CLI 의 "fetching N tiles")
    entry_count: u64,
}

fn check_bbox(bbox: [f64; 4]) -> Result<(), String> {
    let [w, s, e, n] = bbox;
    if !(w.is_finite() && s.is_finite() && e.is_finite() && n.is_finite()) {
        return Err("영역 좌표에 숫자가 아닌 값이 있습니다".into());
    }
    if !(-180.0..=180.0).contains(&w) || !(-180.0..=180.0).contains(&e) || w >= e {
        return Err(format!("경도가 이상합니다 (서 {w}, 동 {e}). 날짜변경선을 넘는 영역은 아직 안 됩니다"));
    }
    if !(-85.05..=85.05).contains(&s) || !(-85.05..=85.05).contains(&n) || s >= n {
        return Err(format!("위도가 이상합니다 (남 {s}, 북 {n})"));
    }
    Ok(())
}

async fn make_plan(remote: &Remote, bbox: [f64; 4], maxzoom: u8, cancel: &AtomicBool) -> Result<Plan, String> {
    check_bbox(bbox)?;
    let mut header = parse_header(&remote.range(0, HEADER_LEN as u64, cancel).await?)?;
    if !header.clustered {
        return Err("원본이 clustered 가 아니라 떼어 낼 수 없습니다".into());
    }
    let maxzoom = maxzoom.min(header.max_zoom);
    let ids = relevant_ids(bbox, maxzoom);

    let root_bytes = remote.range(header.root_offset, header.root_length, cancel).await?;
    let root = deserialize_entries(&root_bytes, header.internal_compression)?;
    let (mut tiles, leaves) = relevant_entries(&ids, maxzoom, &root);

    let leaf_ranges: Vec<SrcDst> = leaves
        .iter()
        .map(|l| SrcDst { src: header.leaf_offset + l.offset, dst: 0, len: l.length as u64 })
        .collect();
    let leaf_fetch = merge_ranges(&leaf_ranges, OVERFETCH);
    for of in &leaf_fetch {
        let chunk = remote.range(of.rng.src, of.rng.len, cancel).await?;
        let mut p = 0usize;
        for &(want, discard) in &of.copy_discards {
            let dir = deserialize_entries(&chunk[p..p + want as usize], header.internal_compression)?;
            let (t, deeper) = relevant_entries(&ids, maxzoom, &dir);
            if !deeper.is_empty() {
                return Err("원본에 잎 목록이 두 겹입니다. 이 앱은 한 겹만 읽습니다".into());
            }
            tiles.extend(t);
            p += (want + discard) as usize;
        }
    }
    tiles.sort_by_key(|e| e.tile_id);
    let entry_count = tiles.len() as u64;
    let (entries, parts, tile_data_len, addressed, contents) = reencode(&tiles);
    let tile_ranges = merge_ranges(&parts, OVERFETCH);
    let requests = 2 + leaf_fetch.len() as u64 + 1 + tile_ranges.len() as u64;

    // 새 머리글 — Extract 7번과 같은 규칙
    let [w, s, e, n] = bbox;
    header.min_lon_e7 = (w * 1e7) as i32;
    header.min_lat_e7 = (s * 1e7) as i32;
    header.max_lon_e7 = (e * 1e7) as i32;
    header.max_lat_e7 = (n * 1e7) as i32;
    header.center_lon_e7 = ((w + e) / 2.0 * 1e7) as i32;
    header.center_lat_e7 = ((s + n) / 2.0 * 1e7) as i32;
    header.min_zoom = 0;
    header.max_zoom = maxzoom;
    header.center_zoom = header.center_zoom.clamp(header.min_zoom, header.max_zoom);
    header.tile_data_length = tile_data_len;
    header.addressed_tiles = addressed;
    header.tile_entries = entry_count;
    header.tile_contents = contents;

    Ok(Plan { header, entries, tile_ranges, tile_data_len, addressed, contents, requests, entry_count })
}

// ── 파일에 자리 지정 쓰기 ────────────────────────────────────────────────────

#[cfg(unix)]
fn write_at(f: &File, buf: &[u8], off: u64) -> std::io::Result<()> {
    use std::os::unix::fs::FileExt;
    f.write_all_at(buf, off)
}
#[cfg(windows)]
fn write_at(f: &File, mut buf: &[u8], mut off: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !buf.is_empty() {
        let n = f.write_at(buf, off)?;
        buf = &buf[n..];
        off += n as u64;
    }
    Ok(())
}
#[cfg(unix)]
fn read_at(f: &File, buf: &mut [u8], off: u64) -> std::io::Result<()> {
    use std::os::unix::fs::FileExt;
    f.read_exact_at(buf, off)
}
#[cfg(windows)]
fn read_at(f: &File, buf: &mut [u8], mut off: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    let mut done = 0;
    while done < buf.len() {
        let n = f.read_at(&mut buf[done..], off)?;
        if n == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        done += n;
        off += n as u64;
    }
    Ok(())
}

// ── 받기 (Tauri 와 떨어진 몸통 — 시험에서 그대로 부른다) ─────────────────────

pub struct ExtractResult {
    pub bytes: u64,
    pub tiles: u64,
    pub osm_time: String,
}

pub async fn extract_to(
    url: &str,
    out: &Path,
    bbox: [f64; 4],
    maxzoom: u8,
    cancel: Arc<AtomicBool>,
    progress: Arc<dyn Fn(&'static str, u64, u64, u64, u64) + Send + Sync>,
) -> Result<ExtractResult, String> {
    let remote = Arc::new(Remote::new(url)?);
    progress("dir", 0, 0, 0, 0);
    let plan = make_plan(&remote, bbox, maxzoom, &cancel).await?;
    let src_header = parse_header(&remote.range(0, HEADER_LEN as u64, &cancel).await?)?;
    let metadata = remote.range(src_header.metadata_offset, src_header.metadata_length, &cancel).await?;
    let osm_time = decompress(&metadata, src_header.internal_compression)
        .ok()
        .and_then(|b| serde_json::from_slice::<serde_json::Value>(&b).ok())
        .and_then(|v| v.get("planetiler:osm:osmosisreplicationtime").and_then(|x| x.as_str()).map(String::from))
        .unwrap_or_default();

    let (root, leaves) = build_directories(&plan.entries);
    let mut h = plan.header.clone();
    h.root_offset = HEADER_LEN as u64;
    h.root_length = root.len() as u64;
    h.metadata_offset = h.root_offset + h.root_length;
    h.metadata_length = metadata.len() as u64;
    h.leaf_offset = h.metadata_offset + h.metadata_length;
    h.leaf_length = leaves.len() as u64;
    h.tile_data_offset = h.leaf_offset + h.leaf_length;

    let file = Arc::new(File::create(out).map_err(|e| format!("파일을 못 만들었습니다 {} — {e}", out.display()))?);
    let total_len = h.tile_data_offset + plan.tile_data_len;
    file.set_len(total_len).map_err(|e| format!("파일 자리를 못 잡았습니다 — {e}"))?;
    let io = |e: std::io::Error| format!("파일에 못 썼습니다 — {e}");
    write_at(&file, &vec![0u8; HEADER_LEN], 0).map_err(io)?; // 머리글은 끝에 쓴다
    write_at(&file, &root, h.root_offset).map_err(io)?;
    write_at(&file, &metadata, h.metadata_offset).map_err(io)?;
    write_at(&file, &leaves, h.leaf_offset).map_err(io)?;

    // 타일 — 요청 묶음을 WORKERS 개가 나눠 받는다
    let total_fetch: u64 = plan.tile_ranges.iter().map(|r| r.rng.len).sum();
    let done_bytes = Arc::new(AtomicU64::new(0));
    let queue = Arc::new(Mutex::new(plan.tile_ranges.clone()));
    let total_tiles = plan.addressed;
    progress("tiles", 0, total_fetch, 0, total_tiles);
    let mut set = tokio::task::JoinSet::new();
    for _ in 0..WORKERS {
        let (remote, queue, file, cancel, done_bytes, progress) =
            (remote.clone(), queue.clone(), file.clone(), cancel.clone(), done_bytes.clone(), progress.clone());
        let base = h.tile_data_offset;
        let src_base = src_header.tile_data_offset;
        set.spawn(async move {
            loop {
                let job = queue.lock().unwrap().pop();
                let Some(of) = job else { return Ok::<(), String>(()) };
                let chunk = remote.range(src_base + of.rng.src, of.rng.len, &cancel).await?;
                let (mut p, mut dst) = (0usize, of.rng.dst);
                for &(want, discard) in &of.copy_discards {
                    write_at(&file, &chunk[p..p + want as usize], base + dst)
                        .map_err(|e| format!("파일에 못 썼습니다 — {e}"))?;
                    dst += want;
                    p += (want + discard) as usize;
                }
                let d = done_bytes.fetch_add(of.rng.len, Ordering::Relaxed) + of.rng.len;
                let t = if total_fetch > 0 { (total_tiles as f64 * d as f64 / total_fetch as f64) as u64 } else { 0 };
                progress("tiles", d, total_fetch, t, total_tiles);
            }
        });
    }
    let mut first_err: Option<String> = None;
    while let Some(r) = set.join_next().await {
        let r = r.map_err(|e| format!("받는 일꾼이 죽었습니다 — {e}")).and_then(|x| x);
        if let Err(e) = r {
            if first_err.is_none() {
                first_err = Some(e);
                cancel.store(true, Ordering::Relaxed); // 나머지도 멈춘다
            }
        }
    }
    if let Some(e) = first_err {
        return Err(e);
    }

    progress("write", total_fetch, total_fetch, total_tiles, total_tiles);
    write_at(&file, &serialize_header(&h), 0).map_err(io)?;
    file.sync_all().map_err(io)?;
    Ok(ExtractResult { bytes: total_len, tiles: plan.addressed, osm_time })
}

// ── 받은 영역에서 타일 꺼내기 ────────────────────────────────────────────────

struct Archive {
    region: Region,
    file: File,
    header: Header,
    root: Vec<Entry>,
    leaves: Mutex<HashMap<u64, Arc<Vec<Entry>>>>,
}

impl Archive {
    fn open(path: &Path, region: Region) -> Result<Self, String> {
        let file = File::open(path).map_err(|e| format!("{} 을 못 열었습니다 — {e}", path.display()))?;
        let mut hb = vec![0u8; HEADER_LEN];
        read_at(&file, &mut hb, 0).map_err(|e| format!("머리글을 못 읽었습니다 — {e}"))?;
        let header = parse_header(&hb)?;
        let mut rb = vec![0u8; header.root_length as usize];
        read_at(&file, &mut rb, header.root_offset).map_err(|e| format!("뿌리 목록을 못 읽었습니다 — {e}"))?;
        let root = deserialize_entries(&rb, header.internal_compression)?;
        Ok(Self { region, file, header, root, leaves: Mutex::new(HashMap::new()) })
    }

    /// 그 타일의 압축 풀린 바이트. 이 파일에 없으면 None.
    fn tile(&self, tile_id: u64) -> Result<Option<Vec<u8>>, String> {
        let mut dir: Arc<Vec<Entry>> = Arc::new(Vec::new());
        let mut e = find_tile(&self.root, tile_id);
        for _depth in 0..4 {
            let Some(x) = e else { return Ok(None) };
            if x.run_length > 0 {
                let mut buf = vec![0u8; x.length as usize];
                read_at(&self.file, &mut buf, self.header.tile_data_offset + x.offset)
                    .map_err(|er| format!("타일을 못 읽었습니다 — {er}"))?;
                return decompress(&buf, self.header.tile_compression).map(Some);
            }
            let key = x.offset;
            let cached = self.leaves.lock().unwrap().get(&key).cloned();
            dir = match cached {
                Some(d) => d,
                None => {
                    let mut buf = vec![0u8; x.length as usize];
                    read_at(&self.file, &mut buf, self.header.leaf_offset + x.offset)
                        .map_err(|er| format!("잎 목록을 못 읽었습니다 — {er}"))?;
                    let d = Arc::new(deserialize_entries(&buf, self.header.internal_compression)?);
                    self.leaves.lock().unwrap().insert(key, d.clone());
                    d
                }
            };
            e = find_tile(&dir, tile_id);
        }
        let _ = dir;
        Err("목록이 너무 깊습니다".into())
    }
}

// ── Tauri 상태와 명령 ────────────────────────────────────────────────────────

#[derive(Default)]
pub struct OffmapState {
    /// 열어 둔 영역들. 새로 받은 것이 앞.
    archives: RwLock<Option<Vec<Arc<Archive>>>>,
    jobs: Mutex<HashMap<String, Arc<AtomicBool>>>,
    /// regions.json 을 고치는 동안 잠근다
    index_lock: Mutex<()>,
}

fn maps_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let d = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("앱 데이터 폴더를 못 찾았습니다 — {e}"))?
        .join("maps");
    std::fs::create_dir_all(&d).map_err(|e| format!("{} 을 못 만들었습니다 — {e}", d.display()))?;
    Ok(d)
}

fn read_index(dir: &Path) -> Vec<Region> {
    std::fs::read(dir.join("regions.json"))
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_default()
}

fn write_index(dir: &Path, regions: &[Region]) -> Result<(), String> {
    let tmp = dir.join("regions.json.tmp");
    let body = serde_json::to_vec_pretty(regions).map_err(|e| format!("목록을 못 만들었습니다 — {e}"))?;
    std::fs::write(&tmp, body).map_err(|e| format!("목록을 못 썼습니다 — {e}"))?;
    std::fs::rename(&tmp, dir.join("regions.json")).map_err(|e| format!("목록을 못 바꿨습니다 — {e}"))
}

/// regions.json 을 다시 읽어 파일을 연다. 못 연 영역은 건너뛰고 로그에 남긴다.
fn reload(app: &AppHandle, st: &OffmapState) -> Result<(), String> {
    let dir = maps_dir(app)?;
    let mut list = Vec::new();
    for r in read_index(&dir).into_iter().rev() {
        match Archive::open(&dir.join(format!("{}.pmtiles", r.id)), r.clone()) {
            Ok(a) => list.push(Arc::new(a)),
            Err(e) => eprintln!("[지도] 영역 {} ({}) 을 못 열었습니다: {e}", r.name, r.id),
        }
    }
    *st.archives.write().unwrap() = Some(list);
    Ok(())
}

fn now_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

#[tauri::command]
pub async fn offmap_list(app: AppHandle) -> Result<Vec<Region>, String> {
    Ok(read_index(&maps_dir(&app)?))
}

#[tauri::command]
pub async fn offmap_estimate(bbox: [f64; 4], maxzoom: u8) -> Result<Estimate, String> {
    let remote = Remote::new(SOURCE_URL)?;
    let plan = make_plan(&remote, bbox, maxzoom, &AtomicBool::new(false)).await?;
    Ok(Estimate { tiles: plan.addressed, bytes: plan.tile_data_len, requests: plan.requests })
}

#[tauri::command]
pub async fn offmap_download(
    app: AppHandle,
    state: State<'_, OffmapState>,
    name: String,
    bbox: [f64; 4],
    maxzoom: u8,
) -> Result<String, String> {
    check_bbox(bbox)?;
    let dir = maps_dir(&app)?;
    let id = format!(
        "r{}",
        SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0)
    );
    let cancel = Arc::new(AtomicBool::new(false));
    state.jobs.lock().unwrap().insert(id.clone(), cancel.clone());

    let (app2, id2) = (app.clone(), id.clone());
    tauri::async_runtime::spawn(async move {
        let part = dir.join(format!("{id2}.pmtiles.part"));
        let fin = dir.join(format!("{id2}.pmtiles"));
        let emit = {
            let (app, id) = (app2.clone(), id2.clone());
            move |phase: &'static str, db: u64, tb: u64, dt: u64, tt: u64, error: Option<String>| {
                let _ = app.emit(
                    "offmap-progress",
                    Progress { id: id.clone(), phase, done_bytes: db, total_bytes: tb, done_tiles: dt, total_tiles: tt, error },
                );
            }
        };
        let last = Arc::new(Mutex::new(std::time::Instant::now() - Duration::from_secs(1)));
        let emit_p = {
            let emit = emit.clone();
            let last = last.clone();
            Arc::new(move |phase: &'static str, db: u64, tb: u64, dt: u64, tt: u64| {
                // 진행은 0.25초에 한 번만 알린다. 단계가 바뀔 때는 늘 알린다
                let mut l = last.lock().unwrap();
                if phase != "tiles" || db == tb || l.elapsed() >= Duration::from_millis(250) {
                    *l = std::time::Instant::now();
                    emit(phase, db, tb, dt, tt, None);
                }
            }) as Arc<dyn Fn(&'static str, u64, u64, u64, u64) + Send + Sync>
        };

        let res = extract_to(SOURCE_URL, &part, bbox, maxzoom, cancel.clone(), emit_p).await;
        let st = app2.state::<OffmapState>();
        st.jobs.lock().unwrap().remove(&id2);
        match res {
            Ok(r) => {
                if let Err(e) = std::fs::rename(&part, &fin) {
                    let _ = std::fs::remove_file(&part);
                    emit("error", 0, 0, 0, 0, Some(format!("받은 파일 이름을 못 바꿨습니다 — {e}")));
                    return;
                }
                let region = Region {
                    id: id2.clone(),
                    name,
                    bbox,
                    maxzoom,
                    bytes: r.bytes,
                    tiles: r.tiles,
                    created: now_secs(),
                    source_url: SOURCE_URL.into(),
                    source_osm_time: r.osm_time,
                };
                let saved = {
                    let _g = st.index_lock.lock().unwrap();
                    let mut list = read_index(&dir);
                    list.push(region);
                    write_index(&dir, &list)
                };
                if let Err(e) = saved.and_then(|_| reload(&app2, &st)) {
                    emit("error", 0, 0, 0, 0, Some(e));
                    return;
                }
                emit("done", r.bytes, r.bytes, r.tiles, r.tiles, None);
            }
            Err(e) => {
                let _ = std::fs::remove_file(&part);
                if cancel.load(Ordering::Relaxed) && (e == CANCELLED || e.contains(CANCELLED)) {
                    emit("cancelled", 0, 0, 0, 0, None);
                } else {
                    emit("error", 0, 0, 0, 0, Some(e));
                }
            }
        }
    });
    Ok(id)
}

#[tauri::command]
pub async fn offmap_cancel(state: State<'_, OffmapState>, id: String) -> Result<(), String> {
    match state.jobs.lock().unwrap().get(&id) {
        Some(c) => {
            c.store(true, Ordering::Relaxed);
            Ok(())
        }
        None => Err(format!("받는 중인 영역 {id} 이 없습니다")),
    }
}

#[tauri::command]
pub async fn offmap_delete(app: AppHandle, state: State<'_, OffmapState>, id: String) -> Result<(), String> {
    let dir = maps_dir(&app)?;
    {
        let _g = state.index_lock.lock().unwrap();
        let mut list = read_index(&dir);
        let before = list.len();
        list.retain(|r| r.id != id);
        if list.len() == before {
            return Err(format!("영역 {id} 이 목록에 없습니다"));
        }
        write_index(&dir, &list)?;
    }
    // 열어 둔 파일을 먼저 놓는다
    if let Some(v) = state.archives.write().unwrap().as_mut() {
        v.retain(|a| a.region.id != id);
    }
    let path = dir.join(format!("{id}.pmtiles"));
    if path.exists() {
        std::fs::remove_file(&path).map_err(|e| format!("{} 을 못 지웠습니다 — {e}", path.display()))?;
    }
    reload(&app, &state)
}

#[tauri::command]
pub async fn offmap_tile(
    app: AppHandle,
    state: State<'_, OffmapState>,
    z: u8,
    x: u32,
    y: u32,
) -> Result<tauri::ipc::Response, String> {
    if state.archives.read().unwrap().is_none() {
        reload(&app, &state)?;
    }
    let archives = state.archives.read().unwrap().clone().unwrap_or_default();
    if z > 30 || (x as u64) >= (1u64 << z) || (y as u64) >= (1u64 << z) {
        return Err(format!("타일 번호가 이상합니다 {z}/{x}/{y}"));
    }
    let id = zxy_to_id(z, x, y);
    for a in archives.iter() {
        if z > a.region.maxzoom {
            continue;
        }
        if let Some(bytes) = a.tile(id)? {
            return Ok(tauri::ipc::Response::new(bytes));
        }
    }
    Ok(tauri::ipc::Response::new(Vec::<u8>::new()))
}

// ── 시험 ─────────────────────────────────────────────────────────────────────
//
//   cargo test --lib offmap                         인터넷 없이 되는 것
//   cargo test --lib offmap -- --ignored --nocapture 원본에 붙는 것 (환경변수는 각 시험 주석)

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hilbert_matches_spec_examples() {
        // 규격 원문의 예
        assert_eq!(zxy_to_id(0, 0, 0), 0);
        assert_eq!(zxy_to_id(1, 0, 0), 1);
        assert_eq!(zxy_to_id(1, 0, 1), 2);
        assert_eq!(zxy_to_id(1, 1, 1), 3);
        assert_eq!(zxy_to_id(1, 1, 0), 4);
        for z in 0..=15u8 {
            let n = 1u32 << z;
            for (x, y) in [(0, 0), (n - 1, 0), (0, n - 1), (n - 1, n - 1), (n / 3, n / 7)] {
                assert_eq!(id_to_zxy(zxy_to_id(z, x, y)), (z, x, y), "z{z} x{x} y{y}");
            }
        }
    }

    /// 2026-09-15 앱 안에서 "attempt to subtract with overflow" 로 죽었던 입력들.
    /// rotate 의 n−1−x 에서 x 가 칸 크기 s 보다 크면 음수가 된다 (x·y 는 전체 좌표라서).
    /// 한국 부근 타일은 거의 전부 해당한다 — 첫 칸(s=2^(z−1))에서 x 비트가 서고 y 비트가 비면 바로 넘어간다.
    /// 기댓값은 go-pmtiles tile_id.go 를 uint32 감아 넘기기 그대로 파이썬으로 옮겨 구했다.
    #[test]
    fn hilbert_full_coordinate_wrap_regression() {
        for (z, x, y, want) in [
            (1u8, 1u32, 0u32, 4u64),          // s=1 x=1 y=0 에서 넘어감
            (2, 2, 0, 19),                    // s=2 x=2 y=0
            (2, 3, 1, 17),                    // s=2 x=3 y=1
            (14, 13951, 6344, 304727316),     // 인천 앞바다 — 앱이 죽은 영역
            (14, 13952, 6345, 304735638),
            (15, 27904, 12690, 1218942553),
            (15, 32767, 0, 1431655764),       // 확대 15 의 마지막 번호
            (15, 0, 32767, 715827882),        // 넘어가지 않는 입력도 같이
            (15, 32767, 32767, 1073741823),
        ] {
            assert_eq!(zxy_to_id(z, x, y), want, "z{z} x{x} y{y}");
            assert_eq!(id_to_zxy(want), (z, x, y), "되돌리기 z{z} x{x} y{y}");
        }
    }

    #[test]
    fn directory_roundtrip() {
        let e = vec![
            Entry { tile_id: 5, offset: 0, length: 10, run_length: 1 },
            Entry { tile_id: 6, offset: 10, length: 20, run_length: 3 },
            Entry { tile_id: 20, offset: 0, length: 10, run_length: 1 },
            Entry { tile_id: 21, offset: 30, length: 5, run_length: 1 },
        ];
        assert_eq!(deserialize_entries(&serialize_entries(&e), GZIP).unwrap(), e);
        let h = parse_header(&{
            let mut b = vec![0u8; 127];
            b[0..7].copy_from_slice(b"PMTiles");
            b[7] = 3;
            b[96] = 1;
            b[97] = 2;
            b[102..106].copy_from_slice(&(-1234567i32).to_le_bytes());
            b
        })
        .unwrap();
        assert_eq!(serialize_header(&h)[102..106], (-1234567i32).to_le_bytes());
    }

    fn runtime() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_multi_thread().enable_all().build().unwrap()
    }

    /// OFFMAP_BBOX="w,s,e,n" OFFMAP_Z=15 — 계획만 세워 CLI dry-run 과 대 본다.
    #[test]
    #[ignore]
    fn estimate_against_cli() {
        let bbox: Vec<f64> = std::env::var("OFFMAP_BBOX").unwrap().split(',').map(|v| v.parse().unwrap()).collect();
        let z: u8 = std::env::var("OFFMAP_Z").unwrap().parse().unwrap();
        runtime().block_on(async {
            let remote = Remote::new(SOURCE_URL).unwrap();
            let t0 = std::time::Instant::now();
            let p = make_plan(&remote, [bbox[0], bbox[1], bbox[2], bbox[3]], z, &AtomicBool::new(false)).await.unwrap();
            let region_ids = relevant_ids([bbox[0], bbox[1], bbox[2], bbox[3]], z.min(p.header.max_zoom)).len();
            println!(
                "PLAN bbox={bbox:?} z={z} region_tiles={region_ids} entries={} addressed={} contents={} tile_bytes={} tile_requests={} requests={} ({:.1}s)",
                p.entry_count, p.addressed, p.contents, p.tile_data_len, p.tile_ranges.len(), p.requests,
                t0.elapsed().as_secs_f64()
            );
        });
    }

    /// OFFMAP_BBOX · OFFMAP_Z · OFFMAP_OUT=/path/out.pmtiles — 실제로 받는다.
    #[test]
    #[ignore]
    fn extract_small() {
        let bbox: Vec<f64> = std::env::var("OFFMAP_BBOX").unwrap().split(',').map(|v| v.parse().unwrap()).collect();
        let z: u8 = std::env::var("OFFMAP_Z").unwrap().parse().unwrap();
        let out = PathBuf::from(std::env::var("OFFMAP_OUT").unwrap());
        runtime().block_on(async {
            let t0 = std::time::Instant::now();
            let prog: Arc<dyn Fn(&'static str, u64, u64, u64, u64) + Send + Sync> =
                Arc::new(|ph, db, tb, _, _| if ph != "tiles" || db == tb { println!("PROGRESS {ph} {db}/{tb}") });
            let r = extract_to(SOURCE_URL, &out, [bbox[0], bbox[1], bbox[2], bbox[3]], z, Arc::new(AtomicBool::new(false)), prog)
                .await
                .unwrap();
            println!("EXTRACT bytes={} tiles={} osm_time={} ({:.1}s)", r.bytes, r.tiles, r.osm_time, t0.elapsed().as_secs_f64());
        });
    }

    /// OFFMAP_OUT=받은 파일 · OFFMAP_TILES="z/x/y z/x/y …" · OFFMAP_DUMP=폴더 — 타일 꺼내기 경로로 풀어서 떨군다.
    #[test]
    #[ignore]
    fn tile_read_path() {
        let path = PathBuf::from(std::env::var("OFFMAP_OUT").unwrap());
        let dump = PathBuf::from(std::env::var("OFFMAP_DUMP").unwrap());
        std::fs::create_dir_all(&dump).unwrap();
        let region = Region {
            id: "test".into(), name: "test".into(), bbox: [0.0; 4], maxzoom: 15, bytes: 0, tiles: 0,
            created: 0, source_url: String::new(), source_osm_time: String::new(),
        };
        let a = Archive::open(&path, region).unwrap();
        for t in std::env::var("OFFMAP_TILES").unwrap().split_whitespace() {
            let v: Vec<u32> = t.split('/').map(|s| s.parse().unwrap()).collect();
            let got = a.tile(zxy_to_id(v[0] as u8, v[1], v[2])).unwrap();
            match got {
                Some(b) => {
                    std::fs::write(dump.join(format!("{}_{}_{}.mvt", v[0], v[1], v[2])), &b).unwrap();
                    println!("TILE {t} {} bytes", b.len());
                }
                None => println!("TILE {t} none"),
            }
        }
    }
}
