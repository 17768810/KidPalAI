# KidPalAI 小书童 — ESP32-S3 固件

目标芯片：ESP32-S3

---

## 一、环境搭建（首次配置）

### 1. 安装 ESP-IDF v5.x

- **Windows**：下载并运行[官方离线安装包](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/windows-setup.html)，安装完成后使用「ESP-IDF CMD」终端执行后续命令。
- **macOS**：
  ```bash
  brew install cmake ninja dfu-util python3
  mkdir -p ~/esp && cd ~/esp
  git clone --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && git checkout v5.3
  ./install.sh esp32s3
  source export.sh   # 每次新开终端都需执行
  ```

### 2. 安装 ESP-SR（唤醒词检测）

```bash
cd firmware
idf.py add-dependency "espressif/esp-sr^1.0.0"
```

### 3. 安装 ESP-ADF（音频框架）

参考官方文档：https://github.com/espressif/esp-adf

### 4. 设置目标芯片

```bash
cd firmware
idf.py set-target esp32s3
```

---

## 二、参数配置

```bash
idf.py menuconfig
# 进入：KidPalAI Configuration
# 填写：WiFi SSID、WiFi 密码、语音网关 URL
```

---

## 三、编译与烧录

```bash
idf.py build
```

烧录到设备并查看串口日志：

- **Windows**（端口号按实际情况替换，如 `COM3`）：
  ```bash
  idf.py -p COM3 flash monitor
  ```
- **macOS**（端口号按实际情况替换）：
  ```bash
  idf.py -p /dev/cu.usbmodem5B7A0217581 flash monitor
  ```

> 提示：Windows 可在设备管理器 → 端口（COM 和 LPT）中查看端口号；macOS 可用 `ls /dev/cu.*` 查找。

---

## 四、模拟器调试（无硬件时使用 Wokwi）

在硬件到手之前，可以使用 VSCode 的 Wokwi 插件在本机模拟运行固件。

### 1. 安装插件

在 VSCode 扩展商店搜索并安装 **Wokwi Simulator**。

首次使用需激活免费许可证：

`Ctrl+Shift+P` → 输入 `Wokwi: Request a New License` → 按提示完成网页授权。

### 2. 配置文件

项目已包含以下两个配置文件，无需手动创建：

- `firmware/wokwi.toml` — 指定编译产物路径（bin + elf）
- `firmware/diagram.json` — 定义模拟电路（ESP32-S3-DevKitC-1 开发板）

### 3. 编译固件

```bash
cd firmware
idf.py build
```

编译成功后会生成 `build/kidpalai.bin` 和 `build/kidpalai.elf`。

### 4. 启动模拟器

在 VSCode 中打开 `firmware/diagram.json`，然后：

- 点击右上角的 **▶ 启动模拟器** 按钮，或
- `Ctrl+Shift+P` → 输入 `Wokwi: Start Simulator`

模拟器会自动加载编译好的固件并开始运行，串口输出可在 VSCode 终端面板查看。

### 5. 注意事项

- WiFi 模拟：Wokwi 模拟器中的 WiFi 会通过宿主机网络连接，需确保网关服务（`gateway/`）已在本地或远程运行。
- 音频外设（INMP441 麦克风、MAX98357A 功放）在模拟器中为虚拟设备，I2S 数据收发逻辑可验证，但无法采集真实音频。
- 每次修改代码后需重新执行 `idf.py build`，再重启模拟器加载新固件。

---

## 五、硬件接线

| ESP32-S3 引脚 | INMP441 麦克风 | MAX98357A 功放 |
|--------------|--------------|----------------|
| GPIO 12      | SCK          |                |
| GPIO 11      | WS           |                |
| GPIO 10      | SD           |                |
| GPIO 6       |              | BCLK           |
| GPIO 5       |              | LRC            |
| GPIO 4       |              | DIN            |
| GPIO 1       | LED_R        |                |
| GPIO 2       | LED_G        |                |
| GPIO 3       | LED_B        |                |
