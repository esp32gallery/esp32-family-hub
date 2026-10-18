# 语音信箱（内网网页 + 手机推送）

按住板子上的联系人按钮说话 → 松手 → 存到 Mac + 给那个人手机推送。

## 架构

```
板子（录音 + POST 裸 WAV）→ Mac voice-inbox（存档 + 推送）→ 手机 Bark 通知 → 点开网页播放
```

**为什么板子不编码 Opus**：libopus 要几百 KB flash，240MHz 单核实时编码 16kHz 有风险。
板子只做「录音 + POST」，转码/推送放 Mac —— 代码小、好排查。

## Mac 侧

```bash
# 启动
node ~/bin/voice-inbox          # 端口 8896

# 配置（联系人 + 推送 key）
~/.config/voice-inbox/contacts.json
```

接口：
- `GET  /`              网页（联系人分栏 + 播放）
- `POST /voice?to=dad`  板子上传 WAV
- `GET  /api/list`      列表 JSON
- `GET  /audio/<id>`    播放（支持 Range）

推送通道（`contacts.json` 里 `push.type`）：
- `bark`    —— iOS，App Store 免费（推荐）
- `ntfy`    —— 跨平台
- `webhook` —— 自建

**推送失败不影响存档**（实测：key 错时返回 `pushed:false`，WAV 照常落盘）。

## 板子固件

`voice-fw.ino` —— 编译 1.03MB / 32% flash。

改这三处：
```c
#define WIFI_SSID  "你的WiFi名"
#define WIFI_PASS  "你的WiFi密码"
#define INBOX_BASE "http://<Mac的IP>:8896"
```

联系人（3 个按钮，改这里加人）：
```c
static const Contact CONTACTS[] = {
  { "dad", "爸爸",   0xFD20 },
  { "mom", "妈妈",   0xF81F },
  { "me",  "我自己", 0x07FF },
};
```

编译烧录：
```bash
cd ~/Desktop/esp32-dice/voice-fw
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800" --upload -p /dev/cu.usbmodem2101 .
```

## ⚠️ 三个限制

1. **内网 HTTP 网页不能做 Web Push**（iOS 强制 HTTPS + Service Worker）→ 推送必须走 Bark/ntfy 这类第三方 app。
2. **手机和 Mac 必须同一 WiFi**。推送能到，但点开播放要连 Mac 的 8896。出门在外打不开。
3. **Mac 必须开机**（服务在 Mac 上）。

## 已验证 / 未验证

✅ Mac 侧完整可用：上传存档、列表、播放（Range）、推送框架（bark/ntfy/webhook）、推送失败不拖垮存档
✅ 固件编译通过
⚠️ 未验证：ES7210 实际录音（需板子）、Bark 真推送（需 key）、触摸按钮命中（需板子）
