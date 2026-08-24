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
