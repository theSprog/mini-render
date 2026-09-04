#include "mr/surface.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace mr {

void Surface::clear(Pixel color) const noexcept {
    if (! valid()) {
        return;
    }
    for (uint32_t y = 0; y < height_; ++y) {
        Pixel* row = at(0, static_cast<int32_t>(y));
        for (uint32_t x = 0; x < width_; ++x) {
            row[x] = color;
        }
    }
}

// ---------------------------------------------------------------------------

OwnedSurface::OwnedSurface() noexcept : surface_(), owned_(nullptr), capacity_(0) {}

OwnedSurface::~OwnedSurface() {
    std::free(owned_);
}

OwnedSurface::OwnedSurface(OwnedSurface&& other) noexcept
    : surface_(other.surface_),
      owned_(std::exchange(other.owned_, nullptr)),
      capacity_(std::exchange(other.capacity_, 0)) {
    other.surface_ = Surface{};
}

OwnedSurface& OwnedSurface::operator=(OwnedSurface&& other) noexcept {
    if (this != &other) {
        std::free(owned_);
        surface_ = other.surface_;
        owned_ = std::exchange(other.owned_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        other.surface_ = Surface{};
    }
    return *this;
}

bool OwnedSurface::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    const uint32_t stride = width * 4u;
    const size_t bytes = static_cast<size_t>(stride) * height;

    if (bytes > capacity_) {
        auto* fresh = static_cast<uint8_t*>(std::realloc(owned_, bytes));
        if (fresh == nullptr) {
            return false;
        }
        owned_ = fresh;
        capacity_ = bytes;
    }
    surface_ = Surface(owned_, width, height, stride);
    return true;
}

// ---------------------------------------------------------------------------

void blit(const Surface& dst, const Surface& src) noexcept {
    if (! dst.valid() || ! src.valid()) {
        return;
    }
    const uint32_t rows = dst.height() < src.height() ? dst.height() : src.height();
    const uint32_t cols = dst.width() < src.width() ? dst.width() : src.width();
    const size_t bytes = static_cast<size_t>(cols) * 4u;

    // 逐行拷贝，**不能**一次 memcpy 整块：两边的 stride 通常不同。
    // 一次拷整块在 offscreen 后端下恰好能跑（stride == width*4），
    // 上真硬件就是斜的 —— 这正是 stride 类 bug 的典型形态。
    for (uint32_t y = 0; y < rows; ++y) {
        std::memcpy(dst.at(0, static_cast<int32_t>(y)), src.at(0, static_cast<int32_t>(y)), bytes);
    }
}

} // namespace mr
