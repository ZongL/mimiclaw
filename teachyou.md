# ESP-IDF 踩坑：sdkconfig.defaults 分区表问题

## 症状

构建时报错：
```
Error: app partition is too small for binary mimiclaw.bin size 0x10ccb0:
  - Part 'factory' 0/0 @ 0x10000 size 0x100000 (overflow 0xccb0)
```

误以为是 partitions.csv 配置出错，反复修改分区表大小，但问题依然存在。

## 原因分析

ESP-IDF 只能**自动加载**特定命名的配置文件：

| ESP-IDF 自动加载 | 项目实际命名 | 结果 |
|----------------|-------------|------|
| `sdkconfig.defaults.esp32` | `sdkconfig.defaults.esp32cam` | ❌ 未加载 |

因为 `chip: esp32`，ESP-IDF 自动找 `sdkconfig.defaults.esp32`，但项目中是 `sdkconfig.defaults.esp32cam`。

没有正确加载自定义配置，导致 ESP-IDF 使用默认分区表（只有 1MB 的 factory 分区），而固件已超过 1MB。

## 解决方案

### 方案 1：修改文件名匹配 ESP-IDF 规范（推荐）

```bash
# chip: esp32 → 文件名必须叫 sdkconfig.defaults.esp32
mv sdkconfig.defaults.esp32cam sdkconfig.defaults.esp32
```

### 方案 2：构建时手动复制

在 workflow 中手动复制到 `sdkconfig.defaults`：
```bash
if [ -f "sdkconfig.defaults.esp32cam" ]; then
  cp sdkconfig.defaults.esp32cam sdkconfig.defaults
fi
```

## 教训

**分区表本身没问题**，问题在于配置文件没有被正确加载。

检查顺序：
1. 先确认分区表配置是否正确（CSV 文件）
2. 再确认配置文件是否被正确加载
3. 查看构建日志，看用的是 `factory` 还是 `ota` 分区

---

# ESP32-CAM Flash Memory Map (4MB)

基于 `partitions_esp32cam.csv` 和 bootloader 布局：

```
  Flash Address                        Size
  ┌──────────────────────────────┐ 0x000000
  │        Reserved (0xFF)       │    4 KB
  ├──────────────────────────────┤ 0x001000
  │        Bootloader            │   28 KB
  ├──────────────────────────────┤ 0x008000
  │      Partition Table         │    4 KB
  ├──────────────────────────────┤ 0x009000
  │        NVS                   │   20 KB
  ├──────────────────────────────┤ 0x00E000
  │       OTA Data               │    8 KB
  ├──────────────────────────────┤ 0x010000
  │                              │
  │        ota_0 (app)           │ 1216 KB (1.1875 MB)
  │                              │
  ├──────────────────────────────┤ 0x140000
  │                              │
  │        ota_1 (app)           │ 1216 KB (1.1875 MB)
  │                              │
  ├──────────────────────────────┤ 0x270000
  │                              │
  │        SPIFFS                │ 1536 KB (1.5 MB)
  │                              │
  ├──────────────────────────────┤ 0x3F0000
  │       Core Dump              │   64 KB
  └──────────────────────────────┘ 0x400000 (4 MB)
```

## Merged Bin 烧录说明

`esptool.py merge_bin` 生成的 full bin 内部布局与上图一致。

烧录命令：
```bash
# merged bin → 起始地址 0x0（内部已包含正确偏移）
esptool.py --chip esp32 -b 460800 write_flash 0x0 mimiclaw-esp32cam-full.bin
```

## 关键偏移地址（ESP32 vs ESP32-S3）

| 区域        | ESP32 (esp32cam) | ESP32-S3      |
|------------|------------------|---------------|
| Bootloader | **0x1000**       | 0x0           |
| 分区表      | 0x8000           | 0x8000        |
| OTA Data   | 0xE000           | 0xF000        |
| App        | 0x10000          | 0x20000       |

---

# ESP32-CAM 踩坑：QIO Flash 模式导致 Boot Loop

## 症状

烧录 merged bin 后，ESP32-CAM 无限重启：

