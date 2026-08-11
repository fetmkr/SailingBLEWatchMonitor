#!/usr/bin/env python3
"""
brand/FETMMarine.png 에서 앱 아이콘을 생성한다.

    python3 tools/make_icons.py        # 저장소 루트에서 실행

왜 워치용을 따로 만드나:
watchOS 아이콘은 원형으로 마스킹된다. 원본 로고는 F·E·T·M 이 네 귀퉁이를
차지해서 그대로 쓰면 네 글자가 모두 잘린다. 마크만 잘라내 원 안에 들어가는
크기로 줄이고 중앙에 배치한다.
"""

import math
import pathlib
import sys

try:
    from PIL import Image, ImageChops, ImageDraw
except ImportError:
    sys.exit("PIL 이 필요합니다:  pip3 install pillow")

ROOT = pathlib.Path(__file__).resolve().parent.parent
BRAND = ROOT / "brand"
SRC = BRAND / "FETMMarine.png"

SIZE = 1024
BLUE = (0, 0, 255)
# 정사각 마크가 반경 R 원 안에 들어가려면 변 <= 2R/√2. 여유로 0.92 곱한다.
WATCH_FILL = 0.92


def white_mark_bbox(img: Image.Image):
    """파란 배경 위 흰 마크의 경계. 파랑은 R=G=0 이므로 R,G 로만 판정한다.
    (B 채널로 판정하면 배경도 255 라 전체가 잡힌다)"""
    r, g, _ = img.split()
    binarize = lambda ch: ch.point(lambda v: 255 if v > 128 else 0)
    return ImageChops.darker(binarize(r), binarize(g)).getbbox()


def main() -> int:
    if not SRC.exists():
        sys.exit(f"원본이 없습니다: {SRC}")

    src = Image.open(SRC).convert("RGB")  # 알파 제거 (App Store 는 알파 있으면 반려)
    if src.size != (SIZE, SIZE):
        src = src.resize((SIZE, SIZE), Image.LANCZOS)

    # ── iOS: 풀블리드 그대로
    src.save(BRAND / "icon-ios-1024.png")

    # ── watchOS: 마크만 잘라 원 안에 들어가게 축소
    bbox = white_mark_bbox(src)
    mark = src.crop(bbox)
    side = int((SIZE / 2) * math.sqrt(2) * WATCH_FILL)
    mark = mark.resize((side, side), Image.LANCZOS)

    watch = Image.new("RGB", (SIZE, SIZE), BLUE)
    watch.paste(mark, ((SIZE - side) // 2, (SIZE - side) // 2))
    watch.save(BRAND / "icon-watch-1024.png")

    # ── 검증: 원형 마스크를 실제로 씌워 원본과 나란히 비교
    def masked(img, box=320):
        out = Image.new("RGB", (SIZE, SIZE), (30, 30, 30))
        m = Image.new("L", (SIZE, SIZE), 0)
        ImageDraw.Draw(m).ellipse((0, 0, SIZE - 1, SIZE - 1), fill=255)
        out.paste(img, (0, 0), m)
        return out.resize((box, box), Image.LANCZOS)

    sheet = Image.new("RGB", (680, 340), (20, 20, 20))
    sheet.paste(masked(src), (10, 10))
    sheet.paste(masked(watch), (350, 10))
    sheet.save(BRAND / "preview-compare.png")

    # ── 에셋 카탈로그에 복사
    for plat, name in [("iOS", "icon-ios-1024.png"), ("Watch", "icon-watch-1024.png")]:
        dst = ROOT / "app" / plat / "Assets.xcassets" / "AppIcon.appiconset"
        dst.mkdir(parents=True, exist_ok=True)
        Image.open(BRAND / name).save(dst / "AppIcon-1024.png")

    cx, cy = SIZE / 2, SIZE / 2
    corner = max(math.hypot(x - cx, y - cy) for x in (bbox[0], bbox[2]) for y in (bbox[1], bbox[3]))
    print(f"원본 마크 경계 {bbox} — 모서리까지 {corner:.0f}px (원형 마스크 반경 {SIZE//2})")
    print(f"워치용 마크 {side}px ({side/SIZE*100:.0f}%) 로 축소 · 중앙 배치")
    print("생성: brand/icon-ios-1024.png, brand/icon-watch-1024.png, brand/preview-compare.png")
    print("에셋 카탈로그에 복사 완료")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
