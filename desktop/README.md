# Tauri + Vanilla TS

This template should help get you started developing with Tauri in vanilla HTML, CSS and Typescript.

## Recommended IDE Setup

- [VS Code](https://code.visualstudio.com/) + [Tauri](https://marketplace.visualstudio.com/items?itemName=tauri-apps.tauri-vscode) + [rust-analyzer](https://marketplace.visualstudio.com/items?itemName=rust-lang.rust-analyzer)

---

# Sail Analyzer

경기정 모듈이 기록한 `.HLG` 파일을 받아 타임라인으로 본다.
설계는 저장소 뿌리의 `TRANSFER.md`, 파일 규격은 `docs/spec/` 에 있다.

```
npm install
npm run tauri dev      만들면서 보기 (시험용 데이터가 저절로 열린다)
npm run tauri build    배포판
```

## 왜 Tauri 인가

윈도에서는 WebView2 가 곧 Chromium 이다. 맥만 WebKit 인데, 필요한 것이
다 있고 (`requestVideoFrameCallback` Safari 15.4+, WebCodecs 16.4+) HEVC 는
오히려 맥이 기본 지원한다. Electron 은 설치 파일이 150 MB, Tauri 는 10 MB 다.

**Rust 는 얇게 둔다.** 파일 열고 저장하고 보드에서 받아오는 것만 Rust 고,
읽고 그리는 것은 전부 TypeScript 다. 실측으로 8시간 세션(88.7 MB, IMU 288만
줄)을 **2.3초**에 읽는다. 파서를 Rust 로 옮길 이유가 아직 없다.

## 폴더

```
src/hlog.ts       .HLG 읽기 — tools/hlog_parse.py 와 같은 것을 본다
src/timeline.ts   그리기. 픽셀마다 최소·최대만 남겨서 줄인다
src/main.ts       화면과 조작
public/sample.HLG 시험용 데이터 (tools/hlog_make_fixture.py 로 만든다)
```

## 조작 — Saleae Logic 과 같게

```
휠 / 트랙패드 위아래   커서가 놓인 시각을 붙잡고 확대·축소
트랙패드 좌우          밀기
시프트 + 휠            밀기
끌기                   밀기
두 번 누르기           그 자리로 당기기
아래 띠 클릭·끌기      그 자리로 건너뛰기
← →                    밀기 (시프트를 누르면 크게)
+ −                    확대·축소
F / 0                  전체 보기
```

**확대할 때 커서 아래 시각이 제자리에 있어야 한다.** 가운데를 기준으로
확대하면 보고 있던 곳이 화면 밖으로 달아난다.

## 그리기 — 봉우리를 잃지 않는다

8시간이면 IMU 가 288만 점이다. 그대로 그리면 못 쓴다. 화면 가로 픽셀 수만큼
구간을 나누고 **구간마다 최소·최대만** 남겨 세로줄 하나로 그린다.

평균을 쓰면 안 된다. 파도로 튄 봉우리가 사라진다. **그 봉우리가 우리가
100 Hz 로 기록한 이유다.**

## 보드가 없을 때

`tools/hlog_make_fixture.py` 가 펌웨어와 바이트 하나까지 같은 파일을 만든다.
만든 파일은 `tools/hlog_parse.py` 검사를 통과해야 한다.

```
python3 tools/hlog_make_fixture.py desktop/public/sample.HLG --minutes 6
python3 tools/hlog_parse.py desktop/public/sample.HLG
```

## 영상 붙이기

가운데를 좌우 반반으로 나눈다. 왼쪽이 영상, 오른쪽이 데이터다. 가운데 선을
끌면 비율이 바뀐다.

```
영상 열기 / 끌어다 놓기   MP4 · MOV
스페이스                 재생 · 정지
커서를 끌면              영상이 따라온다 (정지 중일 때)
영상이 돌면              타임라인 커서가 따라간다
```

### 왜 수동 싱크가 필요한가

고프로·DJI 가 파일에 적어 두는 찍은 시각은 **틀린 경우가 잦다.**

- 카메라 시계를 안 맞췄다
- 시간대를 UTC 가 아니라 그 지역 시각으로 적었다
- 배터리를 빼서 시계가 돌아갔다

그래서 파일이 적어 둔 시각은 **첫 짐작으로만** 쓴다. 시간대를 잘못 적은
경우는 되돌려 준다 — 한 시간 단위로 딱 떨어지고 그 양이 시간대 범위
(-12~+14시간) 안일 때만이다. 이틀 뒤에 찍은 영상까지 끌어다 붙이면 안 된다.

맞추는 법은 눈으로 한다. **태킹하는 순간은 영상에도 보이고 힐 그래프에도
보인다.** 타임라인에서 그 순간에 커서를 두고, 영상을 그 장면에 맞춘 뒤
`여기 맞춤` 을 누르면 끝난다. −10초 / −1초 / +1초 / +10초 로 밀어도 된다.

아래 숫자는 `영상 0초 = 세션 +21.0초` 처럼 나온다. 누른 부호대로 이 숫자가
움직인다.

## 칸 켜고 끄기

```
[보관함] [영상] [데이터] [정보]      맨 왼쪽 위 네 단추
   1       2      3       4        숫자 키로도 된다
```

네 칸이 다 켜져 있으면 각자 좁다. **끈 칸의 자리는 남은 칸이 나눠 쓴다.**
데이터만 켜면 타임라인이 화면을 다 쓰고, 영상만 켜면 영상이 다 쓴다.

마지막 하나까지 끄는 것은 막는다 — 볼 게 없어진다.

고른 것은 남는다. 다시 열면 그대로다.

## 보드 없이 회수 화면 시험하기

```
python3 tools/fakeboard.py          가짜 보드 (127.0.0.1:8099)
```

앱에서 주소를 `127.0.0.1:8099` 로 두고 [보드] 칸 → [목록].

일부러 험한 것을 섞어 둔다 — 버린 줄이 있는 세션, 제대로 안 닫힌 파일,
위성을 못 잡은 세션. **화면이 그걸 제대로 알려 주는지** 봐야 한다.

## 브라우저에서 만들 때

`npm run dev` 만 띄우고 브라우저로 `localhost:1420` 을 열면 훨씬 빠르다.
Tauri 가 없으면 이렇게 대신한다.

| | 앱 | 브라우저 |
|---|---|---|
| 보관함 목록 | `library.json` | localStorage |
| 기록 파일 | `logs/` 아래 | 램 (새로 읽으면 사라진다) |
| 보드 붙기 | Tauri http | 보통 fetch |

**영상은 브라우저 탭이 숨겨져 있으면 크롬이 안 읽는다.** 자동화로 시험할
때 `readyState` 가 0 에서 안 올라가면 그 때문이다. 앱에서 봐야 한다.

## 화면 하나만 크게

칸마다 오른쪽 위에 손잡이가 있다. 마우스를 올리면 뚜렷해진다.

```
⤢   이 칸만 크게      ⤡   되돌리기      ✕   이 칸 닫기
```

**크게 하고 싶은 칸을 보고 있을 때 손잡이도 거기 있다.** 위 띠까지 눈이
올라갔다 내려올 필요가 없다.

위 띠의 단추로도 된다.

```
◉ 보관함   ◉ 영상   ◉ 데이터   ◉ 정보     파란색이 켜진 것
```

| | |
|---|---|
| 한 번 누르기 | 켜기 / 끄기 |
| 두 번 누르기 | 그 칸만 크게 |
| `1 2 3 4` | 켜기 / 끄기 |
| `Shift + 1 2 3 4` | 그 칸만 크게 |

마지막 한 칸까지 끄려고 하면 **넷 다 켜진다.** 거절만 하면 사람이 어떻게
되돌리는지 모른다.
