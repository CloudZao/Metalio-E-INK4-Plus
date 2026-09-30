# SD 卡升级 `font_data` / `resources` 方案

面向量产后用户：**不拆机、不整包 USB 刷机**，把升级包拷到 SD（或虚拟 U 盘），设备写入对应 Flash 分区。  
与「传输」下发的阅读 `.ef` / 壁纸 **无关**。

---

## 1. 目标与非目标

### 目标

| 分区 | Label | 容量（v1/16m） | 升级内容 |
|------|--------|----------------|----------|
| UI 字库 | `font_data` | 5MB @ `0xae4000` | `use_font/font.fontpack`（EFNT，含 Math） |
| 内置资源 | `resources` | 400KB @ `0xa80000` | 构建生成的 mmap_assets / SPIFFS 资源镜像 |

- 支持 **分别升级**（两个文件）或 **合并成一个包** 一次升两边。
- 校验失败不写分区；写入失败可重试；尽量避免因校验硬挂导致无法开机。

### 非目标

- 不替代 App OTA（`ota_0` / `ota_1`）。
- 不把 fontpack 塞进 `resources`（400KB 不够）。
- 不改为长期从 SD 实时读字库（慢、依赖插卡）。

---

## 2. 现状与约束

| 项 | `font_data` | `resources` |
|----|-------------|-------------|
| 运行时加载 | `font_loader_init_flash` mmap | `mmap_assets_new("resources")` |
| 出厂写入 | CMake → `use_font/font.fontpack` | `spiffs_create_partition_assets(xingzhi-assets)` |
| SD→分区写入 | **未实现** | **未实现**（`Assets::Download` 写的是 `assets` 标签，v1 无此分区） |
| 启动强校验 | magic/version 弱校验 | **编译期** `MMAP_RESOURCES_CHECKSUM` + `MMAP_RESOURCES_FILES`，失败 `ESP_ERROR_CHECK` 会 abort |

**关键**：若只升 `resources`、固件仍是旧 checksum，启动可能直接挂。方案里必须「放宽启动校验」或「资源包与固件版本绑定」。

---

## 3. 总体架构

```
┌─────────────┐     拷贝      ┌──────────────────┐
│ PC / U 盘   │ ───────────► │ SD: metalio/...  │
│ 升级包      │              └────────┬─────────┘
└─────────────┘                       │
                                      ▼
                          ┌───────────────────────┐
                          │ 设置「系统升级」或开机扫描 │
                          │ 校验 → 擦写分区 → 重启   │
                          └───────────────────────┘
                                      │
              ┌───────────────────────┼───────────────────────┐
              ▼                       ▼                       ▼
        font_data              resources                 NVS 记录版本
        (EFNT 包)           (mmap 资源镜像)            last_ok / pending
```

日常运行仍只读 Flash mmap，**不**依赖 SD 常驻字库。

---

## 4. SD 路径约定（建议）

根目录相对 SD 挂载点（用户视角 `metalio/e-ink/...`）：

```
metalio/e-ink/system/
├── font.fontpack              # 仅升字库（可选）
├── resources.bin              # 仅升资源（可选，构建产物整包）
└── metalio_sys.pkg            # 合并包（推荐用户只发这一个）
```

虚拟 U 盘启用时，用户在电脑上看到同一路径即可拖拽。

---

## 5. 分别升级（双文件）

### 5.1 字库 `font.fontpack`

1. 打开文件，读 32B 头：magic=`EFNT`，version=`1`。  
2. `file_size ≤ font_data.size`（5MB），且 `file_size ≥ header + index` 合理下限。  
3. 可选：与 NVS 中 `fontpack_crc32` / `fontpack_size` 比较，相同则跳过。  
4. `esp_partition_erase_range` 整分区（或按文件向上对齐扇区）。  
5. 分块 `esp_partition_write`（建议 4KB），进度回调到 UI。  
6. 写完读回头校验 magic；成功则更新 NVS，删除或改名 SD 文件（防反复刷）。  
7. `font_loader_deinit()` → `font_loader_init_flash()`，或直接 **重启**（更稳）。

### 5.2 资源 `resources.bin`

1. 文件必须是 **与 `idf.py` 刷 `resources` 相同格式** 的分区镜像（构建生成），不是散文件 zip。  
2. `file_size ≤ 400KB`。  
3. 解析/核对包内文件数、自带 checksum（若镜像带头）；与当前策略见 §7。  
4. 同字库：擦除 → 写入 → 校验。  
5. 热重载 `mmap_assets` **困难**（已挂 LVGL FS），**强制重启**。

构建侧产出示例（名称按工程脚本调整）：

```bash
# 编译后从 build / flasher 中取出 resources 分区镜像
# 或单独导出：与 merge_firmware / parttool 读出的内容一致
cp <build产出的resources镜像> metalio_sys包内的 resources.bin
```

---

## 6. 合并成一个文件：支持，且推荐

**可以合并。** 建议自定义容器（MCU 友好，不依赖 zip），一次拷贝升两边。

### 6.1 容器格式 `metalio_sys.pkg`（草案）

小端；总布局：

```
Offset  Size   Field
0       4      magic = "MZPK"          # Metalio Zone PacKage
4       2      version = 1
6       2      flags                 # bit0=含 font，bit1=含 resources
8       4      header_crc32          # 覆盖其后到 header 末（不含本字段）
12      4      pkg_total_size        # 整文件字节数
16      4      min_app_version       # 可选：低于此固件拒绝（BCD 或 PROJECT_VER 哈希）
20      4      reserved0
24      4      font_offset           # 相对文件头；0=无
28      4      font_size
32      4      font_crc32
36      4      res_offset
40      4      res_size
44      4      res_crc32
48      ...    payload（font 与/或 resources 裸镜像，可 4B 对齐）
```

