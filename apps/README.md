# Apps · 单 App 版本

`family-hub/` 是**默认固件**——一个系统，首页并列 `Roll Dice` 和 `Voice Notes` 两个 App 加一个设置页。

这里放的是**每个 App 单独跑的最小版本**，用途是隔离调试，不是给你日常刷机的。

| 目录 | 是什么 | 维护状态 |
|---|---|---|
| `roll-dice/` | 只有骰子，无语音/网络 | ⚠️ 见下方 |
| `voice-notes/` | 只有语音，无骰子 | 早期版本 |

> **要刷机就给 `family-hub/` 刷，不要刷这里。**
> 这里的代码是为了「把一个问题单独拿出来看」而保留的。

---

## roll-dice/ · ⚠️ 已停止维护

**这个 `.ino` 是历史快照，不是当前骰子实现。**

| | `apps/roll-dice/`（本目录） | `family-hub/`（当前） |
|---|---|---|
| 渲染分辨率 | 240×240 低分辨率 + 脏矩形放大 | SDF 直渲 480×480 |
| 翻滚动画 | 有 | 有，且预渲染 24 帧序列缓存 |
| 最后改进 | 2026-10-18 | 持续 |
| 附带能力 | 满屏骰子 + 防烧屏调光 | 骰子 + 语音 + 设置 |

如果你现在往板子上刷 `roll-dice.ino`，拿到的是**旧渲染器**。
最新的骰子实现在 `family-hub/family-hub.ino`（`diceCacheTask` / `drawCachedRollFrame`）。

### 保留它的两个理由

1. **`host_test.cpp`** —— 在 Mac 上离线跑 `dice_sdf.h`，输出各分辨率耗时和 PPM 图。
   改渲染参数时先跑它验证，不用反复烧板子。这个 `family-hub/` 里没有。
2. **最小可读示例** —— 去掉语音、网络、音频两百多行依赖后，纯渲染管线看得更清楚。

```bash
# 离线验证渲染器（不需要板子）
g++ -O2 -std=c++17 -o /tmp/dice_host host_test.cpp && /tmp/dice_host /tmp/dice_out
```

⚠️ 注意这里的 `dice_sdf.h` 也是旧版。要做真实性能测试，用 `family-hub/dice_sdf.h` 替换后再编译。

---

## voice-notes/ · 早期版本

只有录音 + POST 上传，不含骰子。服务端协议与 `family-hub/` 相同，都打 `server/voice-inbox`。

已被 `family-hub/` 的语音页取代，保留作参考。

依赖 `private-config.h`（WiFi 密码 / 服务器地址），该文件不入库——参考 `family-hub/private-config.h` 的字段自己建。