```
rst:0x10 (RTCWDT_RTC_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
mode:QIO, clock div:2
load:0x3fff0030,len:6708        ← 第一段正常
load:0x8086002f,len:51663360    ← 第二段是垃圾值
1150 mmu set 00010000, pos 00010000
...（疯狂映射内存页）
rst:0x10 (RTCWDT_RTC_RESET)    ← WDT 超时，复位，无限循环
```

## 排查过程

### 1. 确认 flash 数据完整性

用 esptool 读出 flash 数据与原始 bin 做二进制对比：

```bash
esptool.py --chip esp32 read_flash 0x0 0x8000 readback.bin
```

结果：0x0 - 0x8000 完全一致，说明烧录没有问题。

### 2. 检查 bootloader 镜像信息

```bash
pip install esptool              # Windows 下安装
esptool image_info bootloader.bin  # Windows 用 esptool，不是 esptool.py
```

输出：

```
Segments Information
====================
Segment   Length   Load addr
      0  0x01a34  0x3fff0030   ← 串口日志匹配 ✅
      1  0x041d8  0x40078000   ← 串口日志显示 0x8086002f ❌
      2  0x010fc  0x40080400

Flash mode: DIO                  ← 注意：bootloader 编译出来是 DIO
```

### 3. 发现根本原因

| 来源 | Flash Mode |
|------|-----------|
| bootloader.bin 原始头部 | **DIO** |
| `merge_bin --flash_mode qio` | **QIO**（覆写了头部）|
| ROM 启动日志 `mode:QIO` | **QIO**（读取了被覆写的头部）|

`merge_bin` 的 `--flash_mode qio` 参数覆写了 merged bin 的文件头部。
ROM 启动时读取头部，以 QIO 模式访问 flash。
ESP32-CAM 的 flash 芯片在 QIO 下不稳定——前几 KB 能读对，后面返回垃圾数据。
esptool 回读用兼容模式，所以校验通过，但 ROM 启动时的 QIO 读取失败。

## 解决方案

### 方案 1：分开烧录（保持 bootloader 原始 DIO 头部）

烧录工具设置：
- SPI SPEED: **40MHz**
- SPI MODE: **DIO**
- DoNotChgBin: **勾选**（保持各 bin 原始头部）

四个文件及地址：

| 地址 | 文件 |
|------|------|
| `0x1000` | `bootloader-esp32cam-xxx.bin` |
| `0x8000` | `partition-table-esp32cam-xxx.bin` |
| `0xe000` | `ota_data_initial-esp32cam-xxx.bin` |
| `0x10000` | `mimiclaw-esp32cam-xxx.bin` |

步骤：
1. 添加以上四个文件，勾选每行前面的复选框
2. 先点 **ERASE** 擦除整个 flash
3. 再点 **START** 烧录
4. 烧录完成后，拔掉 GPIO0 跳线，按 RST

### 方案 2：merged bin 烧录时覆盖头部

烧录工具设置：
- SPI SPEED: **40MHz**
- SPI MODE: **DIO**
- DoNotChgBin: **不勾选**（工具用 DIO 覆盖 bin 头部的 QIO）
- 地址: **0x0**

### 方案 3：修改 workflow（根本解决）

将 `release.yml` 中的 `flash_mode` 从 `qio` 改为 `dio`：

```yaml
- name: esp32cam
  chip: esp32
  flash_mode: dio    # 改为 dio
```

## 教训

- `merge_bin --flash_mode` 会覆写输出文件的头部，与 bootloader 编译时的设置无关
- `DoNotChgBin` 勾选时，烧录工具不修改 bin 头部，ROM 按头部中的模式启动
- ESP32-CAM（AI-Thinker）的 flash 芯片可能不支持 QIO，使用 DIO 更安全
- 排查 boot loop 时，`esptool image_info` 是检查 bin 文件的利器

## 深入理解：ESP32 的 QIO/DIO 与 CONFIG_ESPTOOLPY_FLASHMODE

### sdkconfig 中的 QIO 配置不等于 bin 头部的 QIO

`sdkconfig.defaults.esp32` 中设置了：

```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
```

但 `esptool image_info` 显示编译出的 bootloader.bin 和 mimiclaw.bin 头部都是 **DIO**。

这**不是**配置没生效，而是 ESP-IDF **故意为之**。

