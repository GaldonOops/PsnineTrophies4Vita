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
   如 `ftp://192.168.1.20:1337`。
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

## 4. 数据来源与巡检脚本

所有奖杯内容都由 psnine.com 以普通网页渲染。本项目**不是爬虫**：用户每打开一个页面，
app 才发起一次匿名 HTTP GET——与人类访客的浏览器行为完全一致；不做批量 URL 遍历、
没有后台抓取循环、不镜像站点内容（仅在机内缓存少量奖杯/封面图片用于显示）。

由于站点是无公开 API 的服务端渲染 PHP 程序，解析器依赖其 DOM 结构。为在改版影响到
用户之前发现问题，仓库内置一个**巡检脚本**：

```sh
npm install     # 首次，安装 cheerio
npm run patrol  # scripts/scraper.mjs --patrol
```

每次运行会：

- 抓取一小组固定探针页（一个搜索结果页、游戏 `57900`、奖杯 `57900001`，
  外加由该游戏派生的两个 Tips 页）——总共十来个请求，适合偶尔运行（例如每周一天的
  cron），绝非常驻抓取。
- 断言的是语义结果而非原始 HTML：结果数 > 0、每行奖杯有可跳转 id、等级/名称字段
  解析成功、每个 Tips 页至少命中两套并存模板（`div.post` 或 `ul.list > li`）之一。
- 每条断言输出一行 `PASS`/`FAIL`，外加模板普查统计（`div.post=N ul.list=M`）反映
  线上 Tips 页当前用哪套模板；任一断言失败即非零退出——站点改版首先表现为巡检变红，
  而不是真机上静默白屏。
- 把抓到的页面存到 `scripts/fixtures/`（已 gitignore）作为离线语料。

交叉验证：`scripts/parse_probe.c` 可在编译容器里用 app 自己的 C 解析器
（`src/parse.c` + gumbo）回放这些 fixture，确认机内解析器与巡检对同一份 DOM 的
理解一致：

```sh
gcc -I vita/PsnineTrophies4Vita/src -I vita/PsnineTrophies4Vita/third_party/gumbo-parser/src \
  scripts/parse_probe.c vita/PsnineTrophies4Vita/src/parse.c \
  vita/PsnineTrophies4Vita/third_party/gumbo-parser/src/*.c -o /tmp/parse_probe
/tmp/parse_probe game scripts/fixtures/trophies-57900.html
```

---

本项目与索尼互动娱乐及 psnine.com 无隶属关系。所有奖杯数据与 Tips 版权归原作者；
本项目仅作展示。
