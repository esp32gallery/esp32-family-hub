# 家庭小站固件

微雪 ESP32-S3-Touch-AMOLED-2.16 的合并固件：骰子、语音信箱和设置。

## 板子操作

- 首页点「骰子」：触摸屏幕、摇一摇，或按 GPIO18 按键投骰子。双击 BOOT 回首页。
- 首页点「语音信箱」：黄色爸爸头像、红色妈妈头像。选好联系人后，同屏上方显示该联系人的最新语音和「点一下重听」，下方固定「按住说话」；只有连续确认手指确实离开屏幕才结束并自动发送。播放完仍停留这一屏。板子在空闲时每约 5 秒检查新语音，收到后自动切到对应联系人并播放。
- 首页点「设置」：调亮度、音量或重新配网。亮度、音量和服务器地址保存在 NVS。
- 锁屏：非骰子页面按 GPIO18 让屏幕关闭并清屏；再按 GPIO18 或 BOOT 键唤醒，回到原页面。骰子页按 GPIO18 仍是投骰。
- Telegram：小孩发出的语音会同时发送到 `@pim1air_bot`。反向语音需要单独注册一个 Bot，避免与现有 `pihome` 文字会话争抢消息。把新 Bot Token 写入 Air 的 `~/.config/voice-inbox/telegram-voice-token` 和 `telegram-send-token`（权限 600），再加载 `com.liu1.tg-voice-receiver`；新 Bot 只给家庭语音使用。

- ES8311 通过 GPIO8 输出 PCM，GPIO46 控制功放。播放和骰子音效需要接好板子的扬声器/音频输出。

2026-10-18 板上验证：手机方向给爸爸栏发送 1 秒测试音后，板子读到 1 条来信，播放通路输出 32,000 字节 PCM；测试消息随后已删除。真人语音的扬声器听感仍需现场确认。

## 手机操作

- 与 Air 在同一 Wi-Fi：打开 <http://192.168.31.114:8896/> 查看语音和下载安装页。第一次先把 Air 生成的 `family-hub-local-ca.p12` 安装为手机受信任证书；随后使用内网直连 <https://192.168.31.114:9443/> 直接录音。证书密码是 `familyhub`。
- iPhone 安装后还需在「设置 → 通用 → 关于本机 → 证书信任设置」中开启该根证书的完全信任。Android 在安全和凭据里安装 CA 证书。安装一次后，同一部手机可长期使用内网地址。
- 内网 HTTPS 允许麦克风，不经过 Tailscale；按住右下角悬浮麦克风说话，松手自动发送，同时能播放板子发来的语音。出门在外时才使用带访问密钥的 Tailscale/公开分享入口。
- 板子空闲时约每 5 秒检查新消息并自动播放；正在录音、放音、投骰或调设置时会延后。服务端在 Air 的 `~/bin/voice-inbox`，由 `com.local.voice-inbox` 常驻。

局域网的 `http://192.168...` 地址不能直接调用浏览器麦克风；双向语音使用 Air 本机 `9443` 端口的 HTTPS。所有收发 WAV 和索引都保存在 Air 的 `~/Desktop/voice-inbox/`，两位大人和小孩的消息共用这一个双向存档，手机在线状态不影响存档。局域网根证书和服务器证书在 Air 的 `~/.config/voice-inbox/tls/`。

## 编译与烧录

```sh
python3 family-hub/make_font.py
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800' family-hub
arduino-cli upload --fqbn 'esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800' -p /dev/cu.usbmodem2101 family-hub
```

骰子在后台预渲染每个结果的真实 3D 旋转序列。点按后立即显示第一帧旋转姿态，连续播放 12 张 160×160 姿态帧，再显示预先缓存的 480×480 原生结果。原板子实测 11 个后续动态帧，完整滚动约 1.39 秒，停稳画面约 82 毫秒；当前 Arduino_GFX 逐像素推屏仍是帧率瓶颈。若开机缓存尚未生成，则使用即时渲染路径，不会用随机数字冒充翻转。串口命令：`s` 状态、`r` 投骰、`v` 拉取爸爸栏语音、`p` 播放第一条、`h` 回首页。

在 Mac 上预览同一套姿态：在 `preview/` 目录执行 `python3 -m http.server 8899 --bind 127.0.0.1`，再打开 `http://127.0.0.1:8899/firmware-sim.html`。
