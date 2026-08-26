#!/usr/bin/env python3
"""SailSight 마크를 아이콘 파일로 뽑는다.

마크가 전부 직선이라 SVG 를 래스터로 바꾸는 도구가 필요 없다. 선을 직접
그린다. 이 편이 결과가 확실하고, 맥에 뭘 더 깔 필요도 없다.

부드러운 가장자리는 4배로 크게 그린 뒤 줄여서 얻는다.

    python3 brand/make_icons.py
"""

import os
import shutil
import subprocess
from PIL import Image, ImageDraw

# ── 마크 모양. 242 칸짜리 정사각형 안의 자리다 ────────────────────────
V = 242.0
INSET, ARM = 40.0, 44.0          # 꺾쇠가 가장자리에서 떨어진 거리, 팔 길이
MAST_X, MAST_TOP, MAST_BOT = 140.0, 68.0, 174.0
LUFF_TOP = (140.0, 68.0)         # 돛 앞머리 — 위
LUFF_BOT = (90.0, 174.0)         # 돛 앞머리 — 아래
DASH_ON, DASH_OFF = 33.1, 9.0    # 세 토막으로 끊는 규칙

SS = 4                            # 4배로 그린 뒤 줄인다

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


SMALL = 56          # 이보다 작으면 꺾쇠를 뺀다. 44 에서도 꺾쇠가 돛에 닿았다


def stroke_for(px):
    """작을수록 선을 굵힌다. 안 그러면 선이 아예 사라진다."""
    if px >= 256: return 6.0
    if px >= 128: return 7.0
    if px >= 90:  return 8.5
    if px >= 64:  return 10.0
    if px >= 44:  return 12.0
    return 13.0


