// 영상과 데이터 시각 맞추기.
//
// 왜 수동 맞춤이 필요한가
//   고프로·DJI 는 찍은 시각을 파일에 적어 두는데, 그게 틀린 경우가 잦다.
//   카메라 시계를 안 맞췄거나, 시간대를 UTC 가 아니라 그 지역 시각으로
//   적어 두거나, 배터리를 빼면 시계가 돌아간다.
//
//   그래서 파일이 적어 둔 시각은 **첫 짐작**으로만 쓰고, 사람이 눈으로 보고
//   맞출 수 있게 한다. 태킹하는 순간이 영상에도 데이터에도 보이므로 그
//   순간을 맞추면 끝난다.
//
// 어떻게 맞추나
//   offsetMs = 영상 0초에 해당하는 **세션 시각(ms)**
//
//     세션 시각 → 영상 시각   (sessionMs - offsetMs) / 1000
//     영상 시각 → 세션 시각   offsetMs + videoTime * 1000

/** MP4 가 적어 둔 찍은 시각을 읽는다. 못 읽으면 null. */
export async function mp4CreationTime(blob: Blob): Promise<Date | null> {
  // moov > mvhd 를 찾는다. 파일 앞이나 뒤에 있으므로 양쪽을 본다.
  //
  // 상자 구조:  [크기 4바이트][이름 4바이트][내용 …]
  // mvhd 의 creation_time 은 **1904-01-01 부터의 초**다 (UNIX 는 1970).
  const MAC_EPOCH_DIFF = 2082844800;

  const scan = async (from: number, len: number): Promise<Date | null> => {
    const buf = new Uint8Array(await blob.slice(from, from + len).arrayBuffer());
    const d = new DataView(buf.buffer);
    for (let i = 0; i + 20 < buf.length; i++) {
      if (buf[i] !== 0x6d || buf[i + 1] !== 0x76 ||
          buf[i + 2] !== 0x68 || buf[i + 3] !== 0x64) continue;  // "mvhd"
      const ver = buf[i + 4];
      let secs: number;
      if (ver === 1) {
        // 64비트. 자바스크립트 수로 안전한 범위라 상위 32비트는 버려도 된다.
        const hi = d.getUint32(i + 8, false);
        const lo = d.getUint32(i + 12, false);
        secs = hi * 4294967296 + lo;
      } else {
        secs = d.getUint32(i + 8, false);
      }
      const unix = secs - MAC_EPOCH_DIFF;
      // 2000년 이전이거나 2100년 이후면 못 믿는다
      if (unix < 946684800 || unix > 4102444800) continue;
      return new Date(unix * 1000);
    }
    return null;
  };

  const head = await scan(0, Math.min(blob.size, 2 * 1024 * 1024));
  if (head) return head;
  if (blob.size > 2 * 1024 * 1024) {
    return await scan(Math.max(0, blob.size - 2 * 1024 * 1024), 2 * 1024 * 1024);
  }
  return null;
}

export interface Sync {
  /** 영상 0초에 해당하는 세션 시각 (ms) */
  offsetMs: number;
  /** 파일이 적어 둔 시각으로 짐작했나. 사람이 손대면 false */
  guessed: boolean;
  /** 파일이 적어 둔 시각 (있으면) */
  fileTime: Date | null;
}

/**
 * 첫 짐작. 파일이 적어 둔 시각과 세션의 첫 fix UTC 를 견준다.
 *
 * 카메라가 시간대를 그 지역 시각으로 적어 두는 일이 많아서, 한 시간 단위로
 * 어긋나면 그쪽으로 당겨 본다. 그래도 틀리면 사람이 맞춘다.
 */
export function guessOffset(
  fileTime: Date | null,
  sessionUtcStart: number,      // UNIX 초. 0 이면 모름
  sessionFirstMs: number,       // 세션 첫 레코드의 local_ms
): Sync {
  if (!fileTime || !sessionUtcStart) {
    return { offsetMs: 0, guessed: false, fileTime };
  }
  // 영상이 시작한 UTC - 세션이 시작한 UTC = 세션 안에서의 자리
  let delta = fileTime.getTime() - sessionUtcStart * 1000;

  // 시간대를 잘못 적은 경우를 되돌린다.
  //
  // 카메라가 UTC 가 아니라 그 지역 시각을 적어 두면 정확히 몇 시간 단위로
  // 어긋난다. 그럴 때만 지운다. 조건 둘을 다 만족해야 한다.
  //
  //   1) 한 시간 단위에서 2분 안으로 딱 떨어진다
  //   2) 어긋난 양이 시간대 범위 안이다 (-12 ~ +14시간)
  //
  // 2번이 없으면 "이틀 뒤에 찍은 영상" 까지 끌어다 붙인다. 그건 시간대가
  // 아니라 다른 날 영상이고, 사람이 봐야 한다.
  const hour = 3600_000;
  const off = Math.round(delta / hour) * hour;
  const tzLike = Math.abs(delta - off) < 120_000
                 && Math.abs(off) >= hour
                 && off >= -12 * hour && off <= 14 * hour;
  if (tzLike) delta -= off;

  return { offsetMs: delta + sessionFirstMs, guessed: true, fileTime };
}

export const sessionToVideo = (s: Sync, sessionMs: number) =>
  (sessionMs - s.offsetMs) / 1000;

export const videoToSession = (s: Sync, videoSec: number) =>
  s.offsetMs + videoSec * 1000;

export function formatOffset(ms: number): string {
  const sign = ms < 0 ? "-" : "+";
  const t = Math.abs(ms) / 1000;
  const m = Math.floor(t / 60);
  const s = t - m * 60;
  return m > 0 ? `${sign}${m}분 ${s.toFixed(1)}초` : `${sign}${s.toFixed(1)}초`;
}
