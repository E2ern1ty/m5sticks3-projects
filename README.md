# M5StickS3 Projects 🛠️

**A collection of pocket tools for the M5StickS3 (ESP32-S3).**
为 M5StickS3 打造的口袋工具合集——刷上即用的三个小应用。

| 应用 | 一句话 | 截图/说明 |
|---|---|---|
| 📡 [WiFi Signal Hunter](wifi_scanner/) | 信号寻踪器：扫描排序、实时追踪、盖革蜂鸣、3D 方位罗盘 | [![List View](docs/img/list-view.svg)](wifi_scanner/) |
| 🗣️ [Pocket Translator](translator/) | 翻译器：按住说话，边说边出字，英文/日文译文+朗读 | 见 [translator](translator/) |
| 🫧 [Bubble Level](level/) | 水平仪：气泡/数字角度/校零，检查桌子平不平 | 见 [level](level/) |

硬件：M5Stack M5StickS3（K150，ESP32-S3-PICO-1-N8R8 + BMI270 + ES8311 + 红外 + 240×135 LCD）。
同类 ESP32-S3 + M5Unified 设备稍改引脚可跑。所有项目基于 Arduino + M5Unified
（板级自动识别），MIT License。

## 📥 免编译烧录

到 [Releases](https://github.com/E2ern1ty/m5sticks3-projects/releases) 下载对应应用的
`*-flashable.zip`，解压后：

```bash
pip install esptool   # 或 brew install esptool
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  write_flash 0x0 full-image-*.bin
```

烧完**拔插一次 USB** 即开机（ESP32-S3 USB 下载模式需断电退出）。
> ⚠️ 翻译器的免编译包只含占位密钥，联网使用需按 [translator/README](translator/) 配置密钥后源码构建。

## 🔧 从源码构建

```bash
arduino-cli compile --fqbn \
  'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,FlashSize=8M,PartitionScheme=default_8MB' \
  <目录名>            # wifi_scanner / translator / level
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn \
  'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,FlashSize=8M,PartitionScheme=default_8MB' \
  <目录名>
```

依赖：esp32 core ≥3.x + M5Unified（+ WebSockets/ArduinoJson/ESP8266Audio 仅翻译器）。

## 📚 其他

- 出厂 UIFlow2 固件还原：`restore_stock.sh`（需自备份镜像，见脚本注释）
- macOS 下 USB 串口会偶发把 S3 踢进下载模式 → 拔插一次 USB 即恢复
- 排障工具：`tools/usbreset.c`

---
Blog-style build notes (Chinese) live in each app's README. Made with an M5StickS3, an afternoon, and far too much debugging.
