# 当前进度（2026-10-18 暂停点）

## ✅ 已完成，代码就绪

| 项 | 位置 | 验证 |
|---|---|---|
| SDF 渲染核心（纯 C++，可离线单测） | `dice3d/dice_sdf.h` | Mac g++ 编译通过；6 点数落位全对；圆角/凹坑/红底白点目视确认 |
| 离线测试程序 | `dice3d/host_test.cpp` | 输出各分辨率 ms/帧 + 6 张 PPM |
| 固件接入 | `dice3d/dice3d.ino` | 编译 410KB / 13% flash |
| 渲染器切换 | `DICE_USE_SDF` | 1=SDF / 0=旧6平面（可回退） |
| 分辨率可配 | `SDF_RES` | 240/200/180/160/140/120 |
| 开机性能基准 | setup() 内 | 一次烧录出 6 档完整表格 |
| 静止不重绘 | `frameDirty` | 静止时零渲染负载 |
| 文档 | `dice3d/BUILD.md` | 编译参数 + 5 个已知坑 |

## 🔑 关键数据（Mac 实测，仅作相对参考）

| 分辨率 | ms/帧 | 步进像素 | SDF求值 |
|---|---|---|---|
| 480² | 28.44 | 147192 | 786440 |
| 320² | 12.69 | 65874 | 349500 |
| 240² | 7.27 | 37460 | 196588 |
| 160² | 3.27 | 16896 | 87408 |
| 120² | 1.84 | 9652 | 49160 |

平均 **9.4 步/像素**（不是担心的 56）；SDF 求值 **5.2 次/像素**。

## ⏸ 卡点：板子不在 USB 上

`/dev/cu.*` 里没有任何 ESP32 设备。需要：
1. 换一根**能传数据**的 USB-C 线（现有这根今天断流过 3 次）
2. 按住 **BOOT** 插 USB，再松开
3. 或按一下侧面 **PWR** 键开机

## 恢复后立刻做

```bash
cd ~/Desktop/esp32-dice
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc,UploadSpeed=460800" --upload -p /dev/cu.usbmodemXXXX dice3d
```
然后读串口 `=== SDF BENCH ===` 那段，就能拿到 t1 要求的实测表格，据此定 SDF_RES 并完成 t4。

## 备份（随时可回滚出厂固件）

`~/Desktop/esp32-backups/amoled216-factory-20261018-1331.bin`（16MB，三重校验通过）
