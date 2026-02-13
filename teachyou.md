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