# OCR 和翻译：中文工作记录与剩余 TODO

状态：2026-11-13。英文版 [ocr-translation-plan.md](ocr-translation-plan.md) 是权威设计文档；
本文是中文参考，记录当前状态、已定的命名和剩余事项，不逐段翻译英文计划。
原始机器翻译全文备份在 `notes/ocr-translation-plan_zh.full.md`（本地文件，不入 git）。

## 当前状态

- M0、M1、M2、M3、M4、M4.5、M4.6 以及 OCR/翻译入口点都已落地。
- `dev` = `7c24a9e`，`origin/dev` 已同步；主仓库本地和远端只剩 `dev`、`main`。
- kit = `4b70707`，已推到 `quantumblow/unisic-kit`；`.gitmodules` 指向该 fork，
  本地 kit 的 `origin` 是 fork，`upstream` 保留 `unisic/unisic-kit`。
- `ctest` 28/28。F8 在容器 Xvfb 下 108 PASS、5 FAIL、34 SKIP；新增的
  `ocr action`、`translation action` 都 PASS。5 个 FAIL 仍是容器缺件：
  WebP 编码器、portal 两次、ffmpeg 两次。
- AppImage 已从 `e8637c3` 构建：175,258,104 字节，sha256
  `23b6b2a6888be99893c89db8c59803b7daf8bffa6b52c6918cac28ac8fb4fdfc`，
  10 gates ON，`--version` 0.8.9，中文标签已确认在包内。这是 0.8.9 的预发布
  检查包，发布 0.8.10 前要重新构建。