### 根本原因：Kconfig 中的强制映射

ESP-IDF 的 `components/esptool_py/Kconfig.projbuild` 中定义了两个独立的配置项：

1. `CONFIG_ESPTOOLPY_FLASHMODE_QIO` — bool，用户在 menuconfig 中选择的意图
2. `CONFIG_ESPTOOLPY_FLASHMODE` — string，实际传给 esptool 写入 bin 头部的值

关键映射如下：

```kconfig
config ESPTOOLPY_FLASHMODE
    string
    default "dio" if ESPTOOLPY_FLASHMODE_QIO     # ← QIO 映射为 "dio"
    default "dio" if ESPTOOLPY_FLASHMODE_QOUT    # ← QOUT 映射为 "dio"
    default "dio" if ESPTOOLPY_FLASHMODE_DIO
    default "dout" if ESPTOOLPY_FLASHMODE_DOUT
```

**QIO、QOUT、DIO 三种模式全部映射为 `"dio"`**，只有 DOUT 映射为 `"dout"`。

因此编译生成的 sdkconfig 中会同时存在：
```
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y       # 用户意图：运行时用 QIO
CONFIG_ESPTOOLPY_FLASHMODE="dio"        # 实际写入 bin header 的值
```

这两行并不矛盾。

### 为什么必须这样做：ESP32 ROM bootloader 的限制

ESP32 ROM bootloader（烧录在芯片内部，不可修改）**只支持 DIO/DOUT 模式**读取 flash。

如果 bin 头部写了 QIO (`0x00`)，ROM 会尝试以 QIO 模式读取 flash，但 ROM 不会发送 flash 芯片所需的 QIO 使能命令（写 QE bit 等），导致 flash 芯片无法正确响应 → 读到垃圾数据 → boot loop。

### ESP32 启动时的 flash mode 切换流程

```
ROM 固件（芯片内置，不可修改）
  │  从 0x1000 读取 bootloader 头部 → Flash mode byte = 0x02 (DIO)
  │  以 DIO 模式加载 bootloader 到内存
  ▼
ESP-IDF Second-stage Bootloader（运行在 RAM 中）
  │  调用 bootloader_enable_qio_mode()
  │  读取 flash 芯片 manufacturer ID
  │  在已知芯片表中查找匹配项
  │  发送芯片特定命令启用 QIO（设置 status register 的 QE bit）
  │  重新配置 SPI 控制器为 QIO 读取模式
  │  以 QIO 模式加载 app
  ▼
App 运行（QIO 模式，如果硬件支持）
```

### 如何验证 QIO 确实在运行时生效

`esptool image_info` 看到 DIO 是正常的，要确认 QIO 实际生效，检查串口启动日志：

```
I (32) qio_mode: Enabling default flash chip QIO    ← bootloader 正在切换到 QIO
I (36) boot.esp32: SPI Speed      : 40MHz
I (40) boot.esp32: SPI Mode       : QIO              ← 确认 QIO 已生效
...
I (603) spi_flash: flash io: qio                     ← app 运行时也是 QIO
```

### 验证总结表

| 查看方式 | 显示值 | 说明 |
|---|---|---|
| `esptool image_info *.bin` | `Flash mode: DIO` | 正常 — bin header 故意写 DIO |
| 串口日志 `boot.esp32: SPI Mode` | `QIO` | 正常 — bootloader 运行时切换到 QIO |
| 串口日志 `spi_flash: flash io` | `qio` | 正常 — app 以 QIO 模式访问 flash |

### merge_bin --flash_mode qio 为什么会破坏启动

`merge_bin --flash_mode qio` 强制把 merged bin 的头部从 DIO 改写为 QIO。
这绕过了上述流程，导致 ROM 直接以 QIO 模式读取 flash。
由于 ROM 没有执行 bootloader 的软件切换流程，flash 芯片无法正确响应 QIO 命令。

### 罪魁祸首：一个字节的差异

通过二进制对比 QIO 和 DIO 的 merged bin，发现差异仅在 **offset 0x1002** 的一个字节：

