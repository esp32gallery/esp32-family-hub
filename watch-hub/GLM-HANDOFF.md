# GLM 接手：family-hub 腕表版（M5Stack StopWatch）重构任务书

仓库：`~/Desktop/esp32-family-hub/watch-hub`（PlatformIO 项目，env=stopwatch）
原版（方板，可参考的正确基线）：`~/Desktop/esp32-family-hub/family-hub/family-hub.ino`
设备文档：https://docs.m5stack.com/zh_CN/core/StopWatch

## 硬件事实（已验证，勿再猜）
- M5Stack StopWatch (SKU C152)：ESP32-S3R8，16MB flash / 8MB PSRAM(OPI)
- 屏：1.75" **466×466** CO5300 QSPI 圆形 AMOLED（M5GFX 面板 468×468、offset 6；**可视安全区按 466、圆形内切圆排 UI**）
- 触摸 CST820B：**必须用 `M5.Touch.getDetail()`**（内部已 convertRawXY 0..233 → 0..465），**严禁再 ×2**
- 电源 M5PM1、IO 扩展 M5IOE1（屏电源/复位/功放/震动全在 IOE 上）——全由 `M5.begin()` 接管
- 按键：**KEYA(黄)=GPIO1、KEYB(蓝)=GPIO2**（注意 Arduino 核心有 `BOOT_PIN` 宏，已改名 KYA_PIN/KYB_PIN）
- 麦克风/喇叭：ES8311 + MEMS MIC + AW8737A，全走 `M5.Mic` / `M5.Speaker` 官方 API。
  **禁止手写 ES8311 寄存器**（之前手写 0x14/0x17/0x32/0x37 导致录音"嗡嗡"杂音，已回滚教训）。
  M5Unified 对该板的 mic 回调会在 I2S 时钟启动后正确 arm ES8311。

## 当前两个未修 bug（根因已定位）
1. **骰子页花屏/闪退**：`src/roll_motion.h` 的 `compose()` 用 `SIDE=480` 行Stride 写 `animCanvas`（466×466）→ **每帧越界 14 列×466 行**，破坏相邻 PSRAM。`roll_motion.h` 是方板时代的 480 基准。
   修法（重构核心）：让 `compose()` 模板化/参数化输出尺寸（W=H=466），输入 cache 仍 240×RES；`dice_sdf.h` 的 Renderer 已支持任意 W/H（buf 外部传入，按 W/H 写），检查 `diceCacheTask`/`drawDice` 全部用 SCR_W/SCR_H，不得有 480/468 残留。
2. **录音沙沙但可听**：增益链是 M5Unified 默认（PGA+21dB/ADC+6dB）→ 已加**软件 AGC 归一 -3dBFS(≤12×)**，保留。沙沙可能来自 M5.Mic 采样路径，可用 `Mic_Class` 的 `noise_filter_level`/低 `magnification` 配置实验（见 M5Unified src/utility/Mic_Class.hpp），但**不得手写寄存器**。

## 必须保留的已修好项
- 触摸 `readTouch()` 走 `M5.Touch.getDetail()`，不缩放
- `blitFull()` 分带推屏（32 行一带 + delay(1)，防花屏）
- UI 全部在圆形安全区内（首页/语音页/设置页 466 版式已重排）
- 按键：KEYA 短按回首页 / KEYB 短按切页 / 双键长按 0.8s 熄屏唤醒
- 录音：AGC + 裁开头静音；播放：`M5.Speaker.playWav(buf,len)`（勿改回 playRaw 手算）
- 收信轮询 1.2s + 触摸中不轮询；`WiFi.setSleep(false)`
- 角色：本机=妈妈（SEND_CONTACT "mom"，监听 dad）；服务端 Air http://192.168.31.114:8896

## 构建验证（必须全绿才算完成）
```
cd ~/Desktop/esp32-family-hub/watch-hub
PLATFORMIO_CORE_DIR=$PWD/.pio-core .pio-venv/bin/pio run -e stopwatch
```
（.pio-core/.pio-venv 是指向 ~/Desktop/magpie-usage-display/ 同名目录的软链，勿删）
烧录：`... -t upload --upload-port /dev/cu.usbmodem101`（只有用户说烧才烧）
串口看日志：pyserial 读 /dev/cu.usbmodem101 @115200，**别用 DTR/RTS 复位**（会把板子按进下载模式）

## 交付
重构 roll_motion.h（尺寸参数化）+ 相关调用点 + 编译通过。改动最小化，风格与原码一致（中文注释可以）。

---

## 2026-10-20 重构完成记录（接手人已交付）

### 改动摘要（共 2 文件，3 处实质改动）
1. `src/roll_motion.h`
   - 第 4 行：`constexpr ... SIDE = 480` → 删除 `SIDE` 常量（附注释说明已废除），消除全部 480 硬编码源。
   - `compose()`：签名改为 `compose(cache, frame, lift, out, int OUT)`，行 stride / 循环边界 / 越界判据全部由 `SIDE` 改为参数 `OUT`；输入 cache 仍为 240×RES。**修掉每帧越界 14 列写坏相邻 PSRAM 的根因（骰子页花屏根因）**。新增 2 行中文注释说明参数含义与历史 bug。
2. `src/watch-hub.ino`
   - 唯一调用点 `drawCachedRollFrame()`（L298）：`compose(..., animCanvas)` → `compose(..., animCanvas, SCR_W)`（SCR_W=466）。

### 编译验证
```
PLATFORMIO_CORE_DIR=$PWD/.pio-core .pio-venv/bin/pio run -e stopwatch
→ SUCCESS 00:00:20.9（未烧录，按任务书要求）
```

### 480/468 残留核查（watch-hub 全部源文件）
- `watch-hub.ino:81`：注释「原始坐标 0..233 → 468」——仅说明触摸芯片原始坐标系，代码用 `M5.Touch.getDetail()` 不缩放，无实际 468 使用，保留。
- `dice_sdf.h:34/231-232`：焦距基准注释与 `p.focal * (W / 480.0f)` —— 这是 Renderer 按 480 基准归一化焦距、随实际 W 缩放的设计（正是支持任意画布的机制），非硬编码越界，保留。
- `roll_motion.h`：`SIDE` 已全删（diff 与方板基线 family-hub/roll_motion.h 对拍确认仅上述差异）。

### 未动项（按任务书「必须保留」清单）
触摸 getDetail / blitFull 32 行分带 / UI 466 圆形安全区 / AGC 录音链 / playWav / 1.2s 轮询 / WiFi.setSleep(false) 等全部原样。

### 中途踩坑（给下一手）
hashline `replace` 的 `text` 若缺换行，会吞掉下一行首段（首次提交时 `pose()` 被吃、旧行残留）——**替换单行时 text 末尾必须带换行**。已在第 2 次提交修正，最终文件已逐行核对。