- 完整 CI 矩阵 [43419341901](https://github.com/quantumblow/unisic/actions/runs/43419341901)：
  Debian、Arch、RPM 绿；Nix 红（kit pin，见下）；Windows、AppImage、Flatpak
  以该 run 的最终结果为准。
- 真机验收尚未做：Linux 桌面、Windows 11、Flatpak 沙箱、native packages。

## 已定的入口点和中文命名

- 截图 overlay：Shot 和 Measure 药丸的工具栏末尾是 Copy text (OCR) 和 Translate
  两个一次性 chip；OCR 药丸只显示 Translate，因为它的确认按钮已经是 Read text。
  点击确认选区、执行 OCR 或翻译并关闭 overlay，不保存、不进历史、不开编辑器。
- 编辑器：保留 OCR split button（主按钮整图 OCR，下拉是 Select text…），右侧新增
  labelled Translate 按钮，和 overlay 走同一条 OCR + 翻译路径。
- 结果窗口：不改第二个 Translate 入口；未配置时显示常驻提示行，Ctrl+Enter 是键盘入口，
  Copy all 保持 filled，Translate 保持 tonal。
- 托盘：`OCR region…`；中文是 `OCR 区域`，去掉省略号。
- Settings：Tesseract 移到 Text recognition 下，Translation 紧随；Editor tools 改名
  Tools；默认目标语言跟随 UI 语言，用户手改过的目标在切换语言后保留。
- 中文标签只改 `zh_CN`：`OCR（文字识别）`、`文本翻译`、`OCR 区域`。英文源字符串和
  其余 7 个语言目录保持原样。结果窗口的 Translation 仍是“译文”，因为那里指译文文本。

## 发布前 P0

### 1. Nix kit pin（当前唯一已知的红）

子模块已经指到 fork 的 `4b70707`，但 `flake.nix` 和 `flake.lock` 仍指 `0ebe87b`，
Nix job 卡在 "The kit commit must be the same in all THREE pins"。

1. 把 [flake.nix](../../flake.nix) 的 `unisic-kit` URL 改成
   `github:quantumblow/unisic-kit/4b70707af154f19dc10027cc31a96097d909632f`。
2. 在有 Nix 的机器上执行 `nix flake update unisic-kit`，让 `flake.lock` 同时更新
   `owner`、`rev`、`lastModified`、`narHash`。本机没有 Nix，没有在本地伪造 hash。
3. 一起提交 `flake.nix` + `flake.lock` 并推送，Nix job 才会转绿。

同一条 fork 变更还影响 [packaging/aur/README.md](../../packaging/aur/README.md)：
里面“kit commit 必须在 github.com/unisic/unisic-kit”这句要改成 fork。

### 2. Linux 桌面验收（Wayland KDE 为主，GNOME 一遍）

清单在 [ocr-translation-plan.md](ocr-translation-plan.md) 的 Manual verification pass。

1. F8 真桌面：`paddleocr`、`ocr result`、`ocr result image`、`translation`、
   `translation overlay`、`ocr action`、`translation action` PASS，无新增 FAIL。
2. 区域 OCR：`Meta+Shift+T` 分别读中文页和英文页；结果窗口 100% 打开、行可点、
   Copy all 正常。
3. 翻译主流程：本地 Ollama；逐行 overlay、深色横幅取色、Cancel 中断、
   Original/Translation 切换、Esc 关闭。
4. 结果窗口细节：Save image / Copy image 在两层都按原图分辨率；Edit in editor；
   200% 缩放点击命中；Ctrl+wheel 缩放和文字大小；拖动平移；分隔条重启后保留；
   RTL 目标右对齐。
5. M4.6 词/局部覆盖：双击单词或跨行选区；短答案画在 glyph 上、长答案进 callout；
   Save/Copy image 不包含词覆盖或 callout；中文 cover 不偏大；cover 起点和终点
   不越过邻居；短答案居中；缩放、平移、切层、新结果无残留。
6. 新入口点：Shot/Measure 的两个 chip；OCR 药丸只显示 Translate；点击 OCR
   不保存、不进历史、不进编辑器；Translate 未配置时禁用，结果窗口有常驻提示行，
   Ctrl+Enter 走同一路径；编辑器 Translate 按钮；误点只花一个 Esc。
7. Settings > General：Tesseract 在 Text recognition 下、Translation 紧随；
   Tools 列出 OCR/Translate 且说明指向 General；波兰语 UI 默认目标是 Polish，
   改过的目标在切语言后保留；托盘是 `OCR 区域`。
8. 中文标签：确认 `OCR（文字识别）`、`文本翻译`、`OCR 区域` 在真机上的排版，
   重点看编辑器顶栏和结果窗口是否挤压或截断。
9. 模型：AppImage/Flatpak 上 Tiny 显示 Included、in use，无下载按钮；下载 Small
   后日文走 Paddle；中断下载续传；`UNISIC_PADDLE_MODEL_BASE` 镜像校验；
   Remove background 的 runtime 来源和一次 undo。
10. DPI/窄窗口：100%、125%、150% 和 1060x700 窗口下 OCR 行、结果窗口、
    翻译 overlay 不裁切。

X11 只差一条：没有在 Wayland 可用的 OCR 行在 X11 被灰掉。M3/M4/M4.5 已在
Ubuntu X11 过；Wayland 仍未跑。

### 3. Windows 11 真机（T6.3，功能验证的关键）

Windows CI 只证明编译、ctest、打包、安装/卸载，不能代替真机功能。

1. MSYS2 UCRT64 构建并跑 `ctest`。CI 的 Windows job 目前 configure、gates 已过，
   build/test 仍在跑；本地命令见 [AGENTS.md](../../AGENTS.md) 的 Windows port。
2. 截图（全屏/区域/窗口）、overlay、编辑器、OCR 结果窗口、翻译全部走 Tesseract。
3. 新入口点：Shot/Measure 的 OCR/Translate chip、OCR 药丸只显示 Translate、
   点击不保存、Translate 未配置时禁用、结果窗口常驻提示行、Ctrl+Enter、
   编辑器 Translate 按钮、托盘 `OCR region` / `OCR 区域`、Settings 的 OCR/翻译分组、
   `translate.svg` 图标。
4. Settings > General > Text recognition：PaddleOCR 相关行灰掉，显示 Linux-only
   说明（`PaddleOcr::available()` 为 false），不提供 runtime/model 下载。
5. `bin\unisic.exe` 不应因为 6.3 MB 模型变大；`strings` 不应找到
   `PP-OCRv6_det_tiny`；不嵌入 ONNX Runtime。
6. Remove background 不在编辑器 More 菜单里。
7. 安装版和 portable zip 的安装/卸载/自更新路径不受这次改动影响。CI 会跑
   安装/卸载和截图，但真机仍建议走一遍。

### 4. Flatpak 沙箱（T6.1）

- 安装 bundle；完全无下载时 OCR 可用，Settings 显示
  `Tiny (6 MB): Included, in use`，无 Download。
- 沙箱内下载 Small，OCR 日文页。
- 沙箱内 Remove background（181 MB 模型下载 + 一次运行）。

### 5. 发布机制与网站

- `UNISIC_VERSION_STRING` 目前还是 0.8.9，CHANGELOG 已经是 0.8.10；发布前 bump 到
  0.8.10，并在 [metainfo](../../resources/app.unisic.Unisic.metainfo.xml) 加
  `<release version="0.8.10">`。
- 当前 AppImage 是 0.8.9 的预发布检查包；版本 bump 后重打发布用 AppImage，
  重新生成 `dist/SHA256SUMS`。
- T6.7 网站同步：`../unisic-website` 还停在 `VERSION = '0.8.9'`。需要更新九个
  `src/lib/i18n/*.ts`、`content/docs/{capture,dependencies,editor,installation}.md`、
  `Read.svelte`，把 OCR 从 Tesseract-only 改成 PP-OCRv6 + 翻译。
- 中文标签已由用户 review；本功能新增的波兰语等字符串仍建议 native review。
- 最后开 `dev -> main` 的 PR，让完整 CI 矩阵全绿后再发布。

### 6. README 与中文 README

- 英文 README 的 OCR 功能段还没提 overlay 的两个 chip 和编辑器 Translate 按钮，
  发布前补一句。
- 顺便增加一版中文 README（建议根目录 `README.zh_CN.md`，从英文 README 链接）。
- 中文 README 和网站中文文案都按当前中文命名：`OCR（文字识别）`、`文本翻译`、
  `OCR 区域`。

## 其它本机/渠道验收 P1

- **AppImage/portable tarball**：在没装 ONNX Runtime 的桌面上确认 Settings 显示
  `ONNX Runtime 1.31.1`、Tiny Included；Remove background 只要求 181 MB 模型；
  tarball 解包 `./AppRun` 同样。
- **deb/rpm/Arch**：开箱 OCR 走 Tesseract，PaddleOCR 提供 11 MB 下载；下载 Tiny
  后 in use，Small 后日文走 Paddle；rpm/Arch 用系统 `onnxruntime` 时不提供下载。
- **AUR**：发布前把 kit 来源说明改成 `quantumblow/unisic-kit`，并确认 release
  的完整 tarball 带上了 `4b70707` 的 kit。
- **旧 AppImage 实例**：当前还有一个旧实例在运行，重启后才会用上 `dist` 里的新包。
- **未跟踪文件**：`.memsearch/` 已加入忽略；`docs/dev/ocr-translation-plan_zh.md`
  就是本文件，随本次提交入库。

## 下一轮：M5

剩余 M5 任务在英文 plan 的 M5 一节，目标是韩文、阿拉伯文、西里尔、泰文、印度系、
希腊文从 Tesseract 迁到 PP-OCRv5 的分脚本 recognizer。要点：

- 检测继续用 PP-OCRv6 的 DB det，识别按脚本换 rec + dict；一套 pipeline、一个 runtime。
- pin 每套 rec+dict 的 URL、size、SHA-256；SnowShot 镜像没有这些文件，需用官方
  PaddleOCR release assets。
- Settings 的模型下载区加 scripts 分组；`supportsLanguages()` 按脚本路由；
  `effectiveTier()` 和内存守卫继续适用。
- 每条路径补 F8 + Developer，catalog、CHANGELOG EN/PL、计数同步。

M5 不阻塞 0.8.10，按既定顺序放在发布之后。

## 验证命令

```sh
# 构建和单测（容器）
docker exec -u 1000:1000 -e HOME=/tmp -w /src unisic-zh-build \
  cmake --build build --parallel 20
docker exec -u 1000:1000 -e HOME=/tmp -e QT_QPA_PLATFORM=offscreen -w /src unisic-zh-build \
  ctest --test-dir build --output-on-failure

# F8（无桌面；Xvfb + dbus-run-session）
# 先启动 Xvfb :99，然后：
HOME=/tmp/unisic-f8-home XDG_RUNTIME_DIR=/tmp/unisic-f8-run \
  DISPLAY=:99 QT_QPA_PLATFORM=xcb \
  dbus-run-session -- timeout 180 ./build/unisic --hotkey smoke-test
# 日志：$HOME/.local/share/Unisic/unisic-dev/logs/unisic.log
```
