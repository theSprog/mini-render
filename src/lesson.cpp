#include "mr/lesson.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace mr {

const Input& LessonParams::input() const noexcept {
    static const Input kNothingPressed{};
    return input_ptr != nullptr ? *input_ptr : kNothingPressed;
}

const char* to_string(Cadence cadence) noexcept {
    switch (cadence) {
        case Cadence::Static:
            return "static";
        case Cadence::Animated:
            return "animated";
        case Cadence::Progressive:
            return "progressive";
    }
    return "?";
}

namespace {

// 函数内 static：避免静态初始化顺序问题。注册发生在别的 TU 的静态初始化
// 期间，如果注册表是一个命名空间作用域的全局对象，它可能还没构造。
std::vector<Lesson>& registry() noexcept {
    static std::vector<Lesson> table;
    return table;
}

std::vector<const Lesson*>& sorted_view() noexcept {
    static std::vector<const Lesson*> view;
    return view;
}

} // namespace

namespace detail {

bool register_lesson(const Lesson& lesson) noexcept {
    if (lesson.id == nullptr || lesson.render == nullptr) {
        return false;
    }
    registry().push_back(lesson);
    return true;
}

} // namespace detail

LessonList lessons() noexcept {
    std::vector<const Lesson*>& view = sorted_view();
    if (view.size() != registry().size()) {
        view.clear();
        view.reserve(registry().size());
        for (const Lesson& lesson : registry()) {
            view.push_back(&lesson);
        }
        std::sort(view.begin(), view.end(), [](const Lesson* a, const Lesson* b) {
            return std::strcmp(a->id, b->id) < 0;
        });
    }
    return LessonList{view.data(), view.size()};
}

const Lesson* find_lesson(std::string_view id) noexcept {
    if (id.empty()) {
        return nullptr;
    }

    const Lesson* prefix_hit = nullptr;
    size_t prefix_count = 0;

    for (const Lesson* lesson : lessons()) {
        const std::string_view candidate(lesson->id);
        if (candidate == id) {
            return lesson;  // 精确匹配优先，不受前缀歧义影响
        }
        if (candidate.size() > id.size() && candidate.compare(0, id.size(), id) == 0) {
            prefix_hit = lesson;
            ++prefix_count;
        }
    }

    // 前缀歧义时返回 nullptr 而不是随便挑一个。`run 0` 命中五节课，
    // 静默跑其中一节比报错难查得多。
    return prefix_count == 1 ? prefix_hit : nullptr;
}

} // namespace mr