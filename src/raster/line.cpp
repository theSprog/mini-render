#include "mr/raster/line.hpp"

namespace mr::raster {

namespace {

/// Cohen–Sutherland 的 outcode，只用来做**平凡拒绝**，不做裁剪。
/// 纯整数比较，四个 bit。
constexpr int kLeft = 1;
constexpr int kRight = 2;
constexpr int kBottom = 4;
constexpr int kTop = 8;

int outcode(const Surface& surface, Point p) noexcept {
    int code = 0;
    if (p.x < 0) {
        code |= kLeft;
    } else if (p.x >= static_cast<int32_t>(surface.width())) {
        code |= kRight;
    }
    if (p.y < 0) {
        code |= kTop;
    } else if (p.y >= static_cast<int32_t>(surface.height())) {
        code |= kBottom;
    }
    return code;
}

} // namespace

void draw_line(const Surface& surface, Point a, Point b, Pixel color) noexcept {
    if (! surface.valid()) {
        return;
    }

    // 两个端点落在同一侧的屏幕外区域 => 整条线必然在屏幕外。
    // 这一句只挡住"完全在外面"的情况，挡不住"横穿屏幕但端点极远"的情况 ——
    // 后者需要真正的裁剪，见头文件里的说明。
    if ((outcode(surface, a) & outcode(surface, b)) != 0) {
        return;
    }

    trace_line(a, b, [&surface, color](int32_t x, int32_t y) noexcept {
        surface.put(x, y, color);
    });
}

} // namespace mr::raster
