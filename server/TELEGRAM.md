# 家庭语音 Telegram

独立 Bot：`@family_voice_2026_bot`。Air 上的文件：

- `~/.config/voice-inbox/telegram-voice-token`：接收 Token，权限 600。
- `~/.config/voice-inbox/telegram-send-token`：发送 Token，权限 600。
- `~/tg-voice-receiver.mjs`：仓库 `server/tg-voice-receiver.mjs` 的部署副本。
- `~/bin/voice-inbox`：仓库 `server/voice-inbox` 的部署副本。
- `~/Library/LaunchAgents/com.liu1.tg-voice-receiver.plist`：独立接收守护任务；不要复用其它 Bot 的 getUpdates。

不把 Token 写进 Git、命令文本或日志。默认只接收 chat 795477335，可通过 `VOICE_TG_CHAT_ID` 配置。接收 curl 经本机 HTTP 代理 `127.0.0.1:7890`，需要代理正常工作。

板子发送→Air 保存→立即答复板子→持久 `.telegram-queue.json`→转 Opus 发 Bot。以 Bot API JSON `ok` 为发送依据。大人语音→getUpdates→getFile→32k PCM→本机 `/voice?from=dad`，带来源标识防重。只有成功保存后才推进该 Bot 独立的 offset；来信不回传 Telegram，避免回声。

用户第一次必须给新 Bot 点 Start。服务不能代替 Telegram 用户完成这个授权；未开始时 `getChat` 返回 chat not found，sendVoice 会失败并保留队列等待重试。

接收日志：`~/Library/Logs/voice-inbox/tg-receiver.log`。恢复启动：

```sh
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.liu1.tg-voice-receiver.plist
# 已加载的任务可重启：
launchctl kickstart -k gui/$(id -u)/com.liu1.tg-voice-receiver
```

Telegram 提供的是家庭的补充入口；本地 WAV 是主存档。网络发送可能在 Telegram 已接收、服务尚未来得及记账时中断，因此极少数重试仍可能重复发送一份，不能承诺外部服务严格只送一次。
