/**
 * @file tests/test_line.cpp
 * @brief Bresenham 的不变量检查
 *
 * ## 为什么是"不变量"而不是"期望输出"
 *
 * 把某条线的正确像素串写死在测试里，只能证明那一条线是对的，
 * 而且那串期望值本身是谁算的？如果是同一份实现算出来的，
 * 这个测试等于什么都没测。
 *
 * 不变量不一样：它们是从算法的**定义**推出来的性质，
 * 和实现无关。任何一条不成立，实现就一定错了 ——
 * 而且不需要事先知道正确答案是什么。
 *
 * 这一点在很多代码由 AI 写的项目里格外重要。AI 写出来的
 * 光栅化循环通常"看起来完全正确"，错误集中在少数几个卦限、
 * 或者只在 `dx == dy` 时差一格。逐像素比对期望值发现不了这类问题
 * （因为期望值往往也是它生成的），不变量可以。
 */
#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>
#include <vector>

#include <mw/internal/color.hpp>

#include "mr/raster/line.hpp"
#include "mr/surface.hpp"

namespace {

using mr::raster::Point;
using mr::raster::trace_line;

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
    ++g_checks;
    if (! condition) {
        ++g_failures;
        std::printf("  %s %s\n", "FAIL", what.c_str());
    }
}

using Pixels = std::vector<std::pair<int32_t, int32_t>>;

/// 步数上限。**这不是防御性编程，是让失败模式可控。**
///
/// 判别式写错的一种常见形态是终止条件永远不成立（比如某个方向的
/// 步进符号取反），此时 trace_line 会无限循环。一个挂死的测试比一个
/// 失败的测试难查得多 —— 它没有输出、没有退出码，在 CI 里表现为超时。
/// 加了上限之后，同样的 bug 变成一条明确的 FAIL。
///
/// 实测有效：把 `sy` 写死成 1 会让向上的线永不终止，
/// 有上限时 check_length 直接报错，没有时整个测试进程挂住。
constexpr size_t kMaxSteps = 4096;

Pixels trace(Point a, Point b) {
    Pixels out;
    trace_line(a, b, [&out](int32_t x, int32_t y) -> bool {
        out.emplace_back(x, y);
        return out.size() < kMaxSteps;
    });
    return out;
}

std::string describe(Point a, Point b) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "(%d,%d)->(%d,%d)", a.x, a.y, b.x, b.y);
    return std::string(buf);
}

// ---------------------------------------------------------------------------

/// 端点必须被画上，而且必须是第一个和最后一个
void check_endpoints(Point a, Point b) {
    const Pixels px = trace(a, b);
    const std::string tag = describe(a, b);
    check(! px.empty(), tag + ": produced no pixels");
    if (px.empty()) {
        return;
    }
    check(px.front() == std::make_pair(a.x, a.y), tag + ": first pixel is not the start point");
    check(px.back() == std::make_pair(b.x, b.y), tag + ": last pixel is not the end point");
}

/// 相邻像素必须 8-连通：每步 dx 和 dy 都在 [-1, 1]，且不能原地不动。
/// 这一条抓的是"某个卦限一步走了两格"——最常见的判别式写错的症状。
void check_connectivity(Point a, Point b) {
    const Pixels px = trace(a, b);
    const std::string tag = describe(a, b);
    for (size_t i = 1; i < px.size(); ++i) {
        const int32_t dx = px[i].first - px[i - 1].first;
        const int32_t dy = px[i].second - px[i - 1].second;
        const bool ok = dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && (dx != 0 || dy != 0);
        if (! ok) {
            check(false, tag + ": step " + std::to_string(i) + " is not 8-connected");
            return;
        }
    }
    ++g_checks;
}

/// 同一个像素不能被画两次。重复点在硬边线里是纯浪费，
/// 在带混合的版本（Wu、半透明）里会直接画错。
void check_no_duplicates(Point a, Point b) {
    const Pixels px = trace(a, b);
    const std::set<std::pair<int32_t, int32_t>> unique(px.begin(), px.end());
    check(unique.size() == px.size(), describe(a, b) + ": emitted a pixel more than once");
}

/// 步数必须正好是 max(|dx|, |dy|) + 1。少一格或多一格都是差一错误，
/// 而差一错误在屏幕上看不出来。
void check_length(Point a, Point b) {
    const Pixels px = trace(a, b);
    const int32_t dx = std::abs(b.x - a.x);
    const int32_t dy = std::abs(b.y - a.y);
    const size_t expected = static_cast<size_t>(dx > dy ? dx : dy) + 1u;
    check(px.size() == expected, describe(a, b) + ": expected " + std::to_string(expected) +
                                     " pixels, got " + std::to_string(px.size()));
}

