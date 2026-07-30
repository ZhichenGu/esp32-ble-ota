# ESP32-S3 配网 + HiveMQ + OTA 模板

一套"浏览器扫蓝牙 → 配 WiFi → 配 HiveMQ → 空中升级(OTA)"的可复用方案，
专为 **ESP32-S3-N16R8**（16MB Flash / 8MB OPI PSRAM）设计。

## 文件

| 文件 | 作用 |
|---|---|
| `provision.h` | **可复用核心**：BLE 配网 + WiFi + MQTT(TLS) + NVS 持久化 + BLE-OTA + URL-OTA。**每个固件都要 `#include` 它** |
| `esp32_ota_provision.ino` | 主程序骨架，业务逻辑写在标注的"业务区" |
| `partitions.csv` | 16MB 双 OTA 分区表（ota_0 / ota_1 各 4MB，支持回滚） |
| `index.html` | 网页控制台（托管到 GitHub Pages） |
| `test_firmware_blink.bin` | 测试固件：刷进去板载灯会闪，用来验证 OTA |

## 编译 / 烧录设置（Arduino IDE 工具菜单）

- 开发板：**ESP32S3 Dev Module**
- Flash Size：**16MB (128Mb)**
- PSRAM：**OPI PSRAM**
- Partition Scheme：**Custom**（会自动读本目录 `partitions.csv`）
- USB CDC On Boot：**Enabled**

命令行等价（本机 Arduino IDE 自带 arduino-cli）：

```bash
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=custom,CDCOnBoot=cdc"
"$CLI" compile --fqbn "$FQBN" esp32_ota_provision
"$CLI" upload -p /dev/cu.usbmodem2101 --fqbn "$FQBN" esp32_ota_provision
```

## ⚠️ 最重要：怎样保证 OTA 后功能不丢

固件 A 通过 OTA 换成固件 B 后，**能不能再配网/再 OTA，完全取决于固件 B 里有没有这套代码**。
所以只要遵守一条纪律：

> **以后每一个要 OTA 上去的固件，都 `#include "provision.h"` 并调用 `provisionSetup()` / `provisionLoop()`。**

只要不违反，能力永远都在。另加两道保险：

1. **双 OTA 分区**：刷坏了还能用 USB 刷回，或（进阶）用 `esp_ota` 回滚到上一个好固件。
2. **开机 BLE 窗口**：每次上电 BLE 都会广播，随时能重新连上重配/重刷。
3. **USB 永远是终极后路**：BLE 完全刷废了，插线用上面的命令 5 秒刷回。

## 网页托管到 GitHub Pages

把 `index.html` 传到仓库，Settings → Pages 选分支即可。Web Bluetooth 要求 **https**，GitHub Pages 天然满足。
注意：**只有桌面/安卓的 Chrome、Edge 支持 Web Bluetooth，iOS/Safari 不支持**。

## 两条 OTA 通道（按 BLE 是否还在）

| 通道 | 何时用 | 说明 |
|---|---|---|
| **A. 蓝牙直传** | 蓝牙还连着（还没连 MQTT） | 网页选本地 `*.ino.bin`，经 BLE 分片上传，带进度条。约 1~3 分钟 |
| **B. URL 拉取** | 任何时候（含蓝牙已释放） | 网页填固件直链，ESP 自己下载刷写；也可经 MQTT 发布到 `前缀/ota` 远程触发。**大固件更快** |

> 为什么分两条：连 HiveMQ 的 TLS 握手需要一大块连续内存，代码会 `freeBle()` 释放蓝牙。
> 蓝牙一释放，通道 A 就用不了，这时用通道 B。

## 测试 OTA（推荐流程）

1. USB 刷入 `esp32_ota_provision`（已帮你刷好）。
2. 打开网页 → 连蓝牙 → 配 WiFi。
3. **通道 A**：第 5 步选 `test_firmware_blink.bin` → 上传。完成后设备重启，**板载灯开始闪** = OTA 成功。
4. 重新连蓝牙 → 说明升级后能力**没丢**。再把 `esp32_ota_provision.ino.bin` 刷回即可。
5. **通道 B**：把某个 `.bin` 传到 GitHub Release，拿到直链，填进"方式 B"→ 拉取。

## MQTT 主题约定

- `前缀/status`：设备上/下线（online / offline / updated）
- `前缀/ota`：往这里发布固件 URL 即远程 OTA
- 业务主题：在 `esp32_ota_provision.ino` 的 `onMyMqttConnect()` 里自行 `mqtt.subscribe(...)`

## 已相对你原始需求补充/修正的点

1. **NVS 持久化**：原 servo 代码重启即丢 WiFi/MQTT 配置，这里存进 NVS，开机自动重连。
2. **分区表必须改**：原来的 "Huge APP (No OTA)" 根本不能 OTA，已换双 OTA 分区。
3. **本地 .bin 只能走 BLE**：GitHub Pages(https) 无法直接 POST 给局域网 http 的 ESP（浏览器拦截混合内容），所以本地包走蓝牙，大包走 URL。
4. **浏览器限制**：Web Bluetooth 仅 Chrome/Edge，iOS 不支持——需提前知晓。
5. **可复用化**：把配网/OTA 抽成 `provision.h`，这是"功能不丢"的根本保证。