- `font` payload = 完整 `font.fontpack`（自带 EFNT 头）。  
- `res` payload = 完整 `resources` 分区镜像。  
- 仅字库：`flags=0x1`，`res_*=0`。  
- 仅资源：`flags=0x2`，`font_*=0`。  
- 两者都有：`flags=0x3`。

### 6.2 合并包优点

- 用户只拷 **一个文件**。  
- 原子性更好：同一版本字库+资源一起发，减少「新公式字 + 旧关机图」错配。  
- 仍允许只带一侧（flags），售后可只升字库。

### 6.3 打包工具（PC，建议放 `tools/syspkg/`）

```bash
./pack_metalio_sys.py \
  --font use_font/font.fontpack \
  --resources build/.../resources.bin \
  --min-app 2.0.55 \
  -o metalio_sys.pkg
```

设备侧：`unpack` 只解析头 + 按 offset 流式写分区，**不必把 5MB 整包读进 RAM**（边读 SD 边写 Flash）。

---

## 7. `resources` 与固件 checksum 策略（必选其一）

### 策略 A：版本绑定（实现简单）

- 包内 `min_app_version` / 资源 `build_id` 与固件一致才允许写。  
- 改 `xingzhi-assets` 必须发 **App OTA + resources 包**（或整包）。  
- 启动仍可用编译期 `MMAP_RESOURCES_CHECKSUM`。

### 策略 B：放宽启动校验（用户体验更好）

- `mmap_assets_new`：**禁止**对失败 `ESP_ERROR_CHECK` 直接 abort。  
- 优先用分区内自描述；checksum 不匹配则日志 + 使用出厂备份（若有）或空资源降级。  
- SD 升级可换任意合法镜像（文件数变化时需固件已支持动态 `max_files`，或约定文件表兼容）。

**建议**：一期用 **A + 合并包**；二期再做 B。

---

## 8. 设备端流程（统一）

```
进入「设置 → 存储 / 系统 → 从 SD 升级」
  或开机检测到 metalio_sys.pkg / 分文件且 NVS 未标记已处理
        │
        ├─ 无卡 / 无文件 → 提示并退出
        ├─ 读头校验 magic/crc/size/min_app
        │     失败 → 提示「包损坏或版本过旧」
        ├─ 电量过低 → 禁止写入
        ├─ UI：进度条「正在升级字库…」「正在升级资源…」
        ├─ 写 font_data（若有）
        ├─ 写 resources（若有）
        ├─ 任一步失败 → 停止后续、保留 SD 包、提示重试
        ├─ 成功 → NVS 记版本，可选 rename *.pkg → *.pkg.done
        └─ 重启
```

写入任务跑在 **非 LVGL 线程**，进度用 `lv_async_call` 回写（遵守 LVGL 线程安全 skill）。

断电：扇区擦写中途掉电可能导致该分区损坏 → 文案写明「请保持供电」；字库损坏时公式/部分 UI 缺字但不一定无法开机；resources 损坏若仍 ERROR_CHECK 则可能起不来 → 更要做策略 B 或出厂整包救援。

---

## 9. 与现有通道对照

| 通道 | 更新对象 | 能否替代本方案 |
|------|----------|----------------|
| App OTA | 应用固件 | 否（不写这两分区） |
| 传输「字体」 | SD `.ef` 阅读字 | 否 |
| `merge_firmware` / `idf.py flash` | 出厂/售后整包 | 是，产线主路径 |
| `flash_fontpack.sh` | 仅 `font_data` USB | 是，研发/售后 |
| **本方案 SD 包** | `font_data` ± `resources` | 用户自助 |

---

## 10. 实现分期建议

### P0（可交付用户升字库）

1. SD 路径 + `font.fontpack` 校验写入 `font_data`。  
2. 设置页入口 + 进度 + 重启。  
3. 文档：如何从仓库拷贝 `use_font/font.fontpack` 到 SD。

### P1（合并包 + 资源）

1. `MZPK` 打包/解析工具与设备端流式写入。  
2. `resources.bin` 写入；策略 A 版本绑定。  
3. 启动侧：resources 失败降级（至少不 abort），为售后留活路。

### P2

1. 开机自动检测未处理包。  
2. 云端下发 `metalio_sys.pkg` URL（可选，仍落 SD 再写分区）。  
3. 双银行式「pending / confirm」降低变砖概率（可选，复杂度高）。

---

## 11. 工作量粗估

| 模块 | 内容 |
|------|------|
| `partition_updater` | 通用于任意 label：校验回调 + erase/write + 进度 |
| font 适配 | EFNT 头校验 |
| resources 适配 | 大小 + 可选 checksum；重启重载 |
| MZPK | PC pack 脚本 + 设备解析 |
| UI | 设置页 1 个入口、进度、文案 |
| 文档 | 用户说明 + 本方案 |

---

## 12. 直接回答「能否合并成一个文件」

**能。** 推荐自定义 **`metalio_sys.pkg`（MZPK）** 单文件，内嵌 fontpack 与/或 resources 镜像；设备流式拆写到 `font_data` / `resources`。  

也可用 zip，但 ESP 上要解压库与临时空间，不如 MZPK 干净。

---

## 13. 用户侧一句话说明（可写进说明书）

> 将官方提供的 `metalio_sys.pkg`（或 `font.fontpack`）复制到 SD 卡目录 `metalio/e-ink/system/`，在设备「设置」中选择「从 SD 升级系统资源」，保持供电直至完成并自动重启。

产线仍以整包 `merge_firmware` 为准；本方案服务 **已出货设备补字库/补内置图**。
