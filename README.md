# V350 Scanner：Epson Perfection V350 胶片自动进片器（AFL）的 macOS 扫描程序

用 Epson Perfection V350 Photo（GT-F700）自带的 **135 胶片条自动进片器（AFL）** 扫描整条胶片。
先扫一张 300 dpi 预览，程序自动找出帧与帧之间的间隙并画出切分线，再用 600–4800 dpi 一格一格地扫描。
每一格保存为一个 TIFF（不裁切，8/16 位，DPI 标签正确）。负片默认直接保存为**正像成片**
（「输出成片」：去色罩、反相、自动色阶、灰平衡，效果接近 Epson Scan）；也可以关掉，保存**原始数据**，
之后在 Capture One / Lightroom / Negative Lab Pro 等软件里自己处理。正片始终保存原始数据。

- 原生 macOS 程序 **V350 Scanner.app**（Apple Silicon），界面中文、白底黑字
- 支持四种胶片的自动分割：**彩色负片、黑白负片、彩色正片、黑白正片**
  - 正片：帧间间隙是黑色的
  - 负片：帧间间隙是透明片基（黑白）或橙色片基（彩色），比画面亮；未曝光的片头也一样
  - 负片的预览用和「输出成片」完全相同的处理显示，所见即所存
- 扫描时边扫边显示预览；可以拖动、增加、删除切分线；每一格右边的勾选框决定是否扫描
- 文件直接存进你选的文件夹，编号接着文件夹里已有的最大 `frame_NN` 往后排，从不覆盖
- 同时提供命令行工具 `v350scan`

> 本项目与 Epson（精工爱普生）没有任何关系。它**不包含**任何 Epson 软件或固件：
> 运行时使用你自己从 Epson 官网安装的 ICA 驱动。

## 1. 先安装 Epson 官方驱动（必需）

本程序通过 Epson 官方 **ICA Scanner Driver 5.8.23** 里的扫描仪接口（`Interpreter 68.bundle`，
其中包含扫描仪固件 `Esfw68.bin`）和扫描仪通信。请先从 Epson 官网下载安装：

- Epson V350 支持页面（Drivers → ICA Scanner Driver v5.8.23 for Image Capture）：
  https://epson.com/Support/Scanners/Perfection-Series/Epson-Perfection-V350-Photo/s/SPT_B11B185011
- 直接下载（Epson 官方服务器）：https://ftp.epson.com/drivers/ESICA_5.8.23.dmg

安装后驱动位于：

```
/Library/Image Capture/Devices/EPSON Scanner.app/Contents/PlugIns/Interpreter 68.bundle
```

V350 Scanner 会自动在这里（以及 `~/Library/Image Capture/Devices/...`）查找。如果装在别处，
可以在程序里「Epson 驱动 → 手动指定驱动位置…」选择，或者给命令行加
`--interpreter PATH` / 设置环境变量 `V350_INTERPRETER`。找不到驱动时程序会显示中文提示和下载链接。

## 2. 安装 V350 Scanner.app

