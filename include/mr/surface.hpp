/**
 * @file mr/surface.hpp
 * @brief 一块可写的像素平面 —— 本项目与 mini-wayland 之间唯一的接缝
 *
 * ## 为什么不直接用 `mw::display::Frame`
 *
 * 算法代码直接吃上游类型会带来三个后果，每一个都会在几个月后发作：
 *
 *  1. **算法没法脱离显示库单测。** 想验证 Bresenham 画出的像素集合，
 *     就得先开一个 Screen。而那个测试本该只需要一块 `std::vector<uint8_t>`。
 *  2. **上游改了 `Frame`，每个 lesson 都要改。** mini-wayland 的
 *     `mw/render` 与 `mw/drm` 明确标了"会变"（见其 `docs/api.md`），
 *     Step 6 还会给 buffer 加 fence 字段。
 *  3. **`stride` 会被忘掉。** 这是本项目最容易犯、也最难一眼看出的错：
 *     用 `width * 4` 当行跨距，在 offscreen 后端下恰好是对的，
 *     上真硬件立刻画面倾斜。把 stride 做成 `Surface` 的独立字段，
 *     并且只提供 `pixel()` 这一个定位入口，是让人根本没机会写错。
 *
 * 所以 `mr::Surface` 是一个**借用视图**：不拥有内存，不知道内存从哪来。
 * 从 `mw::display::Frame` 转换过来是 `from_frame()` 一行，
 * **那一行是全项目唯一认识 `mw::` 的地方**（除了 harness 本身）。
 *
 * ## 像素格式
 *
 * 只支持 32 位 packed，字节序由 `Pixel` 的字段顺序写死。
 * 不做格式抽象 —— 一个只会有一种格式的项目里，格式抽象层是纯粹的负担。
 * 真的需要第二种格式时再说，那时改这一个文件。
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace mr {

/**
 * @brief 一个像素
 *
 * **字段顺序就是内存里的字节顺序。** 小端机器上的 DRM `XRGB8888`
 * 在内存里是 B, G, R, X —— 不是 X, R, G, B。这一点几乎每个人第一次
 * 都会搞反，症状是红蓝互换，而红蓝互换的画面看起来"只是配色怪"，
 * 不像 bug，所以能瞒很久。
 *
 * 想验证的话：把整块填成 `rgb(255, 0, 0)`，屏幕应该是红的，不是蓝的。
 */
struct Pixel {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t x = 0;  ///< XRGB 里的 X，无意义；ARGB 时是 alpha
};

static_assert(sizeof(Pixel) == 4, "Pixel must be exactly one 32-bit texel");

constexpr Pixel rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return Pixel{b, g, r, 0};
}

constexpr Pixel kBlack = rgb(0, 0, 0);
constexpr Pixel kWhite = rgb(255, 255, 255);

/**
 * @brief 借用的可写像素平面
 *
 * 不拥有内存。有效期由提供者决定 —— 从 `Frame` 转来的话，
 * 到下一次 `begin_frame()` 为止。
 *
 * @warning 真实 scanout buffer 的内存**很可能是写合并甚至非缓存的**：
 *          顺序写正常，读回慢几十倍。需要 read-modify-write 的算法
 *          （混合、抗锯齿、深度测试）应当在自己的堆内存里算完再整块拷进来。
 *          `OwnedSurface` 就是给这个用的。
 */
class Surface {
  public:
    constexpr Surface() noexcept = default;

    constexpr Surface(uint8_t* data, uint32_t width, uint32_t height, uint32_t stride) noexcept
        : data_(data), width_(width), height_(height), stride_(stride) {}

    constexpr uint32_t width() const noexcept {
        return width_;
    }
    constexpr uint32_t height() const noexcept {
        return height_;
    }

    /// 行跨距，字节。**不等于** `width() * 4`。定位像素只能用它。
    constexpr uint32_t stride() const noexcept {
        return stride_;
    }

    constexpr bool valid() const noexcept {
        return data_ != nullptr && width_ != 0 && height_ != 0 && stride_ >= width_ * 4u;
    }

    constexpr bool contains(int32_t x, int32_t y) const noexcept {
        return x >= 0 && y >= 0 && static_cast<uint32_t>(x) < width_ &&
               static_cast<uint32_t>(y) < height_;
    }

    /// 越界返回 nullptr。热路径上确认过范围的话用 `at()`。
    Pixel* pixel(int32_t x, int32_t y) const noexcept {
        return contains(x, y) ? at(x, y) : nullptr;
    }

    /// **不做边界检查。** 调用方必须先 `contains()`。
    Pixel* at(int32_t x, int32_t y) const noexcept {
        return reinterpret_cast<Pixel*>(data_ + static_cast<size_t>(y) * stride_ +
                                        static_cast<size_t>(x) * 4u);
    }

    /// 越界静默丢弃 —— 光栅化里越界是常态，不是错误
    void put(int32_t x, int32_t y, Pixel color) const noexcept {
        if (contains(x, y)) {
            *at(x, y) = color;
        }
    }

    void clear(Pixel color) const noexcept;

    uint8_t* data() const noexcept {
        return data_;
    }

  private:
    uint8_t* data_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t stride_ = 0;
};

/**
 * @brief 自己带内存的 Surface
 *
 * 两个用途：
 *  - **单测**。不需要显示设备就能验证算法画出的像素。
 *  - **累积**。渐进式算法（路径追踪）在这里攒结果，每帧整块拷到屏幕上。
 *
 * stride 刻意**不做对齐**。模仿一个"差不多像硬件"的对齐值只会让人
 * 以为自己处理好了对齐，而其实只是恰好对上了 —— 对齐问题必须上真硬件验。
 */
class OwnedSurface {
  public:
    OwnedSurface() noexcept;
    ~OwnedSurface();

    OwnedSurface(OwnedSurface&&) noexcept;
    OwnedSurface& operator=(OwnedSurface&&) noexcept;
    OwnedSurface(const OwnedSurface&) = delete;
    OwnedSurface& operator=(const OwnedSurface&) = delete;

    /// 尺寸不变时不重新分配，直接复用。返回是否可用。
    bool resize(uint32_t width, uint32_t height);

    Surface view() const noexcept {
        return surface_;
    }

  private:
    Surface surface_{};
    uint8_t* owned_ = nullptr;
    size_t capacity_ = 0;
};

/// 整块拷贝。尺寸不一致时拷贝重叠区域。
void blit(const Surface& dst, const Surface& src) noexcept;

} // namespace mr
