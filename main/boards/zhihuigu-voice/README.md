# Zhihuigu Voice (zhihuigu-voice)

基于 AI_Box-v1.2 电路（ESP32-S3）的自定义语音助手板，从早期 xiaozhi-esp32 工程的
`esp-box-lite` 定制板迁移而来。硬件与 ESP32-S3-BOX-Lite 同源（ES8311 输出 +
ES7210 输入），但显示改为 SSD1306 OLED，并增加了外设电源控制。

## 硬件配置

| 功能 | 引脚 / 器件 | 说明 |
|------|-------------|------|
| 音频输出 | ES8311（I2C1，SDA=8，SCL=18） | MCLK=2, BCLK=17, WS=47, DOUT=15 |
| 音频输入 | ES7210（同上 I2C） | DIN=16，16 kHz，支持 AEC 参考通道 |
| 功放使能 | GPIO46 | 高电平有效 |
| OLED 显示 | SSD1306 128x64（I2C0，SDA=6，SCL=7） | 地址 0x3C，XY 镜像 |
| OLED 电源 | GPIO45 | **低电平打开** |
| AUX 电源 | GPIO39 | **高电平打开** |
| 状态 LED | GPIO38 | **低电平点亮**（GpioLed 反相驱动，随设备状态变化） |
| BOOT 按键 | GPIO0 | 按住说话（press-to-talk），低电平唤醒轻睡眠 |
| ADC 按键 ×3 | ADC1 通道 0（GPIO1）分压 | NEXT=音量+，PREV=音量-，ENTER=对话切换 |

## 电源管理

无交互 30 秒后进入轻睡眠：断开 AUX 电源（GPIO39 拉低）、OLED 进入待机，
并使能 BOOT 按键（GPIO0 低电平）唤醒；唤醒后自动恢复 AUX 电源与 OLED 显示。
OLED 电源（GPIO45）在轻睡眠期间保持导通，以保证睡眠前后刷新可靠。

## 构建

```sh
python scripts/build.py zhihuigu-voice
```

注意：本板沿用项目默认 16MB flash / `partitions/v2/16m.csv`。若实际模组为
8MB flash，请在 `config.json` 的 `sdkconfig_append` 中加入：

```json
"CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y",
"CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions/v2/8m.csv\""
```

## 已知行为差异（相对旧工程）

- 轻睡眠唤醒后**继续运行**（旧工程唤醒后重启整机）。
- 状态 LED（GPIO38）由框架的 `GpioLed` 驱动，按设备状态呼吸/闪烁（旧工程仅静态电平）。
- 未连接 WiFi 时按 ENTER 键进入配网模式（对应旧工程的 `ResetWifiConfiguration`）。
