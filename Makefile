## mini-render -- classic graphics algorithms on top of mini-wayland
##
##   make                     debug build
##   make BUILD=release
##   make SANITIZE=1          ASan + UBSan
##   make test                run the invariant tests
##   make lint                enforce the lesson rules (grep-based, see scripts/lint.sh)
##   make check               check-headers + lint + test
##   make run LESSON=01       build then run offscreen
##   make V=1
##
## 设计说明：
##  - 非递归。lessons/ 下每个子目录 = 一节课，加目录即被构建，不用改这里。
##  - 告警集合与 mini-wayland 保持一致（**逐条对齐，不是"差不多"**）。
##    两个项目的代码会互相搬，告警不一致就意味着搬过去才发现编译不过。

PROJECT    := mini-render
BUILD      ?= debug
V          ?= 0
SANITIZE   ?= 0

O          := build/$(BUILD)$(if $(filter 1,$(SANITIZE)),-asan)

CXX        ?= g++
PKG_CONFIG ?= pkg-config

# ---------------------------------------------------------------------------
# 依赖：mini-wayland
# ---------------------------------------------------------------------------
# 通过 pkg-config 找。装在非标准前缀时设 PKG_CONFIG_PATH：
#   export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig
#
# **用 -isystem 引入**，理由和 mini-wayland 用 -isystem 引 libdrm 一样：
# 那是我们不改的代码，它的告警不该打断我们的构建。
# 副作用是 mw/ 头文件里的问题在这边看不见 —— 那是**故意的**，
# mini-wayland 有自己的 check-headers 负责那件事。

MW_PKG     := mini-wayland
MW_CFLAGS  := $(shell $(PKG_CONFIG) --cflags $(MW_PKG) 2>/dev/null | sed 's/-I/-isystem /g')
MW_LIBS    := $(shell $(PKG_CONFIG) --libs   $(MW_PKG) 2>/dev/null)

# ---------------------------------------------------------------------------
# 告警：与 mini-wayland 逐条一致
# ---------------------------------------------------------------------------
# 故意不加的两个：
#   -Wpedantic  : mini-wayland 的 TRY() 用了 GNU statement-expression
#   -Wpadded    : 噪音

WARN := -Wall -Wextra                       \
        -Weffc++                            \
        -Wconversion                        \
        -Wsign-conversion                   \
        -Wsign-compare                      \
        -Wold-style-cast                    \
        -Wzero-as-null-pointer-constant     \
        -Wshadow                            \
        -Wnon-virtual-dtor                  \
        -Woverloaded-virtual                \
        -Wcast-qual                         \
        -Wcast-align                        \
        -Wdouble-promotion                  \
        -Wformat=2                          \
        -Wundef                             \
        -Wmissing-declarations              \
        -Wredundant-decls                   \
        -Wno-unused-parameter

WARN += -Werror=return-type                 \
        -Werror=uninitialized               \
        -Werror=return-local-addr           \
        -Werror=unused-result               \
        -Werror=suggest-override            \
        -Werror=vla                         \
        -Werror=implicit-fallthrough

WARN += -Werror

CXXSTD   := -std=c++17
EXCFLAGS := -fno-exceptions

ifeq ($(BUILD),debug)
  OPT := -O0 -g3 -DMR_DEBUG=1
else ifeq ($(BUILD),release)
  # 光栅化是这个项目的主体，release 该真的快。-O2 起步，
  # 不开 -ffast-math：后面的课要讨论浮点精度，把它改掉会让讨论失去意义。
  OPT := -O2 -g -DNDEBUG
else
  $(error BUILD must be 'debug' or 'release', got '$(BUILD)')
endif

OPT += -fno-omit-frame-pointer

ifeq ($(SANITIZE),1)
  SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all
endif

INCLUDES := -Iinclude

CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(EXCFLAGS) $(SANFLAGS) \
            $(INCLUDES) $(MW_CFLAGS)                          \
            -ffunction-sections -fdata-sections -MMD -MP

LDFLAGS  := $(SANFLAGS) -rdynamic -Wl,--gc-sections
LDLIBS   := $(MW_LIBS) -lm

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
# 三组，分开是有原因的：
#
#   CORE_SRCS   —— src/ 下的公共代码 + harness
#   LESSON_SRCS —— lessons/*/ 下的课
#   TEST_SRCS   —— tests/ 下的不变量检查
#
# **lesson 的 .o 直接链进可执行文件，不打包成 .a。** MR_LESSON 的注册
# 依赖静态初始化，而链接器不会从静态库里拉一个没有任何符号被引用的
# 目标文件进来 —— 打成 .a 的话课会静默消失，`list` 里少几行，
# 没有任何报错。这个坑值得在这里写清楚而不是靠 --whole-archive 绕过去。

