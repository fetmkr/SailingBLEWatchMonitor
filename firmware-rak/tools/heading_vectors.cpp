// 방위 식 교차 검증 벡터 — 보드(heading_tilt.h)가 낸 값을 파일로 쓴다.
// 데스크탑 앱(desktop/src/heading.ts)이 같은 입력으로 같은 값을 내는지 desktop/tools/heading_check.ts 가 본다.
//
//   빌드  c++ -std=c++17 -I../include -o heading_vectors heading_vectors.cpp
//   실행  ./heading_vectors out.tsv
//
// 한 줄: axisA axisB signA signB off decl | ax ay az | mx my mz | tilt flat   (못 구하면 -1)
#include <cmath>
#include <cstdio>

#include "heading_tilt.h"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "쓸 파일 이름을 주세요\n"); return 2; }
    FILE* fp = std::fopen(argv[1], "w");
    if (!fp) return 2;
    std::fprintf(fp, "# axisA axisB signA signB off decl ax ay az mx my mz tilt flat visible\n");

    // 결정적인 난수 (돌릴 때마다 같은 벡터)
    unsigned long seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245UL + 12345UL; return ((seed >> 8) & 0xFFFFFF) / (double)0xFFFFFF; };

    int n = 0;
    for (int a = 0; a < 3; ++a)
    for (int b = 0; b < 3; ++b)
    for (int sa = -1; sa <= 1; sa += 2)
    for (int sb = -1; sb <= 1; sb += 2) {
        if (a == b) continue;
        hdg::HeadingCfg c;
        c.axisA = (uint8_t)a; c.axisB = (uint8_t)b; c.signA = (float)sa; c.signB = (float)sb;
        c.offDeg = (float)(rnd() * 720.0 - 360.0);
        c.declDeg = (float)(rnd() * 20.0 - 10.0);
        for (int k = 0; k < 40; ++k) {
            // 가속: 방향은 아무렇게나, 크기는 0.8~1.2 g (일부는 1 g 문턱 밖 → -1)
            double ax = rnd() * 2 - 1, ay = rnd() * 2 - 1, az = rnd() * 2 - 1;
            double an = std::sqrt(ax * ax + ay * ay + az * az);
            if (an < 1e-3) continue;
            const double g = 0.8 + rnd() * 0.4;
            float acc[3] = { (float)(ax / an * g), (float)(ay / an * g), (float)(az / an * g) };
            float mag[3] = { (float)(rnd() * 100 - 50), (float)(rnd() * 100 - 50), (float)(rnd() * 100 - 50) };
            const float t = hdg::tiltHeadingDeg(acc, mag, c);
            const float f = hdg::flatHeadingDeg(mag, c);
            const float visible = hdg::tiltHeadingDeg(acc, mag, c, false);
            std::fprintf(fp, "%d %d %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
                         a, b, sa, sb, c.offDeg, c.declDeg, acc[0], acc[1], acc[2],
                         mag[0], mag[1], mag[2], t, f, visible);
            ++n;
        }
    }
    std::fclose(fp);
    std::printf("방위 벡터 %d줄\n", n);
    return 0;
}