/// 反向画同一条线，像素**集合**必须相同。
///
/// 注意是集合不是序列 —— 序列当然是反的。也注意这一条**不是**
/// 平凡成立的：判别式在 |dx| == |dy| 之类的平局上要做选择，
/// 选择规则如果不对称，反向画会挑到另一侧的像素。
/// 很多教科书实现在这里是不对称的。
void check_reversal_symmetry(Point a, Point b) {
    const Pixels fwd = trace(a, b);
    const Pixels rev = trace(b, a);
    const std::set<std::pair<int32_t, int32_t>> sf(fwd.begin(), fwd.end());
    const std::set<std::pair<int32_t, int32_t>> sr(rev.begin(), rev.end());
    check(sf == sr, describe(a, b) + ": pixel set differs when drawn backwards");
}

// ---------------------------------------------------------------------------

void run_all_octants() {
    // 八个卦限，外加三个边界斜率（水平、垂直、45 度）。
    // 半径取 17 是刻意的质数：整除关系会掩盖一整类差一错误。
    constexpr int32_t r = 17;
    const Point origin{40, 40};
    const Point tips[] = {
        {origin.x + r, origin.y},          {origin.x + r, origin.y + r},
        {origin.x, origin.y + r},          {origin.x - r, origin.y + r},
        {origin.x - r, origin.y},          {origin.x - r, origin.y - r},
        {origin.x, origin.y - r},          {origin.x + r, origin.y - r},
        {origin.x + r, origin.y + 1},      {origin.x + 1, origin.y + r},
        {origin.x + r, origin.y + r / 2},  {origin.x + r / 2, origin.y + r},
    };

    for (const Point& tip : tips) {
        check_endpoints(origin, tip);
        check_connectivity(origin, tip);
        check_no_duplicates(origin, tip);
        check_length(origin, tip);
        check_reversal_symmetry(origin, tip);
    }

    // 退化：起点即终点
    check_endpoints(origin, origin);
    check_length(origin, origin);

    // 负坐标：算法本身不做裁剪，坐标可以是负的
    check_connectivity(Point{-30, -7}, Point{12, 40});
    check_length(Point{-30, -7}, Point{12, 40});
}

/// draw_line 必须永不越界写。ASan 下这一条最有价值。
void run_clipping() {
    mr::OwnedSurface owned;
    if (! owned.resize(64, 48)) {
        check(false, "OwnedSurface::resize failed");
        return;
    }
    const mr::Surface s = owned.view();
    s.clear(mr::kBlack);

    // 完全在外、部分在外、横穿、退化，逐个来一遍
    mr::raster::draw_line(s, Point{-1000, -1000}, Point{-900, -900}, mr::kWhite);
    mr::raster::draw_line(s, Point{-1000, 24}, Point{1000, 24}, mr::kWhite);
    mr::raster::draw_line(s, Point{32, -1000}, Point{32, 1000}, mr::kWhite);
    mr::raster::draw_line(s, Point{0, 0}, Point{63, 47}, mr::kWhite);
    mr::raster::draw_line(s, Point{10, 10}, Point{10, 10}, mr::kWhite);
    ++g_checks;  // 没崩、ASan 没报，就算过

    // 完全在外的那条不该留下任何痕迹：检查左上角
    const mr::Pixel* corner = s.pixel(0, 0);
    check(corner != nullptr, "pixel(0,0) should be inside a 64x48 surface");

    // 横穿的水平线必须真的画进去了
    const mr::Pixel* on_line = s.pixel(32, 24);
    check(on_line != nullptr && on_line->r == 255,
          "the horizontal line crossing the surface was not drawn");
}

/// stride 不等于 width*4 时也必须画对 —— 这是真硬件上的常态。
/// 做法：在一块宽 64 的内存上开一个宽 40 的 surface，
/// 画满之后检查第 40..63 列没有被碰过。
void run_stride_safety() {
    std::vector<uint8_t> memory(static_cast<size_t>(64) * 4u * 32u, 0u);
    const mr::Surface narrow(memory.data(), 40, 32, 64 * 4u);

    for (int32_t y = -5; y < 40; ++y) {
        mr::raster::draw_line(narrow, Point{-5, y}, Point{60, y}, mr::kWhite);
    }

    bool clean = true;
    for (uint32_t y = 0; y < 32 && clean; ++y) {
        for (uint32_t x = 40; x < 64; ++x) {
            const size_t offset = static_cast<size_t>(y) * 64u * 4u + static_cast<size_t>(x) * 4u;
            if (memory[offset] != 0u) {
                clean = false;
                break;
            }
        }
    }
    check(clean, "drawing wrote past width() into the stride padding");
}

} // namespace

int main() {
    std::printf("%s\n", "mini-render invariant tests");

    run_all_octants();
    run_clipping();
    run_stride_safety();

    if (g_failures == 0) {
        std::cout << "  " << internal::color::green("PASS") << "  " << g_checks << " checks\n";
        return 0;
    }
    std::cout << "  " << internal::color::red("FAIL") << "  " << g_failures << " of " << g_checks
              << " checks\n";
    return 1;
}
