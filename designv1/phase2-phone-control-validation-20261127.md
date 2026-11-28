# 阶段二：电话状态、控制与 Android 验证

日期：2026-11-27。状态：阶段二接口已实现并烧录，两端构建通过，网关 56 项主机回归通过，最终 Android 本地通话 11 项检查通过；S3 全量主机测试存在下列环境错误，未记通过。范围：单通电话的 HFP 状态和控制。蓝牙坏帧按用户要求仅记录，不阻塞本阶段。

## 实现

- S3 与经典端复用绑定认证 TCP 39002；每个控制包仍为 128 字节。协商电话扩展后心跳为 500 ms；PONG 后增加经过 HMAC、双端 nonce 和序号检查的 type 7 电话快照。旧固件继续原媒体桥，不能接收没有新鲜电话状态的 S3 电话命令。
- 快照提供通话 ID、状态版本、空闲／来电／呼出／接通／结束阶段、HFP 指示、来电号码或未知、号码截断标记、自动接听策略与倒计时、实际接通／结束时间。
- 通话计时由经典端真实 HFP 接通／结束事件计算，S3 根据快照年龄外推；SCO 建立不会开始计时，SCO 关闭不会暂停计时，结束后冻结。时间戳是经典端开机单调时间，不是日期。
- 接听、拒接／挂断、取消倒计时绑定通话 ID。一个待确认请求、5 秒超时、非零递增序号、同一连接内重复／旧序号不执行。断线清除待确认请求，重连不重放。
- ACK 只表示本地 HFP API 接受请求或返回错误，真实接通／结束仍由电话指示确认；AT 结果为独立诊断。
- 默认 3 秒自动接听；重复来电指示不会延后期限。关闭策略或取消倒计时后不重新定时；重新打开只影响后续来电。策略为本次运行设置，重启恢复默认启用。
- `audio off` 抑制网关当前通话的自动 SCO 重试，`audio on` 恢复；手机自身仍可控制音频路由。

协议完整定义：`D:\work\Bluetooth-gateway\docs\protocol\gateway-management-v1.md`。

## 串口调试

两端均可使用：

```text
phone help
phone status
phone answer
phone reject
phone hangup
phone auto off
phone auto on
phone auto cancel
phone audio off
phone audio on
gateway status
```

S3 经认证通道发命令；经典端串口直接调用同一电话策略。经典端旧的 `answer`、`hangup`、`audio on|off`、`status`、`packets` 保留。

`PHONE` JSON：`fresh`／`age_ms`（S3）、`call_id`、`phase`、`call_setup`、`call_active`、`caller`、`caller_truncated`、`flags`、`remaining_ms`、`started_ms`、`ended_ms`、`duration_ms`、`at_result`。S3 另含当前／上一命令、请求序号和结果。

结果：0 接受，1 远端／API 错误，2 控制断开，3 超时，255 待确认。忙碌、离线、状态过期、旧端不支持、无目标通话在入队前拒绝。`fresh=false` 不应在后续界面显示为正在通话。

状态日志为 `PHONE_EVENT`，请求和终结为 `PHONE_COMMAND`／`PHONE_COMMAND_END`。经典本地请求记录 `PHONE_LOCAL`；与原有 HFP CALL、SETUP、AUDIO 日志一起定位“控制已接受但通话／媒体未建立”。

## 实机与固件

| 项目 | 本轮条件 |
| --- | --- |
| 经典端 | ESP32-D0WD-V3，COM5，`D:\work\Bluetooth-gateway` |
| S3 | Waveshare ESP32-S3 Touch LCD 3.5B，COM3，`D:\work\xiaozhi-realtime` |
| Android | Huawei ATU-AL10，API 26，ADB `TPE9X18C17W07990` |
| HFP 对端 | 网关 `58:2A:BD:D5:C5:EE` |
| 无线网络 | 本轮网关 `192.168.5.3`，S3 `192.168.5.7`；两板均 Wi-Fi 开启，媒体 UDP |
| S3 音量 | 保持原 60% 设置 |
| SDK | ESP-IDF 6.1，提交 `fff9895c82d744c7237be8847347bdd1b07c6643` |
| 经典 bin SHA256 | `902d708e7216e201b966ea0ef14b8e6b5bf662aada67e6a22cc6ff016abf8dd8` |
| S3 bin SHA256 | `4d1f4d6a50a966965685bd3c57394198c1167eea227afefc0a0cd574d42ac4c6` |

两端最终构建日志为各自 `artifacts/phase2-control-build-{gateway,s3}-06.log`，烧录日志为各自 `artifacts/phase2-control-flash-{gateway,s3}-02.log`；均已完成并报告 `Hash of data verified`／`Done`。未擦除 NVS、Wi-Fi、双板绑定或手机配对。

完整 bin 大小、仓库基线和各修改源文件 SHA256：网关 `artifacts/phase2-live-02/firmware.json`。构建目录名称 `build-uart-provision` 是历史名称，有效配置关闭 UART，实际使用无线 UDP。

## 主机回归

- 网关最终回归 56 项通过：`artifacts/phase2-host-tests-03.log`。使用 Zig 编译并执行实际 C 策略，覆盖生命周期、计时身份、重复来电、取消／关闭自动接听、号码过滤／截断、断开、请求 ACK／超时／回绕／重放和 44 字节快照布局；同时检查两库 `phone_control.h` 字节一致。
- 曾误用缺少 numpy 的 IDF Python 执行整套网关工具测试，记录为失败 `phase2-host-tests-02.log`；换回本机具备依赖的 Python 后上述 56 项通过，保留前次失败。
- S3 全量测试已执行两次；最近 `artifacts/phase2-s3-host-tests-02.log` 为 149 项，10 个 ERROR、24 项跳过。错误涉及 5 个 Windows 临时目录删除时文件占用、5 个临时 Git 仓库初始化；跳过包含缺少宿主编译／cJSON／mbedTLS 依赖的项目。不能写成 S3 全量主机测试通过。本轮两板实际构建和 Android 实机验证另行登记。

