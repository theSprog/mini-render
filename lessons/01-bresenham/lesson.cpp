/**
 * @file lessons/01-bresenham/lesson.cpp
 * @brief 第一课：Bresenham 画线
 *
 * 算法本身在 `mr/raster/line.hpp`，那里写了判别式是怎么推出来的。
 * 这个文件只负责**把它画成一个人能看出对错的画面**。
 *
 * 画面分三部分，每一部分对应一类容易出错的地方：
 *
 *  1. **辐条轮盘**。从中心向 360 个方向各画一条等长的线。
 *     八个卦限全覆盖。哪个卦限的公式写错了，轮盘上就会缺一块、
 *     多一块，或者某一段的密度明显不同 —— 一眼可见，
 *     不需要逐像素比对。
 *  2. **接近水平/垂直/45 度的一束线**。这三个方向是判别式的边界情况：
 *     `dy == 0`、`dx == 0`、`dx == dy`。45 度那条尤其值得盯 ——
 *     两个判定共用同一个 `e2` 这件事只在这里体现得出来。
 *  3. **一条穿出屏幕的长线**。端点在画布外，验证裁剪路径没把
 *     线画歪、也没崩。
 *
 * 动画部分是让整个图案缓慢旋转。旋转角本身用了浮点（`sin`/`cos`
 * 算端点），**但那是在算端点，不在光栅化里** —— 线一旦有了两个
 * 整数端点，从那里到像素之间没有任何浮点。这条界线是这一课的重点，
 * 不要因为看见 `cos` 就以为算法用了浮点。
 *
 * ## 键盘
 *
 * 键来自**运行这个程序的那个终端**（通常是 ssh 会话）。板子上那套键鼠
 * 连着板子自己的 tty，敲了这个程序收不到 —— 见 `mr/input.hpp` 开头。
 *
 *   space     暂停 / 继续
 *   ←  →      调转速（含反转）
 *   ↑  ↓      加 / 减辐条数
 *   r         复位
 *
 * 屏幕左下角有一排刻度显示当前转速档位。这是**唯一的反馈通道** ——
 * 人在 ssh 终端里，眼睛在板子的屏幕上，所以状态必须画在画面上，
 * 打日志是看不见的（低头看终端就错过画面了）。
 */
#include <cmath>

#include "mr/lesson.hpp"
#include "mr/raster/line.hpp"

