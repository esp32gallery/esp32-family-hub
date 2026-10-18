# 编译与验证

## 板上编译（Arduino）
```bash
cd ~/Desktop/esp32-dice
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800" \
  --upload -p /dev/cu.usbmodem2101 dice3d
```
> ⚠️ 必须加 `UploadSpeed=460800`。默认 921600 在 ESP32-S3 原生 USB CDC 下会中途断流
> （实测备份时读到 0.8% 就 `Serial data stream stopped`）。

## 渲染器切换
`dice3d.ino` 顶部：
```c
#define DICE_USE_SDF 1   // 1 = SDF 光线步进（画质好）  0 = 旧 6 平面扫描线（省 CPU）
#define SDF_RES      240 // SDF 内部渲染分辨率（1:1 直推，不放大）
```

## 离线验证（不需要板子）
```bash
cd ~/Desktop/esp32-dice/dice3d
g++ -O2 -std=c++17 -o /tmp/dice_host host_test.cpp
/tmp/dice_host /tmp
```
输出：
- 6 个点数落位后「朝上的面」是否正确
- 各分辨率的 ms/帧、步进像素数、SDF 求值次数
- `/tmp/sdf_240_val{1..6}.ppm` 六张渲染图

## 静止不重绘（省 CPU/电）
`frameDirty` 标志：骰子静止时 `drawFrame()` 直接 return，不跑 SDF、不推屏。
只有翻滚/落位/新掷骰时才重绘。这样：
- 静止时 CPU 接近空闲（只轮询触摸/IMU/按键）
- 电池续航明显变好（AMOLED 黑底 + 不刷新）
- 输入响应更快（不会被渲染阻塞）

⚠️ 开机基准测的是「翻滚中」的最坏情况（斜视三面），静止时的 0 负载不计入。

## 板上基准（开机自动跑）
烧录后串口会打印：
```
=== SDF BENCH ===
res,ms,steps,sdf,hit
240,...
200,...
...
=== BENCH END ===
```
一次烧录就能拿到「分辨率 → 帧耗时」的完整表格。

## 已知坑
| 现象 | 原因 | 处理 |
|---|---|---|
| 玩一会儿 UI 冻结、fps 掉到 0.5 | `Serial.printf` 在无人读串口时阻塞 | 日志默认关闭；用 `SAFE_LOG` 宏（检查 `availableForWrite`） |
| `touch not found` | 先 `gfx->begin()` 再初始化触摸 | 顺序改为 触摸 → IMU → `gfx->begin()`（照抄官方 05_LVGL_Widgets） |
| 烧录中途 `Serial data stream stopped` | 921600 太快 | 降速到 460800 |
| `'Xxx' does not name a type` | Arduino 把函数原型插到文件第一个函数之前 | 所有 struct/enum 必须定义在第一个函数之前；或改用宏 |
| 板子完全不被识别 | USB 线只能充电 / 未按 PWR 开机 / 需要 BOOT 进下载模式 | 换线；按住 BOOT 插 USB |