```
ESP Image Header（位于 merged bin 的 0x1000，即 bootloader 起始位置）

偏移      含义              QIO 值    DIO 值
0x1000    Magic             0xE9      0xE9      (不变)
0x1001    Segment count     0x03      0x03      (不变)
0x1002    Flash mode        0x00      0x02      ← 唯一变化
0x1003    Flash size+freq   ...       ...       (不变)

Flash mode 编码：0x00=QIO, 0x01=QOUT, 0x02=DIO, 0x03=DOUT
```

ESP-IDF 编译出的 bootloader.bin 头部是 `E9 03 02`（DIO），
但 `merge_bin --flash_mode qio` 把 `0x02` 改写成了 `0x00`（QIO）。

ROM 读到 `0x00` 就直接用 QIO 模式访问 flash，前几 KB 能读对，
之后返回垃圾数据，导致无限 boot loop。

### 最终修复

将 `release.yml` 中 `merge_bin` 的 `flash_mode` 改为 `dio`，保持与编译产物一致：

```yaml
# release.yml
- name: esp32cam
  chip: esp32
  flash_mode: dio    # 与 ESP-IDF 编译产物一致
```

`sdkconfig.defaults.esp32` 中的 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 不需要修改，
ESP-IDF 会自动处理：编译时头部写 DIO，运行时 bootloader 切换到 QIO。

---

# GitHub Actions 测试

如果只是想测试 Actions 会不会工作，不想正式发布，可以：

# 创建临时标签，测试通过后删除
git tag v0.2.0-test
git push origin v0.2.0-test

# 测试通过后删除标签
git tag -d v0.2.0-test
git push origin :refs/tags/v0.2.0-test

---

# GitHub Actions 多目标构建配置说明

## 当前状态

当前 `release.yml` 工作流只构建 **ESP32-CAM** 目标。

## 如何恢复 ESP32-S3 构建

如果需要同时构建 ESP32-S3 和 ESP32-CAM，在 `.github/workflows/release.yml` 的 `matrix.target` 部分重新添加 ESP32-S3 配置：

### 步骤 1: 编辑 `.github/workflows/release.yml`

找到 `strategy.matrix.target` 部分，将：

```yaml
strategy:
  matrix:
    target:
      - name: esp32cam
        chip: esp32
        flash_mode: qio
        flash_size: 4MB
        flash_freq: 40m
        bootloader_offset: 0x0
        bin_offset: 0x10000
        partition_offset: 0x9000
        ota_offset: 0xf000
```

修改为：

```yaml
strategy:
  matrix:
    target:
      - name: esp32s3
        chip: esp32s3
        flash_mode: qio
        flash_size: 16MB
        flash_freq: 80m
        bootloader_offset: 0x0
        bin_offset: 0x20000
        partition_offset: 0x8000
        ota_offset: 0xf000
      - name: esp32cam
        chip: esp32
        flash_mode: qio
        flash_size: 4MB
        flash_freq: 40m
        bootloader_offset: 0x0
        bin_offset: 0x10000
        partition_offset: 0x9000
        ota_offset: 0xf000
```

### 步骤 2: 更新发布说明

在同一个文件中，找到 `body:` 部分，重新添加 ESP32-S3 说明并恢复完整的发布说明。

### 步骤 3: 更新发布文件列表

在 `files:` 部分重新添加 ESP32-S3 固件文件：
```
mimiclaw-esp32s3-full-${{ github.ref_name }}.bin
mimiclaw-esp32s3-${{ github.ref_name }}.bin
bootloader-esp32s3-${{ github.ref_name }}.bin
partition-table-esp32s3-${{ github.ref_name }}.bin
ota_data_initial-esp32s3-${{ github.ref_name }}.bin
```

---

## 触发构建

```bash
# 提交修改
git add .github/workflows/release.yml
git commit -m "workflow: restore ESP32-S3 build"
git push origin main

# 创建标签触发构建
git tag v0.x.x
git push origin v0.x.x
```