def render(px, mark_scale=1.0, bg=BLACK, fg=WHITE, alpha=True):
    """px 가 40 보다 작으면 꺾쇠를 빼고 돛만 크게 그린다.

    32 픽셀에서 꺾쇠를 다 넣으면 선이 서로 붙어 돛이 안 보인다. 눈으로
    확인하고 정한 경계다. 작은 자리에서는 알아보는 게 먼저다.
    """
    small = px < SMALL
    if small:
        mark_scale *= 1.55
    n = px * SS
    im = Image.new("RGBA" if alpha else "RGB", (n, n), bg + ((255,) if alpha else ()))
    d = ImageDraw.Draw(im)
    k = n / V
    c = V / 2

    # 돛만 그릴 때는 돛 자체의 한가운데를 기준으로 키운다.
    # 안 그러면 돛이 오른쪽 아래로 쏠린다.
    ox, oy = ((115.0, 121.0) if small else (c, c))

    def P(x, y):
        return ((c + (x - ox) * mark_scale) * k, (c + (y - oy) * mark_scale) * k)

    w = stroke_for(px) * mark_scale * k
    h = w / 2
    col = fg + ((255,) if alpha else ())

    def bar(x0, y0, x1, y1):
        """가로세로 막대. 네모로 그려야 꺾쇠 모서리가 각지게 채워진다."""
        (ax, ay), (bx, by) = P(x0, y0), P(x1, y1)
        d.rectangle([min(ax, bx) - h, min(ay, by) - h,
                     max(ax, bx) + h, max(ay, by) + h], fill=col)

    # 꺾쇠 넷 — 각 귀퉁이마다 가로 막대 하나와 세로 막대 하나
    if not small:
        i, j = INSET, V - INSET
        bar(i, i, i, i + ARM); bar(i, i, i + ARM, i)      # 왼쪽 위
        bar(j, i, j, i + ARM); bar(j - ARM, i, j, i)      # 오른쪽 위
        bar(j, j - ARM, j, j); bar(j - ARM, j, j, j)      # 오른쪽 아래
        bar(i, j - ARM, i, j); bar(i, j, i + ARM, j)      # 왼쪽 아래

    # 마스트 — 세로줄
    bar(MAST_X, MAST_TOP, MAST_X, MAST_BOT)

    # 돛 앞머리 — 끊긴 대각선. 토막마다 네모를 눕혀 그린다.
    # 이렇게 해야 토막 끝이 뭉툭하게 잘린다 (PIL 의 굵은 선은 끝이 삐져나온다)
    ax, ay = LUFF_TOP
    bx, by = LUFF_BOT
    L = ((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5
    ux, uy = (bx - ax) / L, (by - ay) / L
    px_, py_ = -uy * h, ux * h
    on, off = (L, 0.0) if small else (DASH_ON, DASH_OFF)
    t = 0.0
    while t < L - 0.01:
        t2 = min(t + on, L)
        s = P(ax + ux * t, ay + uy * t)
        e = P(ax + ux * t2, ay + uy * t2)
        d.polygon([(s[0] + px_, s[1] + py_), (e[0] + px_, e[1] + py_),
                   (e[0] - px_, e[1] - py_), (s[0] - px_, s[1] - py_)], fill=col)
        t = t2 + off if off else L

    return im.resize((px, px), Image.LANCZOS)


def write(path, im):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)
    print(f"  {os.path.relpath(path, ROOT):58} {im.size[0]}x{im.size[1]}")


# watchOS 아이콘 한 벌. (지름 pt, 배율, 쓰이는 곳, 화면 크기)
WATCH_SPEC = [
    (24,   2, "notificationCenter", "38mm"),
    (27.5, 2, "notificationCenter", "42mm"),
    (33,   2, "notificationCenter", "45mm"),
    (29,   2, "companionSettings",  None),   # ← 아이폰 워치 앱 목록이 읽는 자리
    (29,   3, "companionSettings",  None),
    (40,   2, "appLauncher", "38mm"),
    (44,   2, "appLauncher", "40mm"),
    (46,   2, "appLauncher", "41mm"),
    (50,   2, "appLauncher", "44mm"),
    (51,   2, "appLauncher", "45mm"),
    (54,   2, "appLauncher", "49mm"),
    (86,   2, "quickLook", "38mm"),
    (98,   2, "quickLook", "42mm"),
    (108,  2, "quickLook", "44mm"),
    (117,  2, "quickLook", "45mm"),
    (129,  2, "quickLook", "49mm"),
]


def watch_icons():
    import json
    d = os.path.join(ROOT, "app/Watch/Assets.xcassets/AppIcon.appiconset")
    for f in os.listdir(d):
        if f.endswith(".png"):
            os.remove(os.path.join(d, f))

    images = []
    for pt, sc, role, sub in WATCH_SPEC:
        px = int(round(pt * sc))
        fn = f"AppIcon-{px}.png"
        render(px, mark_scale=0.92, alpha=False).save(os.path.join(d, fn))
        e = {"filename": fn, "idiom": "watch", "role": role,
             "scale": f"{sc}x", "size": f"{pt:g}x{pt:g}"}
        if sub:
            e["subtype"] = sub
        images.append(e)

    render(1024, mark_scale=0.92, alpha=False).save(os.path.join(d, "AppIcon-1024.png"))
    images.append({"filename": "AppIcon-1024.png", "idiom": "watch-marketing",
                   "scale": "1x", "size": "1024x1024"})

    with open(os.path.join(d, "Contents.json"), "w") as f:
        json.dump({"images": images, "info": {"author": "xcode", "version": 1}},
                  f, indent=2)
    print(f"  {os.path.relpath(d, ROOT):58} {len(images)}장")


def main():
    # ── 아이폰 ────────────────────────────────────────────────────────
    # 앱스토어 아이콘은 투명한 곳이 있으면 안 된다. RGB 로 저장한다.
    print("아이폰")
    write(os.path.join(ROOT, "app/iOS/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png"),
          render(1024, alpha=False))

    # ── 워치 ──────────────────────────────────────────────────────────
    # 워치는 아이콘을 동그랗게 잘라낸다. 꺾쇠 귀퉁이가 잘리지 않게 살짝 줄인다.
    #
    # ★ 1024 한 장만 넣으면 안 된다.
    #   그렇게 하면 컴파일된 자산 목록에 그림이 딱 한 장만 들어간다. 워치
    #   본체는 그걸 줄여 쓰니 잘 보이는데, 아이폰의 워치 앱 목록은
    #   companionSettings 라는 따로 있는 자리(29pt 의 2배·3배, 즉 58 과 87)
    #   를 읽는다. 그 자리가 비어서 목록에 빈칸으로 나왔다.
    #   actool 로 직접 컴파일해서 확인했다.
    print("워치  (동그랗게 잘리므로 마크를 92% 로)")
    watch_icons()

    # ── 데스크탑 ──────────────────────────────────────────────────────
    print("데스크탑")
    ic = os.path.join(ROOT, "desktop/src-tauri/icons")
    for name, px in [
        ("32x32.png", 32), ("64x64.png", 64), ("128x128.png", 128),
        ("128x128@2x.png", 256), ("icon.png", 512),
        ("Square30x30Logo.png", 30), ("Square44x44Logo.png", 44),
        ("Square71x71Logo.png", 71), ("Square89x89Logo.png", 89),
        ("Square107x107Logo.png", 107), ("Square142x142Logo.png", 142),
        ("Square150x150Logo.png", 150), ("Square284x284Logo.png", 284),
        ("Square310x310Logo.png", 310), ("StoreLogo.png", 50),
    ]:
        write(os.path.join(ic, name), render(px))

    # 윈도우용 묶음
    ico = os.path.join(ic, "icon.ico")
    render(256).save(ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (256, 256)])
    print(f"  {os.path.relpath(ico, ROOT):58} 16·24·32·48·64·256")

    # 맥용 묶음. iconutil 이 폴더를 받아 icns 로 묶는다.
    iset = os.path.join(ic, "icon.iconset")
    shutil.rmtree(iset, ignore_errors=True)
    os.makedirs(iset)
    for nm, px in [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64),
                   ("128x128", 128), ("128x128@2x", 256), ("256x256", 256),
                   ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)]:
        render(px).save(os.path.join(iset, f"icon_{nm}.png"))
    subprocess.run(["iconutil", "-c", "icns", iset, "-o", os.path.join(ic, "icon.icns")],
                   check=True)
    shutil.rmtree(iset)
    print(f"  {os.path.relpath(os.path.join(ic, 'icon.icns'), ROOT):58} 16 부터 1024 까지")

    # ── 보기용 큰 그림 ────────────────────────────────────────────────
    write(os.path.join(ROOT, "brand/sailsight-1024.png"), render(1024))


if __name__ == "__main__":
    main()