## Android 测试方式与记录

源码及安装说明：网关 `tools/android-call/README.md`。包名 `dev.gateway.calltest`，测试账户 `local-test`；系统默认拨号器不变。来电显示使用本地测试号码 `5550101` 或未知。

本地呼出使用固定 `sip:gateway-test@example.invalid` 和显式测试账户，由本应用的 ConnectionService 创建拨号／接通状态。服务校验账户、URI scheme 和解码后的固定地址字段；不拨打 SIM 电话，也不连接 SIP 服务器。

最终 APK 位于 `artifacts/android-tools/apk-call-phase2-05/gateway-call-test.apk`，安装后字节哈希已核对：`b45fabba759af9625f738125381abb3f18acb9252025858a2c53d00d60fc5db4`。

```powershell
python tools/phone_control_bench.py --adb artifacts/android-tools/platform-tools/adb.exe --phone TPE9X18C17W07990 --address 58:2A:BD:D5:C5:EE --s3 COM3 --gateway COM5 --out artifacts/<全新测试目录>
```

每轮保存 `s3.log`、`gateway.log`、`android.log`、带主机时间戳的 `timeline.jsonl`、逐场景 `summary.json`。工具先检查两板 ready／状态新鲜和 Android HFP SLC；结束测试应用通话并恢复原自动接听策略，关闭本轮串口及 logcat。任一场景、清理或手机端自动接听计时失败均返回非零。

历史结果保留：

- `phase2-android-control-01`：初版六组控制场景通过；重连诊断发现“空命令携带保留序号”被错误执行为不支持，已补 `command != 0` 判断。
- `phase2-android-control-02`：前六组通过，包括计时／SCO 开关；自定义 URI 被手机 Telecom 在调用测试服务前取消，模拟呼出失败。
- `phase2-android-control-03`：改 SIP 后进入 Telecom 呼出流程，但测试服务直接比较 URI 字符串而拒绝系统编码后的地址；模拟呼出失败。已改为解码字段比较，并等待服务回调后才发模拟接通。
- 最终扩展回归：`phase2-android-control-04`，退出码 0，以下 11 项全部 PASS。结论来自 HFP 状态、双板状态、命令结果和 Android 事件联合检查，未复用前轮 PASS。

| 场景 | 最终结果与证据 |
| --- | --- |
| 关闭自动接听后手动接听／挂断 | 来电保持超过 3 秒，S3 命令接通并结束，同一通话 ID |
| 取消当前倒计时后手动接听、手机结束 | 超过原期限仍为来电，随后接通和结束 |
| 3 秒自动接听、未知号码、SCO off/on | 新来电不沿用旧号码；SCO 关闭超过 3.5 秒且通话仍接通，再恢复 16 kHz mSBC |
| 拒接 | 来电转结束，手机侧呼叫状态清除 |
| 来电方提前取消 | 倒计时撤销，超过期限不再接听 |
| 手机侧接听及非法倒计时取消 | 保持通话 ID，活动通话取消倒计时返回预期错误 1 |
| 本地呼出／接通／S3 挂断 | Android `outgoing_request` 两项校验 true；`outgoing_local`、`outgoing_active` 回调与 S3 呼出／接通／结束一致 |
| 空闲时接听命令 | 本地拒绝，不增加请求序号 |
| 来电期间 HFP 断开／重连 | 倒计时撤销，重连后无活动通话和遗留倒计时 |
| 待确认控制请求期间重连 | 请求 44 以 disconnected=2 结束，重连后未重放，网关日志无 `command=0` 执行 |
| 手机单调时钟自动接听计时 | `incoming known=false` 到 `answer previous=RINGING` 为 **3132 ms** |

本轮 SCO 关闭前后 `duration_ms` 从 1436 增长至 5265；通话结束和 1.2 秒后再次读取均为 9025，验证计时独立于媒体且结束冻结。最终两板 `ready=true`、`remote_fresh=true`，网关 HFP SLC=3，SCO=0、呼叫指示为 0、倒计时为 0、无待确认请求，自动接听启用。测试进程已退出，串口与本轮 logcat 已关闭。

正常音频／蓝牙功能保持启用，未新增大缓冲或任务。本轮结束 S3 内部空闲 14619 字节、最大块 7680 字节、PSRAM 空闲 5903620 字节；运行日志未见断言／崩溃或分配失败。该空闲快照不等同于全部运行路径的内存验收。5 秒请求超时、序号重放／回绕等由实际 C 主机策略测试覆盖，未通过手机或网络故障注入逐项实测；旧固件兼容分支未另烧旧固件互测。

## 验收边界

本报告只验收已列的 Android 本地 Telecom → 实际蓝牙 HFP → 经典 ESP32 → 认证 TCP → S3 电话状态与反向控制。运营商来电、微信等第三方 VoIP、多通保持／等待／合并通话未在本阶段放行；完整双向语音、下行扬声器声学和过载跳音听感沿用阶段一未通过状态。`full_duplex_speech_passed`、`carrier_call_lifecycle_passed` 保持 null。

阶段三的屏幕、AI 就绪策略、AI 电话路由、录音与通话记录尚未实现；当前串口接口用于为后续界面提供同一控制入口。
