# 2026-10-19 验证记录

目标板：ESP32-S3-Touch-AMOLED-2.16，MAC `44:BD:8D:78:9F:B4`，`/dev/cu.usbmodem2101`。另一块 USB 板未刷写。

| 检查 | 结果与证据 |
|---|---|
| Arduino 编译 | 通过，约 1.39 MB，占 3 MB 程序分区 44% |
| 原板启动 | touch / imu / audio / codec / wifi / cache 均为 1；PSRAM 空余约 1.21 MB |
| 动画运行 | 三次完整运行 1122 / 1120 / 1122 ms，分别落到 5 / 6 / 5，10 个动态帧；串口记录在 preview/board-verification.log |
| 实际绘制界面 | 通过串口回读原板 RGB565；preview/board-home.png、board-voice.png、board-settings.png；这是帧缓冲截图，不能证明物理传输无故障 |
| 主机图像与内存检查 | AddressSanitizer + UndefinedBehaviorSanitizer 执行共享渲染器全部 36 种起点/终点，共 468 张帧，无报告 |
| 浏览器画面预览 | 播放到选定落点 3，约 964 ms；可逐帧检查 |
| 爸爸、妈妈自动播放 | 两次父母方向测试上传，最终复核分别约 211 / 216 ms 确认保存，原板均输出 `[play] ok=1 pcm=32000 bytes` |
| 锁屏来信 | 原板从设置页进入熄屏后，新来信唤醒并完成播放 |
| 手机音频读取 | 全文件 200、32044 字节；头/尾 10 字节 Range 均 206；非法、越界、倒序 Range 均 416 |
| 证书下载 | 公共 /local-ca.crt 返回 200、1062 字节；旧私钥包路径 /local-ca.p12 返回 404 |
| 孩子上传响应 | 测试 WAV 约 44 ms 返回成功，Telegram 为 queued，不等待外网 |
| Telegram 队列 | 重启前后待发数量均 1；测试语音删除后队列归零；测试 WAV 已删除 |
| Telegram 新 Bot | getMe 确认为 family_voice_2026_bot，独立接收任务在运行；Token 文件权限均 600 |
| Telegram 未完成步骤 | 用户未 Start，getChat 返回 chat not found。无法验证真人 Bot 语音发出/回复接收，服务会等待用户开始 |
| Air 部署一致性 | voice-inbox 与接收 worker 的 SHA-256 均与本地源码一致；无 Token 写入 Git diff |
| 服务端/网页语法 | Node 服务与 worker 语法通过；Air 返回的展开后网页 JavaScript 语法通过 |
| Git 文本检查 | diff --check 通过 |

仍需人手确认：

- 真机看完整动态画面，确认旋转观感与清晰度；用串口无崩溃代替这个结论是不成立的。
- 小孩实际按住屏幕 3–10 秒再松开，确认不会提前结束；离手的容忍窗口为 220 ms。
- 听真实人声，确认麦克风与扬声器的主观音质。测试 PCM 字节数只验证播放通路。
- 给新 Bot 点 Start 后发一条语音，完成 Telegram 真人往返。

已知边界：启动需先准备姿态缓存；全屏推送约 80 ms，帧率受此限制。录音/播放期间实体键响应会延后。局域网 HTTPS 首次需要手机安装并信任公共证书。Air 需开机。Telegram 外部网络重试在极少数断线时可能重复送达，不能保证严格只发送一次。

最终固件备份：`~/Desktop/esp32-backups/family-hub-20261019-verified.bin`，SHA-256 `bec18b0531de0242d8c6e27a332531cb86919f1b9f7d56fdfbebaf53dbd0db9c`。最终三个 UI 回读图已逐张检查，字形及标签对应正确。

## 家长网页自动播放增量验证（2026-10-19）

仅在本地隔离服务 18996/18998 和 Chrome 中验证；未部署 Air，未调用家庭 Telegram Token。

- 一次点击启用后，新消息自动进入播放，按钮为暂停状态。
- 刷新页面不自动播放旧消息，开关默认关闭。
- 关闭时两条来信显示「宝宝发来 2 条语音」和标题计数，无播放。
- 重新开启后先播放时间较早的一条，再播放下一条；播放结束提醒隐藏，开关仍开启。
- 家长发送方向消息只出现在列表，不触发自动播放和宝宝未读计数。
- `/api/events` 实际接收对应 ID/direction 的事件，上传、转码与通知共约 187 ms。
- 公开入口收到 Unicode 无效密钥返回 401，服务继续工作。
- Node 服务、实际 HTML 展开后的浏览器脚本语法和 Git diff 检查通过。
- 手机 Safari 的实际自动播放、录音中断与后台行为尚需真机确认；后台提醒使用 Telegram，不是 Web Push 订阅。

部署未完成原因：Air 的 LAN 地址不可达、Tailscale 显示离线。等待 Air 唤醒联网。代码与验证画面已纳入 Git。