从 [Releases](https://github.com/Maksim-venus/v350-mac-scanner/releases) 下载
`V350-Scanner-0.1.0-macOS-arm64.zip`（或 `.dmg`），把 **V350 Scanner.app** 拖到「应用程序」。

程序是 ad-hoc 签名（没有 Apple 开发者证书、没有公证），第一次打开时 macOS 会拦截：

- 在 Finder 里**右键（或 Control+点按）→ 打开**，再点「打开」；或者
- macOS 15 及以后：先双击一次，然后到「系统设置 → 隐私与安全性」，点「仍要打开」；或者
- 终端里去掉隔离属性：`xattr -dr com.apple.quarantine "/Applications/V350 Scanner.app"`

要求：Apple Silicon Mac，macOS 13 或更新（在 macOS 26.7 上开发和测试）。扫描前请先退出「图像捕捉」
等可能占用扫描仪的程序。

## 3. 使用

1. 把胶片条插进 AFL，打开 **V350 Scanner**。
2. 选「胶片类型」（彩色负片 / 黑白负片 / 彩色正片 / 黑白正片），点 **预览 (300 dpi)**，
   第一次会让你选择保存文件夹。预览会边扫描边显示。
3. 检查自动切分线（绿色带 = 检测到的间隙）：点击添加，拖动调整，单击切分线删除。
   右边每一格的勾选框决定是否扫描（片头/空白格默认不勾）。
4. 选分辨率（600 / 1200 / 2400 / 4800 dpi）和位深（8 / 16 位），点 **扫描所选格**。
   负片可以选「输出成片」（默认勾选，会记住）：勾选 = 保存正像成片；不勾选 = 保存扫描仪原始数据（负像）。
5. 扫描完用扫描仪上的按钮退片（程序不会自动退片）。

### 「输出成片」是怎么做的

参照 Epson Scan 处理负片的流程（预览后自动曝光 = 每个通道按直方图定黑白点、少量裁切、显示伽马），
在我们自己的代码里实现（`src/v350film.c`）：

1. **片基 / 橙色色罩**：从预览里检测到的帧间间隙和片头（未曝光的片基）测出每个通道的片基亮度；
   找不到间隙时用画面里最亮的 0.5 % 作为片基。每个通道除以片基 = 去掉橙色色罩。
2. **密度**：`D = -log10(原始值 / 片基)`（线性透射率 → 密度）。
3. **自动黑白点**：只统计画面区域（排除间隙、片头、两边齿孔），每个通道单独定黑点（裁 0.1 %）和白点（裁 0.5 %）。
4. **灰平衡**：R、B 通道的中间调按 G 通道对齐（灰世界，修正幅度有上限，避免把夕阳"修"成灰色）。
5. **反相 + 色调**：密度映射回线性亮度（约 2.2 个数量级的场景范围），再按显示伽马 2.2 编码。
6. 黑白负片：三个通道平均后同样处理，保存为**单通道灰度 TIFF**。

参数**整条胶片只算一次**（来自 300 dpi 预览），所以同一条胶片每一格的颜色和亮度一致；
扫描每一格时只用这一格开头的间隙重新测一下片基亮度（高分辨率扫描路径的曝光可能和预览路径略有不同）。
Epson Scan 的缩略图模式是每一格单独自动曝光；本程序目前只有"整条一致"这一种方式。
计算用浮点、16 位查表，16 位输出没有色阶断层。处理参数写在预览 JSON 的 `"finish"` 里。

已经扫好的原始 TIFF 也可以事后处理：
`v350scan --develop raw.tif out.tif --finish-from preview.json --dpi 2400`。

## 命令行 `v350scan`

```bash
v350scan --check-driver                                  # 检查 Epson 驱动（不访问 USB）
v350scan --preview --film cneg --out /tmp/p.tif          # 300 dpi 预览 + PNG + JSON + 建议切分线
v350scan --dpi 2400 --bits 16 --cuts-from /tmp/p.json --only 2,3,4 --outdir ~/Pictures/V350/roll1
v350scan --analyze /tmp/p.tif --film bneg                # 对已有预览重新做间隙检测（不访问扫描仪）
v350scan --dpi 1200 --cuts 1.67,3.14,4.57 --dry-run      # 只打印扫描计划
v350scan --dpi 2400 --film cneg --cuts-from /tmp/p.json --finish-from /tmp/p.json --only 2 --outdir DIR  # 负片成片
v350scan --develop raw.tif out.tif --finish-from /tmp/p.json --dpi 2400                                # 事后成片
```

`--film cpos|bpos|cneg|bneg` 决定间隙检测和预览 PNG 的处理方式。不加 `--finish-from` 时保存原始数据。

**实验开关 `--esc-n1`（未在硬件上测试）：** 负片用扫描仪自己的负片模式（ESC N=1）扫描，用来和默认的
ESC N=0 做对比。预览和扫描必须都用同一个模式（参数来自预览）。程序里默认关闭；要在 app 里试，
在 `~/Library/Application Support/v350scan/gui.json` 里加 `"esc_n1": true`（界面上没有这个开关）。
`app` 里的 `Contents/MacOS/v350scan` 就是这个工具。其它选项见 `v350scan --help`。

## 已知限制

- **AFL 窗口长度：** 600 dpi 以上，扫描仪在一个窗口里只能可靠地扫大约 4.5 英寸。所以每一格单独扫一个窗口
  （约 1.5–1.8 英寸），超过 4.5 英寸的格会被拒绝，需要加切分线。
- **退片：** 用扫描仪上的按钮。软件退片（ESC e）在测试机上不动，还卡住过一次，所以默认关闭。
- **没有 Digital ICE：** V350 没有红外灯，没有硬件除尘。
- 负片默认按「正片模式」（ESC N=0）扫描，得到原始透射数据，「输出成片」在电脑上完成。Epson 自己的软件扫负片时
  用 ESC N=1，扫描仪会用另一套曝光/增益（尤其是蓝色通道）。ESC N=1 还没在硬件上验证，只作为实验开关 `--esc-n1`。
- 8 位预览时，很密的彩色负片蓝色通道只有很少几个原始级别，成片参数会略有误差（合成测试里灰阶最大偏色约 7/255；
  16 位预览约 5/255）。「输出成片」的颜色是自动的（类似 Epson 的自动曝光），不是精确的色彩管理。
- 300 dpi 预览由进片器带动胶片，纵向比例大约短 3%（已标定补偿）；600 dpi 以上由扫描头移动，比例正确。
- 只在一台 V350 上测试过。

## 从源码构建

需要 Xcode 或 Command Line Tools（`swiftc`、`clang`）。

```bash
scripts/build_app.sh          # -> dist/V350 Scanner.app + .zip + .dmg
```

脚本会下载官方 libusb 1.0.30 源码并为 macOS 13 编译（LGPL-2.1，作为单独的 dylib 放在
`Contents/Frameworks/`），编译 CLI、模拟器版 CLI 和 Swift 程序，然后 ad-hoc 签名。
不依赖 Homebrew 的 Python 或 libusb（`LIBUSB_PREFIX=$(brew --prefix libusb)` 可改用 Homebrew 的 libusb）。

只编译命令行：`scripts/build_cli.sh`（需要 `brew install libusb pkg-config`）。

### 测试（不需要扫描仪）

```bash
scripts/run_tests.sh                                       # 模拟器 + 四种胶片的检测测试 + 成片测试
open "dist/V350 Scanner.app" --args --simulate             # 用模拟扫描仪打开程序
```

`test/mocklib.c` 用合成胶片条代替 USB 层（带进片误差、抖动和倾斜；`M_FILM=cneg|bneg|cpos|bpos`）。
`test/synth_film.py` 为四种胶片各生成 20 条合成胶片条（不同间隙宽度、过曝/欠曝帧、夜景、大块黑色、灰尘），
检查每个间隙正好有一条切分线、没有切分线落在画面里。
`test/synth_finish.py` 把一张已知的正像（合成图，或者一张真实的正片预览）模拟成彩色/黑白负片
（橙色色罩、各通道不同反差的特性曲线、可选的染料串色、噪声），再用 `--analyze` + `--develop` 还原，
检查与原图的相关系数和灰阶是否中性，并生成对比图（原始负片 | 简单反相 | 成片 | 原图）。`gui/v350gui.py` 是同一界面的 Python 开发版服务器。

## 文档

- [docs/PROTOCOL.md](docs/PROTOCOL.md)：扫描仪协议和 AFL 行为笔记（我们自己的观察和描述）
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)：第三方许可（libusb LGPL-2.1）

