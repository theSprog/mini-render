# third_party/

`mini-wayland` 应当是一个 git submodule，**钉在一个 tag 上**：

```sh
git submodule add <url> third_party/mini-wayland
cd third_party/mini-wayland && git checkout v0.3.0
```

不跟上游 `main`：mini-wayland 的 `mw/drm` 与 `mw/render` 在其
`docs/api.md` 里明确标了"会变"，Step 5/6/7 一定会改。
升级时手动动这个指针，顺便跑一遍 `make check`。

装出去之后 mini-render 靠 pkg-config 找它：

```sh
cd third_party/mini-wayland && make install PREFIX=$HOME/.local
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig
```