### ItisRunning
```
ets Jul 29 2019 12:21:46

rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:2
load:0x3fff0030,len:6708
load:0x40078000,len:16856
load:0x40080400,len:4348
entry 0x40080664
I (27) boot: ESP-IDF v5.5.2 2nd stage bootloader
I (27) boot: compile time Feb 13 2026 03:32:25
I (28) boot: Multicore bootloader
I (29) boot: chip revision: v3.1
I (32) qio_mode: Enabling default flash chip QIO
I (36) boot.esp32: SPI Speed      : 40MHz
I (40) boot.esp32: SPI Mode       : QIO
I (43) boot.esp32: SPI Flash Size : 4MB
I (47) boot: Enabling RNG early entropy source...
I (51) boot: Partition Table:
I (54) boot: ## Label            Usage          Type ST Offset   Length
I (60) boot:  0 nvs              WiFi data        01 02 00009000 00005000
I (67) boot:  1 otadata          OTA data         01 00 0000e000 00002000
I (73) boot:  2 ota_0            OTA app          00 10 00010000 00130000
I (80) boot:  3 ota_1            OTA app          00 11 00140000 00130000
I (86) boot:  4 spiffs           Unknown data     01 82 00270000 00180000
I (93) boot:  5 coredump         Unknown data     01 03 003f0000 00010000
I (99) boot: End of partition table
I (103) esp_image: segment 0: paddr=00010020 vaddr=3f400020 size=3b620h (243232) map
I (180) esp_image: segment 1: paddr=0004b648 vaddr=3ffb0000 size=045cch ( 17868) load
I (186) esp_image: segment 2: paddr=0004fc1c vaddr=40080000 size=003fch (  1020) load
I (187) esp_image: segment 3: paddr=00050020 vaddr=400d0020 size=c3408h (799752) map
I (422) esp_image: segment 4: paddr=00113430 vaddr=400803fc size=1ead8h (125656) load
I (465) esp_image: segment 5: paddr=00131f10 vaddr=50000000 size=00020h (    32) load
I (480) boot: Loaded app from partition at offset 0x10000
I (480) boot: Disabling RNG early entropy source...
I (490) quad_psram: This chip is ESP32-D0WD
I (491) esp_psram: Found 8MB PSRAM device
I (491) esp_psram: Speed: 40MHz
I (491) esp_psram: PSRAM initialized, cache is in low/high (2-core) mode.
W (498) esp_psram: Virtual address not enough for PSRAM, map as much as we can. 4MB is mapped
I (506) cpu_start: Multicore app
I (520) cpu_start: GPIO 3 and 1 are used as console UART I/O pins
I (521) cpu_start: Pro cpu start user code
I (521) cpu_start: cpu freq: 240000000 Hz
I (524) app_init: Application information:
I (528) app_init: Project name:     mimiclaw
I (532) app_init: App version:      1
I (536) app_init: Compile time:     Feb 13 2026 03:32:19
I (541) app_init: ELF file SHA256:  529e662c2...
I (545) app_init: ESP-IDF:          v5.5.2
I (549) efuse_init: Min chip rev:     v0.0
I (553) efuse_init: Max chip rev:     v3.99 
I (557) efuse_init: Chip rev:         v3.1
I (561) heap_init: Initializing. RAM available for dynamic allocation:
I (567) heap_init: At 3FFAE6E0 len 00001920 (6 KiB): DRAM
I (572) heap_init: At 3FFB9328 len 00026CD8 (155 KiB): DRAM
I (577) heap_init: At 3FFE0440 len 00003AE0 (14 KiB): D/IRAM
I (583) heap_init: At 3FFE4350 len 0001BCB0 (111 KiB): D/IRAM
I (588) heap_init: At 4009EED4 len 0000112C (4 KiB): IRAM
I (594) esp_psram: Adding pool of 4096K of PSRAM memory to heap allocator
I (601) spi_flash: detected chip: generic
I (603) spi_flash: flash io: qio
I (607) main_task: Started on CPU0
I (617) esp_psram: Reserving pool of 64K of internal memory for DMA/internal allocations
I (617) main_task: Calling app_main()
I (617) mimi: ========================================
I (617) mimi:   MimiClaw - ESP32-S3 AI Agent
I (627) mimi: ========================================
I (627) mimi: Internal free: 277203 bytes
I (637) mimi: PSRAM free:    4192024 bytes
I (747) mimi: SPIFFS: total=1438481, used=0
I (747) bus: Message bus initialized (queue depth 8)
I (747) memory: Memory store initialized at /spiffs
I (747) session: Session manager initialized at /spiffs/sessions
I (767) wifi:wifi driver task: 3ffc1b30, prio:23, stack:6656, core=0
I (767) wifi:wifi firmware version: ee91c8c
I (767) wifi:wifi certification version: v7.0
I (767) wifi:config NVS flash: enabled
I (777) wifi:config nano formatting: disabled
I (777) wifi:Init data frame dynamic rx buffer num: 4
I (787) wifi:Init static rx mgmt buffer num: 5
I (787) wifi:Init management short buffer num: 32
I (787) wifi:Init dynamic tx buffer num: 32
I (797) wifi:Init static rx buffer size: 1600
I (797) wifi:Init static rx buffer num: 2
I (807) wifi:Init dynamic rx buffer num: 4
I (807) wifi_init: rx ba win: 2
I (807) wifi_init: accept mbox: 6
I (817) wifi_init: tcpip mbox: 12
I (817) wifi_init: udp mbox: 6
I (817) wifi_init: tcp mbox: 6
I (817) wifi_init: tcp tx win: 5760
I (827) wifi_init: tcp rx win: 5760
I (827) wifi_init: tcp mss: 1440
I (827) wifi_init: WiFi IRAM OP enabled
I (837) wifi_init: WiFi RX IRAM OP enabled
I (837) wifi: WiFi manager initialized
W (847) telegram: No Telegram bot token. Use CLI: set_tg_token <TOKEN>
W (847) llm: No API key. Use CLI: set_api_key <KEY>
W (857) web_search: No search API key. Use CLI: set_search_key <KEY>
I (857) tools: Registered tool: web_search
I (867) tools: Registered tool: get_current_time
I (867) tools: Registered tool: read_file
I (867) tools: Registered tool: write_file
I (877) tools: Registered tool: edit_file
I (877) tools: Registered tool: list_dir
I (887) tools: Tools JSON built (6 tools)
I (887) tools: Tool registry initialized
I (887) agent: Agent loop initialized
[5n
Type 'help' to get the list of commands.
Use UP/DOWN arrows to navigate through command history.
Press TAB when typing command name to auto-complete.

Your terminal application does not support escape sequences.

Line editing and history features are disabled.

On Windows, try using Windows Terminal or Putty instead.
mimi>  I (1917) cli: Serial CLI started
W (1947) wifi: No WiFi credentials. Use CLI: wifi_set <SSID> <PASS>
W (1947) mimi: No WiFi credentials. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h
I (1957) mimi: MimiClaw ready. Type 'help' for CLI commands.
I (1957) main_task: Returned from app_main()
help
help  [<string>] [-v <0|1>]
  Print the summary of all registered commands if no arguments are given,
  otherwise print summary of given command.
      <string>  Name of command
  -v, --verbose=<0|1>  If specified, list console commands with given verbose level

wifi_set  <ssid> <password>
  Set WiFi SSID and password
        <ssid>  WiFi SSID
    <password>  WiFi password

wifi_status 
  Show WiFi connection status

wifi_scan 
  Scan and list nearby WiFi APs

set_tg_token  <token>
  Set Telegram bot token
       <token>  Telegram bot token

set_api_key  <key>
  Set LLM API key
         <key>  LLM API key

set_model  <model>
  Set LLM model (default: claude-opus-4-5)
       <model>  Model identifier

set_model_provider  <provider>
  Set LLM model provider (default: anthropic)
    <provider>  Model provider (anthropic|openai)

memory_read 
  Read MEMORY.md

memory_write  <content>
  Write to MEMORY.md
     <content>  Content to write

session_list 
  List all sessions

session_clear  <chat_id>
  Clear a session
     <chat_id>  Chat ID to clear

heap_info 
  Show heap memory usage

set_search_key  <key>
  Set Brave Search API key for web_search tool
         <key>  Brave Search API key

set_proxy  <host> <port>
  Set HTTP proxy (e.g. set_proxy 192.168.1.83 7897)
        <host>  Proxy host/IP
        <port>  Proxy port

clear_proxy 
  Remove proxy configuration

config_show 
  Show current configuration (build-time + NVS)

config_reset 
  Clear all NVS overrides, revert to build-time defaults

restart 
  Restart the device

mimi>  
mimi>  
mimi>  
```