namespace {

using mr::Pixel;
using mr::Surface;
using mr::raster::draw_line;
using mr::raster::Point;

/// 按角度取一个渐变色，让相邻辐条能分开
Pixel spoke_color(int index) noexcept {
    const auto phase = static_cast<uint8_t>(index * 7);
    return mr::rgb(static_cast<uint8_t>(255 - phase), static_cast<uint8_t>(phase),
                   static_cast<uint8_t>(128 + phase / 2));
}

void draw_wheel(const Surface& out, Point center, int32_t radius, double rotation,
                int spokes) noexcept {
    for (int i = 0; i < spokes; ++i) {
        const double angle = rotation + (2.0 * M_PI * i) / spokes;

        // 端点在这里算，用浮点。**光栅化不用。**
        const Point tip{center.x + static_cast<int32_t>(std::lround(std::cos(angle) * radius)),
                        center.y + static_cast<int32_t>(std::lround(std::sin(angle) * radius))};
        draw_line(out, center, tip, spoke_color(i));
    }
}

/// 三个边界斜率各画一束：接近水平、接近垂直、正好 45 度
void draw_edge_cases(const Surface& out, int32_t x0, int32_t y0, int32_t span) noexcept {
    const Pixel horizontal = mr::rgb(255, 80, 80);
    const Pixel vertical = mr::rgb(80, 255, 80);
    const Pixel diagonal = mr::rgb(255, 255, 80);

    for (int32_t k = 0; k <= 4; ++k) {
        // dy = 0 是纯水平；dy = 1..4 是"几乎水平"，最容易出现阶梯位置错位
        draw_line(out, Point{x0, y0 + k * 6}, Point{x0 + span, y0 + k * 6 + k}, horizontal);
        draw_line(out, Point{x0 + k * 6, y0 + 40}, Point{x0 + k * 6 + k, y0 + 40 + span},
                  vertical);
    }
    // 正好 45 度：dx == dy，两个判定同时命中的唯一情况
    draw_line(out, Point{x0, y0 + 40}, Point{x0 + span, y0 + 40 + span}, diagonal);
}

/// 左下角的一排刻度：当前转速档位。画在**画面上**而不是打日志，
/// 因为操作者的眼睛在板子的屏幕上，不在终端里。
void draw_speed_gauge(const Surface& out, int level, bool paused) noexcept {
    const auto h = static_cast<int32_t>(out.height());
    const int32_t base_y = h - 14;
    const Pixel on = paused ? mr::rgb(200, 120, 60) : mr::rgb(120, 220, 120);
    const Pixel off = mr::rgb(50, 50, 60);

    for (int slot = 0; slot < 9; ++slot) {
        const int32_t x = 16 + slot * 8;
        const bool lit = (slot - 4) == level || (level > 0 && slot > 4 && slot - 4 <= level) ||
                         (level < 0 && slot < 4 && slot - 4 >= level) || slot == 4;
        draw_line(out, Point{x, base_y}, Point{x, base_y + 8}, lit ? on : off);
    }
}

/// 跨帧状态。课是自由函数，所以状态只能放这里。
///
/// 目前只有一节课需要状态，所以**不**为此往 Lesson 结构体里加
/// "创建/销毁上下文"那一套 —— 一个抽象在只有一个使用者时不要建。
/// 第二节需要状态的课出现时再抽。
double g_angle = 0.0;
int g_speed_level = 1;
int g_spokes = 180;
bool g_paused = false;

void handle_input(const mr::Input& in) noexcept {
    if (in.pressed(mr::Key::Space)) {
        g_paused = ! g_paused;
    }
    if (in.pressed(mr::Key::Right)) {
        g_speed_level = g_speed_level < 4 ? g_speed_level + 1 : 4;
    }
    if (in.pressed(mr::Key::Left)) {
        g_speed_level = g_speed_level > -4 ? g_speed_level - 1 : -4;
    }
    if (in.pressed(mr::Key::Up)) {
        g_spokes = g_spokes < 720 ? g_spokes * 2 : 720;
    }
    if (in.pressed(mr::Key::Down)) {
        g_spokes = g_spokes > 8 ? g_spokes / 2 : 8;
    }
    if (in.pressed(mr::key('r'))) {
        g_angle = 0.0;
        g_speed_level = 1;
        g_spokes = 180;
        g_paused = false;
    }
}

void render(const Surface& out, const mr::LessonParams& params) {
    handle_input(params.input());

    if (! g_paused) {
        // 用 dt 积分，不用 params.time_s —— 暂停之后再继续，
        // 角度必须从停下的地方接着走，而不是跳到"如果一直转会到哪"。
        g_angle += params.dt_s * 0.15 * g_speed_level;
    }

    out.clear(mr::rgb(12, 12, 18));

    const auto w = static_cast<int32_t>(out.width());
    const auto h = static_cast<int32_t>(out.height());

    const Point center{w / 2, h / 2};
    const int32_t radius = (w < h ? w : h) / 2 - 20;
    draw_wheel(out, center, radius > 8 ? radius : 8, g_angle, g_spokes);

    draw_edge_cases(out, 16, 16, (w / 4) > 32 ? (w / 4) : 32);

    // 两个端点都在画布外，中间穿过屏幕。裁剪路径的验收。
    draw_line(out, Point{-w, h / 3}, Point{2 * w, h / 3 + h / 5}, mr::rgb(90, 90, 255));

    draw_speed_gauge(out, g_speed_level, g_paused);
}

} // namespace

MR_LESSON("01-bresenham", "Bresenham line drawing", 1962, mr::Cadence::Animated, render);