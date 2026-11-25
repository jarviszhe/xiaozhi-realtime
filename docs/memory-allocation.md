# 内存分配与 PSRAM 使用原则

对已经确认 PSRAM 初始化成功的板子，新增或调整内存分配时，优先把允许使用外部内存的数据放到 PSRAM，为 Wi-Fi、蓝牙和其他必须使用内部 SRAM 的资源留出空间。先核对目标芯片、板型、实际生成配置与驱动要求；不能把 S3 的能力推广到全部经典 ESP32 或无 PSRAM 板型。

## 分配位置

本次双板系统的经典 ESP32 也适用该原则。2026-11-25 实机探测到经典板 8 MB PSRAM，启动测试通过、4 MB 映射进 heap。`CONFIG_SPIRAM` 未开启不代表硬件没有 PSRAM；更换板子/配置后重新核对。经典端具体规则见其仓库 `D:\work\Bluetooth-gateway\docs\memory-allocation.md`。

| 对象 | 默认处理方式 |
| --- | --- |
| 大块 PCM/Opus 缓存、抖动缓存、网络暂存、图片等普通数据 | 调用方和驱动允许时显式分配到 PSRAM；队列有上限，音频热路径复用缓冲区 |
| 第三方组件的工作内存 | 优先使用组件提供的 PSRAM Kconfig/分配接口，核对实际构建配置与启动日志 |
| DMA 描述符 | 保留内部、满足驱动要求的内存 |
| DMA 数据缓冲区 | 核对具体芯片、驱动、对齐与能力要求；只有明确支持外部 DMA 的路径才使用 PSRAM，否则使用内部 DMA 内存 |
| 中断、缓存关闭期间仍需访问的数据 | 按驱动和 ESP-IDF 约束保留内部内存，不能仅为节约 SRAM 迁移 |
| 任务栈、TCB、锁和其他系统对象 | 使用相应 API 的能力约束；任务栈需单独审核调用链，不能全局强制外置 |

普通缓冲区可以使用 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 或对应 `heap_caps_calloc`，并检查返回值、长度溢出和对象生命周期。释放遵循分配接口约定。普通 `malloc`/`new` 不能证明数据已经进入 PSRAM，其位置受构建策略和分配大小影响。

PSRAM 分配失败时，非必要的大块功能应明确失败、降级或减少容量；不要悄悄回退到内部 SRAM，耗尽蓝牙、Wi-Fi 的空间。确实需要回退时，先定义该功能的内部内存预算和最大块需求，再选择 `heap_caps_malloc_prefer` 等接口，并记录实际回退；无 PSRAM 目标同样需要明确的有界实现或 Kconfig 限制。

任务栈默认仍可能使用内部内存；仅设置 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` 不会把所有任务栈迁到 PSRAM。确需外置时，核对组件支持、缓存关闭时的调用链及当前 SDK 的能力分配 API；使用 `xTaskCreateWithCaps` / `xTaskCreatePinnedToCoreWithCaps` 创建的任务，删除时需遵循 `vTaskDeleteWithCaps` 的要求，TCB 仍有内部内存需求。不要为了省 SRAM 绕过相关限制，也不要不加测量地缩减栈大小。

## 诊断与验收

新增大块缓冲区或组件任务前，确定容量、并发数量、分配时机及内部内存预算；能预分配复用的音频缓冲区避免逐帧申请。分配失败至少记录：

- 对象、申请字节数和 capability 标志。
- `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 的 free、minimum 和 largest free block。
- `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` 的 free 和 largest free block。
- 对 DMA 或其他特殊能力，另记录同一能力集合下的空闲量与最大连续块。

分别使用 `heap_caps_get_free_size`、`heap_caps_get_minimum_free_size`、`heap_caps_get_largest_free_block` 查询。需要连续块的申请必须核对最大连续块；历史最低值是诊断线索，不能单凭某次总空闲量或 PSRAM 大小认定内部内存足够。

组件初始化失败要有有界重试/退避、清理和明确错误状态，避免紧循环分配与重复注册；检查失败路径和后续恢复，而不只检查一次成功启动。

配置改动应通过板级 `config.json`、构建入口或受维护的配置源生效，不直接修改生成的 `sdkconfig`、`build/` 或管理组件。记录实际配置文件路径和固件哈希，核对启动日志，避免构建缓存造成“源码配置已改、实际固件未变”。

实机验证保留正常唤醒、音频、Wi-Fi 和蓝牙功能，观察重复启动、BLE 配网/退出、重连及适用的通话负载；比较操作前后内部空闲、最大块、最低值和任务栈高水位。一次编译成功、短时 idle 或媒体包计数不能证明内存稳定或双向语音通过。按本次变更明确报告实际覆盖范围。

## 本项目已有案例

2026-11-25，S3 总空闲约 5 MB，但 mDNS 申请 4096 字节内部任务栈失败；重试又导致重复注册刷屏。Waveshare ESP32-S3 Touch LCD 3.5B 使用组件支持的 `CONFIG_MDNS_TASK_CREATE_FROM_SPIRAM=y` 和 `CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y` 后，通过重复软件复位与保留绑定恢复配网回归。它说明应按内存能力和连续块诊断，不代表可以把所有任务栈外置，也不代表整体内存余量已充分。

证据及限制见 [designv1 第 17.10 节](../designv1/esp32-hfp-s3-realtime-design.md#1710-2026-11-25-mdns-启动内存不足与重复初始化)。通用规则以实际使用的 ESP-IDF 版本为准；当前 SDK 对照位置为 `docs/en/api-guides/external-ram.rst`、`components/heap/include/esp_heap_caps.h`、`components/freertos/esp_additions/include/freertos/idf_additions.h`。
