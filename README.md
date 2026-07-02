# ESP32-S3 BLE 配置 + MQTT (HiveMQ) + OTA 升级

通过 **Web Bluetooth** 连接 ESP32-S3，在网页上配置 WiFi、MQTT (HiveMQ Cloud)、触发 OTA 升级。无需串口屏，所有配置在浏览器中完成。

## 系统架构

```
┌─────────────────────────────────────────────────────┐
│                    浏览器 (Web Bluetooth)            │
│            扫描 → 配网 → MQTT 配置 → OTA              │
└─────────────────────────────────────────────────────┘
                        BLE GATT
┌─────────────────────────────────────────────────────┐
│                     ESP32-S3                        │
│  BLE GATT Service (WiFi/MQTT/OTA) + NVS 存储        │
└─────────────────────────────────────────────────────┘
                   WiFi + MQTT TLS
                        ▼
┌─────────────────────────────────────────────────────┐
│                  HiveMQ Cloud                        │
│        broker: xxxx.s1.eu.hivemq.cloud:8883          │
└─────────────────────────────────────────────────────┘
```

---

## 目录结构

```
esp32-ble-ota/
├── esp32_firmware/
│   └── ESP32_BLE_OTA.ino          ← Arduino 固件
├── web_config/
│   └── index.html                ← Web 配置页面 (直接用浏览器打开)
└── docs/
    └── ARCHITECTURE.md            ← 详细架构文档
```

---

## 第一步：HiveMQ Cloud 配置

### 1.1 创建 HiveMQ Cloud 集群 (免费)

