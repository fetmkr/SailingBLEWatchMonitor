# brand

앱 아이콘 원본과 생성물.

```
FETMMarine.png        원본 로고 (1024x1024, #0000FF 배경 + 흰 FETM 마크)
icon-ios-1024.png     iOS 용 — 풀블리드. 시스템이 둥근 사각형으로 마스킹한다
icon-watch-1024.png   watchOS 용 — 마크를 65% 로 줄여 중앙 배치
preview-compare.png   원형 마스크 비교 (왼쪽=원본 잘림 / 오른쪽=수정본)
```

## 워치는 왜 축소했나

watchOS 아이콘은 **원형으로 마스킹**된다. 원본 로고는 F·E·T·M 이 네 귀퉁이를
차지하고 마크가 가장자리에서 68px(6.6%) 밖에 안 떨어져 있어서, 중심에서
마크 모서리까지 628px 인데 원형 마스크 반경은 512px 다. **네 글자가 모두 잘린다.**

정사각 마크가 반경 R 원 안에 완전히 들어가려면 변 길이가 `2R/√2` 이하여야 한다.
여유를 조금 더 줘서 666px(65%) 로 축소하고 중앙에 놓았다.
`preview-compare.png` 에서 차이를 볼 수 있다.

## 알파 채널

원본에 알파 채널이 있었지만 전 픽셀이 불투명이라 실제로 쓰이지 않았다.
App Store 는 알파가 있는 아이콘을 반려하므로 RGB 로 평탄화했다.

## 다시 만들려면

```bash
python3 tools/make_icons.py       # 저장소 루트에서
```

## 앞으로 — Icon Composer (iOS 26)

iOS 26 부터는 Icon Composer 로 만드는 레이어드 아이콘(`.icon`)이 새 표준이다.
전경/중경/배경을 나눠 넣으면 Liquid Glass 재질과 다크·틴트 변형을 자동 생성하고
iPhone·iPad·Mac·Watch 에 하나로 대응한다.

`/Applications/Xcode.app/Contents/Applications/Icon Composer.app` 에 있다.
**CLI 가 없어 GUI 에서만 조립 가능하다.** 지금의 PNG 방식도 모든 OS 버전에서
정상 동작하므로 급하지 않다.
