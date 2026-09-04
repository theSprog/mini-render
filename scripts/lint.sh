#!/bin/sh
# scripts/lint.sh -- 把 mr/lesson.hpp 里的约定变成构建期的硬失败
#
# 为什么用 grep 而不是"约定俗成"：
#
# mini-wayland 那边有四个现成的工具头（parse_args / signal / env / color）
# 躺了很久没人用。事后查出来原因是它们编译不过那边的告警集合 ——
# 于是每一次"用一下现成的"都以撞墙告终，然后退回去手写。
# 人和 AI 都会做同样的选择，而且都不会留下记录。
#
# 结论是：**光把现成的修好还不够，还得让"手写"这条路走不通。**
# 下面每一条都对应一个已经现成存在的替代品。
#
# 这个脚本刻意只用 POSIX sh + grep：它必须在任何机器上都能跑，
# 包括那块网络不稳、装不上东西的开发板。

set -u

# 字节序列比较必须在 C locale 下做，否则 [^ -~] 这类范围会按当前 locale
# 的排序规则解释，把中文以外的东西也匹配进来（实测在 UTF-8 locale 下
# 这条规则会命中每一行带引号的代码）。
LC_ALL=C
export LC_ALL

fail=0

# 注释行过滤器：只看代码，不看注释。
# 本项目的注释是中文的，而其中一条规则（字符串字面量必须英文）会把
# 带引号的中文注释误判成违规 —— 第一次跑就撞上了。
strip_comments() {
    grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|\*|/\*)'
}

# $1 = 正则  $2 = 搜索路径（**必须整体加引号**，多个路径用空格分隔）
# $3 = 人话解释
#
# where 不加引号的话 shell 会把它拆成多个位置参数，reason 就变成了第二个
# 路径 —— 表现是报错信息里打出一个目录名。同样是第一次跑就撞上了，
# 记在这里因为它下次还会发生。
forbid() {
    pattern=$1
    where=$2
    reason=$3

    # 只看 C++ 源文件。不加 --include 的话 README.md 里的中文会被
    # "字符串字面量必须英文"那条命中 —— 第一次加课时就撞上了。
    # shellcheck disable=SC2086
    hits=$(grep -rnE --include='*.cpp' --include='*.hpp' "$pattern" $where 2>/dev/null \
           | strip_comments)
    if [ -n "$hits" ]; then
        printf '  %-8s %s\n' 'LINT' "FAIL: $reason"
        echo "$hits" | sed 's/^/           /'
        fail=1
    fi
}

# ---------------------------------------------------------------------------
# 课不许做的事
# ---------------------------------------------------------------------------
# 每一条都不是风格洁癖：违反任何一条都会让这节课没法在单测里跑
# （单测里没有 argv，也没有屏幕），或者让它和别的课行为不一致。

forbid 'int[[:space:]]+main[[:space:]]*\(' 'lessons/' \
    'a lesson must not define main(); the harness owns it (src/main.cpp)'

forbid '\bargc\b|\bargv\b' 'lessons/' \
    'a lesson must not touch argv; add an option to the harness instead'

forbid '\bgetenv\b' 'lessons/' \
    'a lesson must not read the environment; the harness parses it (internal::env)'

forbid '\bsigaction\b|\bsignal[[:space:]]*\(' 'lessons/' \
    'a lesson must not install signal handlers; the harness does (internal::sig::guard)'

forbid '#include[[:space:]]*<mw/' 'lessons/ include/mr/' \
    'only src/main.cpp may include <mw/...>; lessons speak mr::Surface'

forbid '\\033\[|\\x1b\[' 'lessons/ src/ include/' \
    'do not hand-write ANSI escapes; use internal::color'

# ---------------------------------------------------------------------------
# 全项目
# ---------------------------------------------------------------------------

# stride 是本项目最容易犯、最难一眼看出的错：用 width*4 当行跨距，
# offscreen 后端下恰好是对的，上真硬件立刻画面倾斜。
forbid 'width\(\)[[:space:]]*\*[[:space:]]*4' 'lessons/ src/raster/' \
    'do not compute a row offset from width * 4; use Surface::stride() / at()'

# 字符串字面量一律英文，和 mini-wayland 一致（注释可以中文）。
# 只查明显的：字面量里出现 CJK 字符。
# `[^ -~]` 在 C locale 下 = 任何非可打印 ASCII 字节，也就是中文（以及
# 任何别的非 ASCII）。只查**双引号之间**的部分。
forbid '"[^"]*[^ -~][^"]*"' 'lessons/ src/ include/' \
    'string literals must be English (comments may be Chinese)'

# ---------------------------------------------------------------------------
# 反向检查：现成的东西有没有真的被用上
# ---------------------------------------------------------------------------
# 上面全是禁令。禁令能挡住手写，但挡不住"干脆不做"——
# 比如 harness 根本不解析参数。所以再正向查一遍。

require() {
    pattern=$1
    where=$2
    reason=$3

    # shellcheck disable=SC2086
    if ! grep -rqE "$pattern" $where 2>/dev/null; then
        printf '  %-8s %s\n' 'LINT' "FAIL: $reason"
        fail=1
    fi
}

require 'internal::parse_args' src/main.cpp \
    'the harness must parse arguments with internal::parse_args'
require 'internal::sig::' src/main.cpp \
    'the harness must install signal handlers with internal::sig'
require 'ENV_SCHEMA' src/main.cpp \
    'the harness must read the environment with internal::env'
require 'internal::color' src/main.cpp \
    'the harness must colourise terminal output with internal::color'

# ---------------------------------------------------------------------------

if [ "$fail" -eq 0 ]; then
    printf '  %-8s %s\n' 'LINT' 'ok'
fi
exit $fail