CORE_SRCS   := $(wildcard src/*.cpp) $(wildcard src/*/*.cpp)
LESSON_SRCS := $(wildcard lessons/*/*.cpp)
TEST_SRCS   := $(wildcard tests/*.cpp)

CORE_OBJS   := $(CORE_SRCS:%.cpp=$(O)/%.o)
LESSON_OBJS := $(LESSON_SRCS:%.cpp=$(O)/%.o)
TEST_OBJS   := $(TEST_SRCS:%.cpp=$(O)/%.o)

# harness 的 main 不能进测试可执行文件
MAIN_OBJ    := $(O)/src/main.o
LIBCORE_OBJS := $(filter-out $(MAIN_OBJ),$(CORE_OBJS))

BIN         := $(O)/bin/$(PROJECT)

# tests/ 下**每个 .cpp 一个可执行文件**，各自带自己的 main。
# 合成一个的话要么共用一个 main（那就得维护一张注册表），
# 要么链接冲突。分开还有个好处：某个测试挂了不影响别的跑完。
TEST_NAMES  := $(patsubst tests/%.cpp,%,$(TEST_SRCS))
TEST_BINS   := $(addprefix $(O)/bin/,$(TEST_NAMES))

HEADERS     := $(wildcard include/mr/*.hpp) $(wildcard include/mr/*/*.hpp)
ALL_SRCS    := $(CORE_SRCS) $(LESSON_SRCS) $(TEST_SRCS)
DEPS        := $(ALL_SRCS:%.cpp=$(O)/%.d)

ifeq ($(V),1)
  Q :=
  say = @true
else
  Q := @
  say = @printf '  %-8s %s\n' $(1) $(2)
endif

# ---------------------------------------------------------------------------

.PHONY: all clean distclean check check-headers check-deps lint test run help compile_commands.json

all: check-deps $(BIN)

$(BIN): $(CORE_OBJS) $(LESSON_OBJS)
	$(call say,LINK,$@)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

define TEST_RULE
$(O)/bin/$(1): $(O)/tests/$(1).o $$(LIBCORE_OBJS) $$(LESSON_OBJS)
	$$(call say,LINK,$$@)
	$$(Q)mkdir -p $$(dir $$@)
	$$(Q)$$(CXX) $$(LDFLAGS) -o $$@ $$^ $$(LDLIBS)
endef
$(foreach t,$(TEST_NAMES),$(eval $(call TEST_RULE,$(t))))

$(O)/%.o: %.cpp
	$(call say,CXX,$<)
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@

check-deps:
	$(Q)$(PKG_CONFIG) --exists $(MW_PKG) || { \
		echo "error: mini-wayland not found by pkg-config"; \
		echo "  build and install it first:"; \
		echo "    cd third_party/mini-wayland && make install PREFIX=\$$HOME/.local"; \
		echo "  then:"; \
		echo "    export PKG_CONFIG_PATH=\$$HOME/.local/lib/pkgconfig"; \
		exit 1; \
	}

check-headers:
	$(Q)mkdir -p $(O)/hdrchk
	$(Q)fail=0; \
	for h in $(HEADERS); do \
		printf '  %-8s %s\n' 'HDRCHK' "$$h"; \
		echo "#include \"$${h#include/}\"" > $(O)/hdrchk/tu.cpp; \
		$(CXX) $(CXXSTD) $(WARN) $(EXCFLAGS) $(INCLUDES) $(MW_CFLAGS) \
			-fsyntax-only $(O)/hdrchk/tu.cpp || fail=1; \
	done; \
	exit $$fail

compile_commands.json:
	$(Q)printf '[\n' > $@
	$(Q)first=1; for s in $(ALL_SRCS); do \
		if [ $$first -eq 0 ]; then printf ',\n' >> $@; fi; first=0; \
		printf '  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
			"$(CURDIR)" "$$s" "$(CXX)" "$(CXXFLAGS)" "$$s" >> $@; \
	done
	$(Q)printf '\n]\n' >> $@
	$(call say,GEN,$@)

lint:
	$(Q)./scripts/lint.sh

test: $(TEST_BINS)
	$(Q)fail=0; \
	for t in $(TEST_BINS); do \
		$$t || fail=1; \
	done; \
	exit $$fail

check: check-headers lint test

run: all
	$(Q)$(BIN) run $(or $(LESSON),01) -f $(or $(FRAMES),120)

clean:
	$(Q)rm -rf build

distclean: clean
	$(Q)rm -f compile_commands.json

help:
	@sed -n '1,14p' Makefile | sed 's/^## \{0,1\}//'

-include $(DEPS)