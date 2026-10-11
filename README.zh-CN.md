**[English](README.md) | 简体中文**

# PsnineTrophies4Vita

一款运行在已破解 PlayStation Vita 上的**只读** PSN 奖杯浏览器，中文界面，视觉与交互
对标系统原生奖杯 app。奖杯数据实时来自 PSN 中文站 [psnine.com](https://psnine.com)。

## 1. 功能

- **游戏搜索** — 按标题模糊搜索 psnine 游戏库，列表带封面、平台标
  （PS5/PS4/PS3/PSV/PC）、白金/金/银/铜数量与玩过人数。
- **奖杯列表** — 本体与 DLC 奖杯组分开展示，每条奖杯含等级图标、原名与中文名、
  达成条件、稀有度和 Tips 条数，等级图标与原生奖杯 app 一致。
- **奖杯 Tips** — 玩家撰写的解锁攻略，长文提供全文阅读页。
- **原生手感** — 五级页面导航（搜索 → 游戏 → 奖杯组 → 奖杯 → Tips → 全文）、
  选中光带、惯性甩动滚动、过顶回弹分离动画、屏幕按键提示条、系统状态栏，
  LiveArea 气泡与启动画面同样按第一方风格定制。
- **严格只读** — 无需登录、不能发布、不向站点写入任何数据；触屏与实体按键均可操作。

需要已安装 HENkaku / taiHEN 与 VitaShell 的破解 PS Vita。

## 2. 快速安装

1. 从 [Releases 页面](../../releases/latest) 下载最新版 `PsnineTrophies4Vita.vpk`。
2. 在 PS Vita 上打开 **VitaShell**，按 **select** 启动 FTP 服务，屏幕会显示地址，
   如 `ftp://192.168.xxx.xxx:1337`。
3. 在电脑上用任意 FTP 工具上传（curl 注意分区冒号要写成 `%3a`）：

   ```sh
   curl -T PsnineTrophies4Vita.vpk "ftp://<VITA-IP>:1337/ux0%3a/downloads/"
   ```

4. 回到 VitaShell，进入 `ux0:/downloads/` 选中 `.vpk` 按 ✕ 安装。
   「This package requests extended permissions」提示对所有自制软件都会出现，属正常，
   确认即可。同 titleID 重复安装会直接覆盖旧版，无需先卸载。
5. 回到 LiveArea，点气泡启动 **PsnineTrophies4Vita**。

## 3. 编译指南

唯一的硬依赖是 [vitasdk](https://vitasdk.org) 工具链，最省事的方式是官方 Docker 镜像，
仓库自带的 `.devcontainer/` 已钉好版本（`vitasdk/vitasdk:2026.08`）。

```sh
# 方式 A：支持 devcontainer 的编辑器（如 VS Code）直接打开仓库，
#          容器内自带 vitasdk + cmake + clangd

# 方式 B：纯 docker
docker run --rm -it -v "$PWD":/workspaces/psnine4vita -w /workspaces/psnine4vita \
  vitasdk/vitasdk:2026.08 bash
```

容器内执行：

```sh
cd vita/PsnineTrophies4Vita
cmake -S . -B build
cmake --build build          # -> build/PsnineTrophies4Vita.vpk
```

注意：

- HTML 解析器 [gumbo-parser](https://github.com/google/gumbo-parser) 已随仓库
  vendored 在 `third_party/`；中文字体在 `assets/font.ttf`。编译过程不需要联网。
- `sce_sys/*.png`（气泡图标、启动画面、LiveArea）必须保持 8-bit 调色板 PNG，
  否则 VitaShell 安装会报 `0x8010113D`。
- 产物传输与安装见第 2 节。

## 4. 数据来源

所有奖杯内容都由 psnine.com 以普通网页渲染。本项目**不是爬虫**：用户每打开一个页面，
app 才发起一次匿名 HTTP GET——与人类访客的浏览器行为完全一致；不做批量 URL 遍历、
没有后台抓取循环、不镜像站点内容（仅在机内缓存少量奖杯/封面图片用于显示）。

---

本项目与索尼互动娱乐及 psnine.com 无隶属关系。所有奖杯数据与 Tips 版权归原作者；
本项目仅作展示。
