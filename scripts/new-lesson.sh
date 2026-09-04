#!/bin/sh
# scripts/new-lesson.sh <NN-slug> "<Title>" <year> <static|animated|progressive>
#
# 生成一节新课的骨架。存在的理由不是省打字 —— 是**让每节课的起点完全一样**。
#
# 这个项目里很多课会由 AI 写。AI 从零起手时会做的第一件事是模仿它看到的
# 上一个例子，而如果每节课的骨架都略有不同（有的手写 argv、有的自己开屏幕、
# 有的 render 签名不一样），那些差异会被一路复制下去，而且越往后越难收敛。
#
# 骨架统一之后，AI 要写的就只剩算法本身 —— 那正是它该写的部分。

set -eu

if [ $# -lt 4 ]; then
    echo "usage: $0 <NN-slug> \"<Title>\" <year> <static|animated|progressive>"
    echo "example: $0 02-wu-lines \"Xiaolin Wu antialiased lines\" 1991 animated"
    exit 2
fi

id=$1
title=$2
year=$3
cadence=$4

case "$id" in
    [0-9][0-9]-*) ;;
    *) echo "error: id must look like 'NN-slug' (two digits, dash, slug)"; exit 2 ;;
esac

case "$cadence" in
    static)      cad_enum="mr::Cadence::Static" ;;
    animated)    cad_enum="mr::Cadence::Animated" ;;
    progressive) cad_enum="mr::Cadence::Progressive" ;;
    *) echo "error: cadence must be static, animated or progressive"; exit 2 ;;
esac

dir="lessons/$id"
if [ -e "$dir" ]; then
    echo "error: $dir already exists"
    exit 1
fi

mkdir -p "$dir"

cat > "$dir/lesson.cpp" <<EOF
/**
 * @file lessons/$id/lesson.cpp
 * @brief $title ($year)
 *
 * TODO: 这节课在讲什么问题，为什么当年是这么解的。
 *       算法本身应当放进 include/mr/ 下的某个头文件，这里只负责
 *       把它画成一个人能看出对错的画面。
 */
#include "mr/lesson.hpp"
#include "mr/surface.hpp"

namespace {

void render(const mr::Surface& out, const mr::LessonParams& params) {
    out.clear(mr::rgb(12, 12, 18));
    (void)params;

    // TODO
}

} // namespace

MR_LESSON("$id", "$title", $year, $cad_enum, render);
EOF

cat > "$dir/README.md" <<EOF
# $id — $title ($year)

## 问题

TODO: 这节课要解决的是什么。

## 想法

TODO: 关键洞察是什么。为什么当年会想到这么做。

## 和上一课的关系

TODO

## 画面怎么看

TODO: 画面上每一块对应哪个容易出错的地方；错了会长什么样。

## 不变量

TODO: \`tests/\` 里为这节课加了哪些性质检查，各自能抓到什么形态的 bug。

\`\`\`sh
mini-render run $id -f 300
sudo mini-render run $id -b kms
\`\`\`
EOF

echo "created $dir/lesson.cpp and $dir/README.md"
echo
echo "next:"
echo "  1. 算法写进 include/mr/... 与 src/...，不要堆在 lesson.cpp 里"
echo "  2. 不变量写进 tests/，然后**故意写错一次**确认测试能抓到"
echo "  3. make check   (check-headers + lint + test)"
echo "  4. make run LESSON=$id"
