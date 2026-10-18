# 固件画面预览

在仓库根目录运行：

```sh
mkdir -p preview/frames
c++ -O2 -std=c++17 preview/render_frames.cpp -o /tmp/family-render
/tmp/family-render
python3 preview/convert_frames.py
python3 -m http.server 8899 --directory preview --bind 127.0.0.1
```

打开 http://127.0.0.1:8899/firmware-sim.html 。先选择落点，再播放或逐帧检查。
生成的帧不提交 Git，可以用上述命令复现。`roll_motion.h` 与 `dice_sdf.h` 直接供主机和固件共用，没有另外复制一套 JS 渲染算法。

这是画面预览，不模拟 QSPI 总线、触摸硬件和声音。`font-native.png` 是 1:1 字形样张，`throw-filmstrip.png` 是 1→3/5/6 的抽帧对照。