1. 访问 [cloud.hivemq.com](https://console.hivemq.cloud/)
2. 用 GitHub/Google 账号登录
3. 点击 **"Create a Cluster"**
   - 选择 **Serverless** (免费额度)
   - Region: 选离你最近的 (如 `EU Central`)
4. 等待集群创建完成

### 1.2 获取连接凭证

1. 进入集群控制台 → **"Access Management"**
2. 点击 **"Create Credentials"**
3. 填写：
   - **Username**: `esp32-client` (随便起)
   - **Password**: `your-strong-password`
4. 点击创建

### 1.3 记录连接信息

```
Broker URL:   xxxxx.s1.eu.hivemq.cloud   ← 在集群概览页查看
Port (TLS):   8883
Username:     esp32-client              ← 你创建的
Password:     your-strong-password
```

> **注意**: HiveMQ Cloud 免费 Serverless 集群会休眠（无流量 30 分钟后）。生产环境建议使用 Professional 或 Broker 集群。

---

## 第二步：Arduino IDE 配置

### 2.1 安装 ESP32 开发板

1. 打开 Arduino IDE → **Preferences**
2. 在 **Additional Board Manager URLs** 添加:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. 打开 **Tools** → **Board** → **Board Manager**
4. 搜索 `esp32`，安装 **esp32 by Espressif Systems**

### 2.2 安装 PubSubClient 库

1. **Sketch** → **Include Library** → **Manage Libraries**
2. 搜索 `PubSubClient`，安装 **PubSubClient by Nick O'Leary**

### 2.3 配置开发板

| 设置 | 值 |
|------|-----|
| Board | `ESP32S3 Dev Module` |
| USB CDC On Boot | `Enabled` (有 USB 串口用这个) |
| CPU Frequency | `240MHz (WiFi/BT)` |
| Core Debug Level | `None` |
| Flash Mode | `QIO 80MHz` |
| Flash Size | `4MB (32Mb)` 或 `8MB` |
| **Partition Scheme** | **`APP with OTA`** ← **OTA 必须选这个!** |
| PSRAM | `Disabled` |
| Upload Speed | `921600` |

> ⚠️ **Partition Scheme 必须选 `APP with OTA`**！否则 OTA 升级会失败，因为默认分区表没有第二个 APP 分区。

### 2.4 固件分区说明

`APP with OTA` 分区布局:
```
| Name   | Type | SubType | Offset  | Size     |
|--------|------|---------|---------|----------|
| nvs    | data | wifi_nvs| 0x009000 | 0x005000 |
| otadata| data | ota     | 0x00e000 | 0x002000 |
| app0   | app  | ota_0   | 0x010000 | 0x140000 |  ← 1.25MB
| app1   | app  | ota_1   | 0x150000 | 0x140000 |  ← 1.25MB
| spiffs | data | spiffs  | 0x290000 | 0x170000 |
```

OTA 时 ESP32 会写入 `app1`，然后切换启动分区。

---

## 第三步：烧录固件

### 3.1 编译上传

1. 打开 `esp32_firmware/ESP32_BLE_OTA.ino`
2. 连接 ESP32-S3 到电脑的 USB
3. **Tools** → **Port** 选择对应 COM 口
4. 点击 **Upload** (→)
5. 等待上传完成，串口监视器显示:
   ```
   ESP32-S3 BLE Config + MQTT + OTA
   [BLE] Advertising as 'ESP32_Config'
   [BLE] Service UUID: 12345678-1234-1234-1234-123456789abc
   [NVS] Initialized
   ```

### 3.2 BLE 设备名称

固件广播的 BLE 设备名为: **`ESP32_Config`**

---

## 第四步：Web 配置页面

### 4.1 打开页面

直接双击打开 `web_config/index.html`，或者：

```bash
cd esp32-ble-ota/web_config
python3 -m http.server 8080
# 然后浏览器访问 http://localhost:8080
```

### 4.2 连接 ESP32

1. 点击 **"连接设备"**
2. 浏览器弹出 BLE 设备选择器
3. 选择 **`ESP32_Config`**
4. 等待连接成功，状态变为 "已连接"

> ⚠️ **注意**: Web Bluetooth 目前仅支持 **Chrome / Edge / Opera** (桌面和安卓)。iOS Safari 不支持 Web Bluetooth API。

---

## 第五步：配置使用流程

### 5.1 配置 WiFi

1. 点击 **"扫描 WiFi"**
2. 从列表中选择你的 WiFi (带 🔒 锁表示需要密码)
3. 输入 WiFi 密码
4. 选择:
   - **"💾 保存 WiFi"** — 只保存到 ESP32 NVS，不连接
   - **"⚡ 保存并连接"** — 保存 + 自动连接

### 5.2 配置 MQTT (HiveMQ)

1. 在 MQTT 配置区填入 HiveMQ 信息:
   ```
   Broker URL:   xxxxx.s1.eu.hivemq.cloud
   端口:         8883
   用户名:        esp32-client
   密码:          your-strong-password
   ```
2. 点击 **"💾 保存 MQTT 配置"**

### 5.3 触发 OTA 升级

1. 填入固件 HTTPS URL (见下方 OTA 固件制作说明)
2. 点击 **"🚀 开始 OTA 升级"**
3. 页面显示进度条，ESP32 自动下载并烧录固件
4. 完成后 ESP32 自动重启

---

## OTA 固件制作与托管

### 编译固件

1. Arduino IDE: **Sketch** → **Export compiled Binary**
2. 编译产物在 sketch 目录下 (`.ino.bin` 或 `.ino.esp32s3.bin`)
3. 重命名为 `firmware.bin`

### 托管固件

你需要将 `.bin` 文件放到一个 HTTPS 服务器上。

**免费方案**:
- **GitHub Releases**: 上传固件到 GitHub private repo → 创建 Release → 使用 `https://raw.githubusercontent.com/.../firmware.bin` (需配置 CORS) 或通过 jsDelivr CDN
- **Cloudflare R2**: 免费 10GB 存储
- **你自己的服务器**: 确保支持 Range 请求 (ESP32 OTA 需要)

**推荐方案 — GitHub + jsDelivr CDN**:
```
https://cdn.jsdelivr.net/gh/YOUR_USERNAME/YOUR_REPO@latest/firmware.bin
```

### OTA 注意事项

- 固件 URL 必须是 **HTTPS**
- 固件必须是为 **ESP32-S3** 编译的
- 固件大小不能超过 `app1` 分区大小 (约 1.25MB)
- 编译时选择的 **Partition Scheme** 要和当前运行固件一致

---

## MQTT Topic 结构

| Topic | 方向 | 说明 |
|-------|------|------|
| `esp32/<device_id>/config` | 网站 → ESP32 | 配置命令 |
| `esp32/<device_id>/status` | ESP32 → 网站 | 状态上报 |
| `esp32/<device_id>/data` | 双向 | 数据通道 |

**device_id** 格式: `esp32_<MAC地址十六进制>` (例如 `esp32_aabbccddeeff`)

---

## BLE GATT Service 详解

### Service UUID
`12345678-1234-1234-1234-123456789abc`

### Characteristics

| UUID | 名称 | 属性 | 用途 |
|------|------|------|------|
| `...ab1` | WiFi Scan | R+N | ESP32 返回 WiFi 扫描结果 |
| `...ab2` | WiFi Config | W | 发送 SSID + 密码 |
| `...ab3` | MQTT Config | W | 发送 Broker + Port + User + Pass |
| `...ab4` | OTA URL | W | 发送 HTTPS 固件 URL |
| `...ab5` | Control | W | 发送控制命令 |
| `...ab6` | Status | R+N | ESP32 上报状态 |

### 控制命令 (写入 Control Characteristic)

| 命令码 | 说明 |
|--------|------|
| `0x01` | 扫描 WiFi |
| `0x02` | 连接已保存的 WiFi |
| `0x03` | 连接 MQTT |
| `0x04` | 开始 OTA |
| `0x05` | 保存所有配置 |
| `0x06` | 重置所有配置 |
| `0x07` | 请求当前状态 |

### 状态码 (从 Status Characteristic 读取)

| 码 | 说明 |
|----|------|
| `0x00` | 空闲 |
| `0x01` | 扫描中 |
| `0x02` | WiFi 连接中 |
| `0x03` | WiFi 已连接 |
| `0x04` | WiFi 失败 |
| `0x05` | MQTT 连接中 |
| `0x06` | MQTT 已连接 |
| `0x07` | MQTT 失败 |
| `0x08` | OTA 开始 |
| `0x09` | OTA 进度 |
| `0x0A` | OTA 成功 |
| `0x0B` | OTA 失败 |
| `0xF0` | OK |
| `0xFF` | 错误 |

---

## 常见问题

### Q: Web Bluetooth 扫描不到设备?
- 确保 ESP32 已烧录固件并正常启动
- 确保浏览器支持 Web Bluetooth (Chrome/Edge/Opera)
- 手机端: Android + Chrome 可以; iOS Safari 不支持

### Q: WiFi 连接成功但 MQTT 连不上?
- 检查 HiveMQ 凭证是否正确
- 检查 Broker URL 和端口
- HiveMQ Cloud 免费版 30 分钟无活动会休眠，首次连接可能需要等待

### Q: OTA 失败?
- 确认固件 URL 是 HTTPS
- 确认固件是为 ESP32-S3 编译的
- 确认 Partition Scheme 是 `APP with OTA`
- 确认固件大小 < 1.25MB

### Q: 如何查看 ESP32 串口日志?
- Arduino IDE 串口监视器，波特率 `115200`

### Q: 浏览器控制台报错?
- 打开浏览器 DevTools (F12) → Console 查看详细错误

---

## 进阶定制

### 修改 BLE 设备名
在 `ESP32_BLE_OTA.ino` 中修改:
```cpp
#define BLE_DEVICE_NAME "你的设备名"
```

### 修改 BLE Service UUID
修改所有 `SERVICE_UUID` 和 `CHAR_*` 常量，确保 ESP32 和网页端一致。

### 启用 MQTT TLS 证书验证
将 HiveMQ 的 CA 证书转成字符串，替换固件中的:
```cpp
wifiClientSecure->setInsecure(); // 改为:
wifiClientSecure->setCACert(ca_cert);
```

### WebSocket 模式连接 MQTT
HiveMQ 支持 WebSocket，端口 `8884`:
- Broker URL 格式相同
- Port 填 `8884`
- 在 MQTT.js 或网页端需要 WebSocket 支持

---

## 依赖版本

- **Arduino IDE**: 1.8.x 或 2.x
- **ESP32 Arduino Core**: 2.0.x 或更高
- **PubSubClient**: 2.8.x
- **ESP32-S3**: 乐鑫 ESP32-S3-DevKitC-1 或兼容板
- **浏览器**: Chrome 56+ / Edge 79+ / Opera 43+
