#!/bin/zsh
# 一键编译 + 烧录 3D 骰子到 Waveshare ESP32-S3-Touch-AMOLED-2.16
set -e
cd "$(dirname "$0")"

FQBN="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,FlashSize=16M,USBMode=hwcdc"

PORT="${1:-}"
if [[ -z "$PORT" ]]; then
  echo "== 检测串口 =="
  arduino-cli board list
  echo ""
  PORT=$(arduino-cli board list --format json \
    | python3 -c 'import sys,json;d=json.load(sys.stdin);p=[x["port"]["address"] for x in d.get("detected_ports",[]) if x["port"]["address"].startswith("/dev/cu.") and "Bluetooth" not in x["port"]["address"] and "debug" not in x["port"]["address"]];print(p[0] if p else "")' 2>/dev/null || true)
fi

if [[ -z "$PORT" ]]; then
  echo "❌ 没找到开发板串口。"
  echo "   1) 用能传数据的 USB-C 线（不是纯充电线）插到板子的 Type-C 口"
  echo "   2) 按住 BOOT 键再插线，然后松开（强制进下载模式）"
  echo "   3) 再跑：./flash.sh"
  echo "   4) 或者手动指定：./flash.sh /dev/cu.usbmodem1101"
  exit 1
fi

echo "== 串口: $PORT =="
echo "== 编译 + 烧录 =="
arduino-cli compile --fqbn "$FQBN" --upload -p "$PORT" dice3d

echo ""
echo "✅ 烧录完成。"
echo "   看日志： arduino-cli monitor -p $PORT -c baudrate=115200"
echo "   板子上应该立刻出现一颗可点/可摇的 3D 骰子。"
