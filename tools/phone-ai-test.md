# Android 本地 Telecom 与阿里直连回归

本工具用于本机 COM3（S3）、COM5（经典网关），两端均为已烧录固件、已配网并已绑定的设备。Android 必须已安装本地 Telecom 应用 `dev.gateway.calltest` 和 HFP 录播应用 `dev.gateway.hfptest`，ADB 已授权，手机与网关 HFP 已连接。它不拨打运营商号码。

两种测试器都会先执行真实 ADB shell 就绪检查，再查询或改变设备状态；`adb devices` 曾枚举到手机不代表 shell 已可用。ADB 不可用时不启用自动接听，不发送来电／挂断，也不拉取旧录音。已有通话或远端状态过期时拒绝启动；清理只结束本测试尝试建立的通话。自动接听的启用与恢复必须得到 `fresh=true` 且 flags 匹配的认证状态，过期快照即使 flags 恰好相同也不算恢复成功。若媒体启动已尝试但 ADB 回执丢失，仍尝试停止本次媒体，避免遗留录播任务。

终端一启动双串口采集；打开串口时保持 DTR/RTS 关闭，不复位设备。使用新的输出目录：

```powershell
python tools/phone_ai_monitor.py --output artifacts/phone-ai-serial-new
```

终端二用本机实际 ADB 路径和 `adb devices` 确认的手机序列号运行：

```powershell
python tools/phone_ai_bench.py --adb D:/work/Bluetooth-gateway/artifacts/android-tools/platform-tools/adb.exe --phone TPE9X18C17W07990 --output artifacts/phone-ai-300-new --log artifacts/phone-ai-serial-new/serial-COM3.jsonl --seconds 300
```

手机录播应用中须已有 `question-1.wav`、`question-2.wav`。S3 调试接口须支持 `ui status`、`ui transcripts`、`ui click hangup`；经典端支持 `status`。固件中普通语音、Wi-Fi、蓝牙及真实云配置保持开启。先核对原始 `websocket_url`，直连测试时应为空，不要把代理结果标成直连。

测试从真实来电／自动接听进入，检查 HFP、双板认证与 AI 聆听，再交替送入已知语音。每轮保存设备页面上的最终转写，结束前等待最后回复排空。Android 系统 `audio`、`media.audio_policy` 和 `telecom` 快照、双向录音、异常和失败报告均保留在该轮目录。需要截图时加 `--screenshot`，并安装 Pillow；截图失败会单列，不能阻止录音回收和挂断。

`result.json` 的 `DIGITAL_PATH_PASS` 表示单一云会话下完成本轮输入和完整回复，且未发现控制失联、worker 异常退出或 panic。失败会返回非零退出码。它不代表 ASR 准确率、声学听感、双讲、运营商或微信兼容性；这些字段独立登记。Android 某些版本的路由 API 返回空时，应检查系统策略中的活动 SCO 输入／输出，不能只根据 `route_failed` 或能听到一段声音做结论。

`--log` 必须指向已经存在的 `serial-COM3.jsonl` 文件，不能只传目录；检查发生在操作手机前。运行失败后保留媒体至少三次、每次间隔四秒的状态观察，保存 Android 音频／蓝牙／Telecom 及 logcat，再按 `cleanup_action` 时间标记清理。S3 的 `fresh=0,hfp=0` 表示远端状态不可用，不能据此认定手机蓝牙已经断开；须与经典端直接 `STATUS`、Android 事件及清理边界对照。

同一监视目录必须同时存在 `serial-COM5.jsonl`。判定会读取该独立日志；通话至清理结束期间经典端出现 panic、重启或 codec 护栏失败，即使 S3 随后恢复也不放行。

ASR 应以同一轮 `reply_text` 中的 AI 最终文本核对手机 `uplink.wav`；来电人输入用本轮固定语料核对页面上的来电人转写。不要引用其他语料的 CER。ASR 成功、结束回到 Ready、包计数增加均不能替代连续会话检查。

采集结束后释放串口：

```powershell
Invoke-RestMethod http://127.0.0.1:7330/close -Method Post -Body '{}' -ContentType application/json
```

录音、日志和账号放在忽略的 artifacts 中，不提交 API key、NVS 备份或私人音频。未验收的出厂／换路由器场景须单独测试；本工具不会清除设备配置。

`phone_ai_recovery_bench.py` 单独验证网络故障恢复：先建立真实 Android 本地电话和 AI 会话，临时发送经典端 `wifi off`，观察媒体停止，在离线时结束电话。保持自动接听策略开启，再发起一次本地来电；直接查询 COM5 的实际电话状态，要求六秒后仍响铃、没有进入 ACTIVE、倒计时已取消，并有匹配 call_id 的 `AUTO_ANSWER suppressed reason=assistant_link_unavailable` 日志。这一步需要支持已绑定助手可用性保护的经典固件，旧固件不能通过。随后结束该来电，`wifi on` 恢复，要求状态新鲜、自动接听恢复，并且同一启动周期的下一通 40 秒问答成功。finally 尝试恢复 Wi-Fi、结束本测试的通话与恢复原自动接听策略；不清 NVS，不复位板子。异常和启动日志仍会使结果失败。该故障注入结果不能与正常 300 秒音频验收混用。

支持故障来源的新固件在 `UI_STATUS` 末尾追加 `ai_error`，不改变已有字段：`cloud_connect`／`cloud_disconnected`、`gateway_control`／`gateway_media_timeout`／`gateway_media_changed`、`audio_queue_overflow`、`audio_memory`、`worker_resources`。非失败状态为 `none`；桥接层保存首次故障，worker 清理路由前取出原因，旧代 worker 不覆盖新通话。恢复测试必须观察到 `ai=6` 及网关连接／媒体超时原因，保存实际帧缓存截图 `network-failure.png`，再验证重新认证后 `ai=1,ai_error=none` 和下一通问答。仅看到停止聆听不算故障提示通过。

```powershell
python tools/phone_ai_recovery_bench.py --adb D:/work/Bluetooth-gateway/artifacts/android-tools/platform-tools/adb.exe --phone TPE9X18C17W07990 --output artifacts/phone-ai-recovery-new --log artifacts/phone-ai-serial-new/serial-COM3.jsonl
```