## 许可

MIT，见 [LICENSE](LICENSE)。Copyright © 2026 Maksim-venus。

---

## English

**V350 Scanner** is a native macOS app (Apple Silicon) and CLI for scanning 35 mm film strips with the
**Epson Perfection V350 Photo** (GT-F700) built-in **auto film loader**. It takes a 300 dpi preview,
detects the gaps between frames automatically, and then scans each selected frame at 600–4800 dpi
as its own TIFF (no crop; 8/16-bit; correct DPI tags). Negatives are saved as **finished positives** by
default (「输出成片」: orange-mask/base removal, inversion in density, per-channel auto black/white points,
grey balance, gamma 2.2 — modelled on Epson Scan's auto exposure; parameters computed once per strip
from the preview so all frames match; B&W negatives as greyscale TIFF). Turn it off to get raw data.
Slides are always raw.

- Film types with automatic frame-gap detection: colour negative, B&W negative, colour slide, B&W positive.
  The negative preview uses the same finishing pipeline, so what you see is what is saved.
  `v350scan --develop raw.tif out.tif --finish-from preview.json` finishes an existing raw scan.
  `--esc-n1` (experimental, not hardware-tested) scans negatives in the scanner's own negative mode for A/B tests.
- Live preview while scanning, editable cut lines, per-frame checkboxes, continuous `frame_NN` numbering.
- UI in Simplified Chinese.

**Requirements:** Apple Silicon Mac, macOS 13+ (developed on macOS 26.7). You must install Epson's
official **ICA Scanner Driver 5.8.23** first
(https://epson.com/Support/Scanners/Perfection-Series/Epson-Perfection-V350-Photo/s/SPT_B11B185011,
direct: https://ftp.epson.com/drivers/ESICA_5.8.23.dmg). The app loads Epson's `Interpreter 68.bundle`
from `/Library/Image Capture/Devices/EPSON Scanner.app/Contents/PlugIns/` at run time.
**No Epson software or firmware is included in this repository or the app.**

**Install:** download the zip/dmg from Releases. The app is ad-hoc signed (not notarized): right-click → Open,
or System Settings → Privacy & Security → Open Anyway, or `xattr -dr com.apple.quarantine "/Applications/V350 Scanner.app"`.

**Build:** `scripts/build_app.sh` (Xcode/CLT; downloads and builds libusb 1.0.30). **Test without hardware:**
`scripts/run_tests.sh`, and `open "dist/V350 Scanner.app" --args --simulate`.

**Known limits:** above 300 dpi each window must stay under ~4.5" (so each frame is its own window);
eject with the scanner's button; no Digital ICE (the V350 has no infrared channel); negatives are scanned in
positive mode (ESC N=0) — Epson's own software uses ESC N=1 for negatives, which is not yet verified here;
tested on one unit only.

Not affiliated with or endorsed by Seiko Epson Corporation. MIT licensed; libusb is LGPL-2.1 (see THIRD_PARTY_NOTICES.md).
