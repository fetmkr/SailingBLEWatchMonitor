# 참고 문서

인터넷에서 다시 찾지 않아도 되게 원본을 여기 둔다. `.txt` 는 `pdftotext -layout`
으로 뽑은 것이라 `grep` 으로 바로 뒤질 수 있다.

| 파일 | 무엇 |
|---|---|
| `gps/Quectel_L76K_protocol_v1.1.pdf` | **우리 모듈의 정본.** PCAS 명령과 CASIC 바이너리 일부 |
| `gps/CASIC_protocol_en.pdf` | 칩(URANUS5 / AT6558 계열) 원본. L76K 문서에 없는 게 많다 |
| `board/RAK19007_datasheet.pdf` | 베이스보드 핀 배치 |

## 이 문서들을 볼 때 알아둘 것

**L76K 문서는 칩이 알아듣는 것의 일부만 적어 놨다.** 문서에 없는 `PCAS06` 이
실제로 응답했고, `CFG-NAVX` 도 마찬가지다. 없다고 단정하기 전에 두드려 볼 것.

**체크섬 공식이 두 문서에서 서로 다르다. L76K 쪽이 맞다** (실기기로 확인).

```
L76K   Checksum = (ID    << 24) + (Class << 16) + Len     ← 이게 먹힌다
CASIC  ckSum    = (class << 24) + (id    << 16) + len     ← 오타
```

펌웨어는 둘 다 시도해 보고 되는 쪽을 기억한다 (`casicQuery`).

**설정이 걸렸는지는 짐작하지 말고 되물어볼 것.** 시리얼에서 `gpscfg` 를 치면
모듈이 답한 실제 값이 나온다. 보낸 것과 걸린 것은 다를 수 있다.
