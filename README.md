# Family Hub · 家庭小站

一块 ESP32-S3 手表/相框，给还不会打字的家人用：**摇一摇掷骰子**，**按住说话留言**。
家人用手机网页回一句，板子会自己亮起来、直接播放。

A ESP32-S3 family device for people who can't type yet: **shake to roll a die**, **hold to leave a voice message**.
When family replies from a phone, the device wakes up and plays it automatically.

[中文](#中文) · [English](#english)

---

## 中文

### 这是什么

给小孩和长辈的语音留言板。不登录、不加好友、不弹广告——按一下就说，松手就发。

- **摇骰子**：触摸、摇一摇、或按实体键，骰子抛起、翻面、落定
- **说说话**：按住说话，松手发送，最长 15 秒
- **收留言**：家人用手机网页回复，板子空闲时自动亮屏播放
- **锁屏**：按 GPIO18 熄屏，新留言会唤醒并播放

### 支持两块板

| | 方板 Waveshare AMOLED-2.16 | 腕表 M5Stack StopWatch |
|---|---|---|
| 屏幕 | 480×480 方 | 466 圆（面板 480，offset 6） |
| 触摸 | CST9220 | CST820B @0x15 |
| 麦克风 | ES7210 + ES8311 | 仅 ES8311（ADC 直采） |
| IMU | QMI8658 | BMI270 |
| 电源管理 | XPowersLib | **M5PM1 @0x6E** |
| IO 扩展 | 无 | **M5IOE1 @0x4F**（屏电源/复位/功放/震动） |
| 震动 | 无 | 有（IOE PWM1 → GPIO9） |

⚠️ 腕表上 LCD 电源、复位、触摸复位、codec 电源、功放使能**全走 M5IOE1**，方板那些直连管脚在腕表上不存在。不初始化 IOE，屏幕根本不亮。

### 目录

```
├── family-hub/          ★ 默认固件 = 一个系统（首页：Roll Dice + Voice Notes + 设置）
├── apps/                每个 App 单独跑的最小版本（隔离调试用，不要刷这里的）
│   ├── roll-dice/       ⚠️ 已停止维护，仅保留离线渲染测试 + 最小示例
│   └── voice-notes/     早期语音版
├── server/              服务端：内网网页 + Telegram Bot
└── preview/             浏览器 1:1 预览
```

**设备上装的是一个系统，不是两个。** `Roll Dice` 和 `Voice Notes` 是它的两个 App，
和手机上的 App 一个概念——它们不各自占一个仓库，也不各自占一次刷机。

### 烧录

```bash
cd family-hub
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800' .
arduino-cli upload  --fqbn 'esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800' -p /dev/cu.usbmodem1101 .
```

刷机前先整片备份（16MB，约 30 秒），这样随时能回到出厂固件：

```bash
esptool -p /dev/cu.usbmodem1101 read_flash 0x0 0x1000000 factory-backup.bin
```

依赖：GFX Library for Arduino 1.6.6 / SensorLib 0.3.3 / U8g2 / WiFiManager / XPowersLib 0.2.6 / M5Unified（腕表用）

### 串口调试命令

| 键 | 作用 |
|---|---|
| `s` | 状态：触摸/IMU/音频/codec/WiFi/缓存/PSRAM/MAC |
| `r` | 掷骰子 |
| `v` | 进家人页面并拉列表 |
| `p` | 重播最新留言 |
| `h` | 回首页 |
| `g` | 设置 |
| `l` | 锁屏 |
| `f` | 回读 RGB565 帧缓冲 |

### 已修的坑

- **停在家人页面时新留言只显示不播放**：列表由 `fetchMessages()` 每 15s 刷新，播放只由 `pollIncoming()` 负责，而后者在投骰/音效/录音时直接返回 → 刷进列表却不出声。现在 `fetchMessages()` 发现比水位线新的留言会直接播放，且只在播放成功后才推进水位线（commit `ebb2ee3`）。
- **播放失败零日志**：`playMessage()` 原来只 `return false`，和"没收到"无法区分。现在会打印具体是哪项头部校验没过。
- **CO5300 更新窗口必须偶数对齐**；单像素/1px 线在该屏不可靠，点数改用实心圆。
- **`bus->writeC8D8(0x36, 0xA0)` 不能删**，库默认 MADCTL 和面板不匹配。
- **功放使能是 GPIO46**，不拉高没声音。

### 测试

```bash
# 板子停在家人页面，发一条留言，应在 ~3s 内自动播放
curl -X POST 'http://<mac-ip>:8896/voice?from=dad' \
  -H 'content-type: audio/wav' --data-binary @some.wav
```

服务端默认 `http://<mac-ip>:8896`（内网），手机录音走 `https://<mac-ip>:9443/`（需装本站局域网根证书）。

---

## English

### What it is

A voice mailbox for kids and grandparents. No login, no friend requests, no ads — press to talk, release to send.

- **Roll dice** — tap, shake, or press the button; the die is tossed, flips, and lands
- **Voice notes** — hold to record, release to send, up to 15 seconds
- **Incoming messages** — family replies from a phone web page; the device wakes and plays it when idle
- **Lock screen** — GPIO18 blanks the screen; a new message wakes it and plays

### Two supported boards

| | Waveshare AMOLED-2.16 | M5Stack StopWatch |
|---|---|---|
| Display | 480×480 square | 466 round (480 panel, offset 6) |
| Touch | CST9220 | CST820B @0x15 |
| Mic | ES7210 + ES8311 | ES8311 only (ADC direct) |
| IMU | QMI8658 | BMI270 |
| Power | XPowersLib | **M5PM1 @0x6E** |
| IO expander | none | **M5IOE1 @0x4F** (LCD power/reset/PA/motor) |
| Haptics | no | yes (IOE PWM1 → GPIO9) |

⚠️ On the StopWatch, LCD power, reset, touch reset, codec power and PA enable all go through **M5IOE1**. The direct pins used on the square board do not exist there — without initializing the IOE the display never lights up.

### Layout

```
├── family-hub/          ★ Default firmware = ONE system (home: Roll Dice + Voice Notes + Settings)
├── apps/                Minimal single-app builds (for isolating a bug; do not flash these)
│   ├── roll-dice/       ⚠️ deprecated — kept only for offline render test + minimal example
│   └── voice-notes/     earlier voice-only build
├── server/              Server: LAN web page + Telegram bot
└── preview/             Pixel-accurate browser preview
```

**The device runs one system, not two apps bolted together.** `Roll Dice` and `Voice Notes`
are its two apps — same idea as apps on a phone. They don't get a repo each, and they don't
need a separate flash each.

### Build & flash

```bash
cd family-hub
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800' .
arduino-cli upload  --fqbn 'esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800' -p /dev/cu.usbmodem1101 .
```

Always take a full-flash backup first (16MB, ~30s) so you can return to the stock firmware:

```bash
esptool -p /dev/cu.usbmodem1101 read_flash 0x0 0x1000000 factory-backup.bin
```

Dependencies: GFX Library for Arduino 1.6.6 / SensorLib 0.3.3 / U8g2 / WiFiManager / XPowersLib 0.2.6 / M5Unified (StopWatch).

### Serial console

| Key | Action |
|---|---|
| `s` | status: touch/IMU/audio/codec/WiFi/cache/PSRAM/MAC |
| `r` | roll dice |
| `v` | open family page and fetch list |
| `p` | replay latest message |
| `h` | home |
| `g` | settings |
| `l` | lock screen |
| `f` | dump RGB565 framebuffer |

### Problems already fixed

- **Messages showed up on the family page but never played.** The list was refreshed by `fetchMessages()` every 15s while playback lived only in `pollIncoming()`, which returns early during a roll, a sound effect, or a recording — so a message could enter the list and stay silent. `fetchMessages()` now plays anything newer than the high-water mark, and the mark only advances after playback succeeds (commit `ebb2ee3`).
- **Silent playback failures.** `playMessage()` used to just `return false`, indistinguishable from "never received". It now logs exactly which header check failed.
- **CO5300 window updates must be even-aligned**; single pixels and 1px lines are unreliable on this panel, so pips are filled circles.
- **`bus->writeC8D8(0x36, 0xA0)` must stay** — the library's default MADCTL doesn't match the panel.
- **PA enable is GPIO46**; no sound without raising it.

### Test

```bash
# Park the device on the family page, send a message,
# it should auto-play within ~3s.
curl -X POST 'http://<mac-ip>:8896/voice?from=dad' \
  -H 'content-type: audio/wav' --data-binary @some.wav
```

The server listens on `http://<mac-ip>:8896` (LAN). Phone recording uses `https://<mac-ip>:9443/` and needs the LAN root CA installed on the phone.

## License

MIT
