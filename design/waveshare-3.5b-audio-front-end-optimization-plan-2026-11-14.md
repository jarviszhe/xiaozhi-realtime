# Waveshare 3.5B 麦克风噪声调优记录

日期：2026-11-14。目标是在当前实体板上降低类似电流声的底噪，保留轻声、尾音和全双工回声消除，并提供实际设备处理输出用于试听。

## 寄存器核对与更正

根据 `docs/datasheet/ES8311.DS.pdf` 第 20–23 页，以及 `ES8311.user.Guide.pdf` 第 19–23 页：

- `0x14[3:0] PGAGAIN` 是模拟 PGA，0–30 dB，3 dB 一档；`0x14[4] LINSEL` 选择 MIC1 差分输入。`0x1A` 表示 MIC1 输入和 30 dB 模拟增益。
- `0x16[2:0] ADC_SCALE` 是 ADC 数字增益，0–42 dB，6 dB 一档。当前官方 `esp_codec_dev_set_in_gain()` 实际修改这里，不修改模拟 PGA。
- 此前把 0x14 解释成掉电控制、把官方接口的 24 dB 解释成模拟 PGA，是错误的。写 0x14=0x14 本身表示 MIC1 与 12 dB PGA，不能据此断定 ADC 掉电。
- 官方驱动启动写入 0x14=0x1A；当前板级参数 24 dB 对应 ADC 数字增益，模拟增益仍为 30 dB。其他 ES8311 板子的默认数字增益保持 30 dB。
- `0x1C[5]` 控制动态 HPF，现有 0x6A 已包含此位。去直流不能等同于滤除全部 50/60 Hz 或高频噪声。
- ALC、自动静音和 AGC 本轮保持关闭。噪声门只能衰减低电平段，不能清除说话期间的噪声，也不能保证绝对静音。

## 数据轨的区别

```text
ES8311 ADC / 内部 DAC 参考
 -> Read() 返回的 24 kHz、16 bit、双声道 PCM（左麦克风，右数字参考）
 -> 24 kHz 转 16 kHz
 -> aec_mic / aec_ref（算法输入）
 -> ESP-SR FD AEC + NS
 -> aec_out（算法输出）
 -> 上行 PCM / Opus / 服务器
```

`Read()` 原始轨不包含软件降噪；评价算法应听 `aec_out` 或最终上行。`aec_mic` 已经经过重采样，不应叫作 ES8311 最原始输出。

## 实验方法

使用连接 COM3 的同一块板，保持摆位、电脑播放音量和语音素材一致。通过 `/api/test/pcm` 诊断路径录制静音、电脑近端语音、设备远端播放及双讲，分别保存麦克风、参考和算法输出。诊断过程不依赖云端 VAD 切段。

录音必须检查 trace gaps、截断、采样连续性、削波、AFE feed 耗时、fetch 错误及样本积压。出现丢块的录音不作为音质证据。同时采集全部原始与 AEC 轨时曾发生 trace 队列溢出；后续受控实验暂时关闭额外 24 kHz 原始轨，以完整的 aec_mic/aec_out 对比算法。

音量比较先按同一语音电平对齐；不能通过整体调小音量宣称降噪。交付 WAV 保留静音段，并区分原始电平与试听统一增益版本，不在电脑端偷偷再降噪。

## 已取得的基线

`tmp-audio-tests/retune-results/baseline24-short-ddefault-aec0-r1/`：

- 模拟 PGA 30 dB，ADC 数字增益 24 dB，AFE WebRTC NS，AEC 关闭。
- 固定语音，完整采集，gaps=0，削波=0。
- 根据同一原始轨选取安静帧，麦克风 RMS 34.87，NS 输出 RMS 18.64，静音噪声下降约 5.44 dB；同一语音帧电平变化约 -0.02 dB。
- 此样本约 60% 的静音噪声功率在 4–8 kHz；尚未证明存在占主导的单一 50/60 Hz 干扰，不直接套用工频陷波。
- AFE feed 平均约 4.83 ms，32 ms 帧周期，无 overrun、fetch error、样本积压。
- 用户反馈此前交付仍有电流声，因此不能把 WebRTC NS 已启用或 RMS 降低视为音质验收通过。此前试听归一化还额外放大了残余噪声。

## 本轮实现与后续验证

1. 在 Waveshare 3.5B 标准变体选择 NSNet2，使用 ESP-SR 自带神经网络降噪替换 WebRTC NS；保留现有 FD AEC、单个 AFE 实例及关闭 AGC。
2. 补齐资源构建脚本对 NSNet 权重的打包。默认、custom 与 Emote 资源都必须包含所选模型，包括关闭 WakeNet 的配置；资源分区大小检查继续生效。
3. AFE 用 `ns_init=true`、`AFE_NS_MODE_NET` 和实际模型名初始化；启动日志确认实际流水线。模型缺失应明确初始化失败，不能静默声称神经降噪已生效。
4. 用同一近端素材比较 WebRTC 与 NSNet2，验证稳定静音和讲话期间的残留，检查语音电平、延迟及资源占用。
5. 再测试远端独播、双讲和云端交互。保留未通过或未覆盖项目，不能仅凭编译成功宣称硬件验收通过。
6. 如果神经降噪仍有明显残留，再根据同语音电平的实录比较模拟 PGA/数字增益组合、供电及显示/Wi-Fi 干扰；不能只降低数字增益。

本轮构建、烧录和实测结果将在取得完整录音后补充。原始采集轨始终保留其物理输入含义；软件降噪改善的是处理后的语音。

## 最终实测结果（2026-11-14）

本轮固件已通过 ESP-IDF 6.1 的 Waveshare 3.5B 标准变体构建，并在 COM3 烧录。日志确认 `Hash of data verified` 和 `Done`，设备启动流水线包含 `AEC(FD_LOW_COST, NLP_AGGRESSIVE) -> NS(nsnet2) -> VAD -> WakeNet(wn9_hiesp)`。

为保持实时处理，将双核 ESP32-S3 上使用 NSNet2 的输入任务移至 core 1，避开 Wi-Fi 的 core 0。为避免诊断传输短暂阻塞造成录音缺块，将有 PSRAM 时的 trace 队列上限由 512 KiB 调整为 1 MiB；仍为有界队列，音频任务不等待网络。

### 完整诊断录音

文件均在 `tmp-audio-tests/retune-results/`，各目录保留 `result.json`、设备统计、原始算法输入、参考及输出 WAV。

| 测试 | 结果 | feed 平均 / 启动以来最大 |
| --- | --- | --- |
| `nsnet24-core1-near` 两次 | gaps=0、overruns=0、fetch errors=0，无样本积压 | 18.82 / 29.28 ms；18.08 / 29.28 ms |
| `nsnet-final-far`，播放峰值 0.18 | 完整、无削波、无积压，整轨衰减 14.80 dB | 20.91 / 26.54 ms |
| `nsnet-final-double`，播放峰值 0.18 + 电脑近端语音 | 完整、无削波、无积压，近端语音在输出中保留 | 20.71 / 28.04 ms |
| `nsnet-final-near`，最终交付来源 | 完整、无削波、无积压 | 18.41 / 28.04 ms |

最终近端实录：麦克风安静帧 RMS 34.33，NSNet2 输出 5.36；语音 RMS 862.21 -> 725.87，约 -1.50 dB。相较同样开启 AEC 的 WebRTC 基线，将两份输出语音 RMS 统一到 3000 后，安静段 RMS 74.90 -> 22.15，改善约 **10.58 dB**。其他重复样本的改善约 11–14 dB，不能只挑最好的一次宣称固定效果。最终样本 4–8 kHz 噪声在同语音电平下下降约 16.6 dB。

双讲实录中，近端语音所在的 4–8 秒保持明显输出，设备独播段则受到抑制。整轨能量比不能用来给双讲语音质量打分。远端较强播放在首次适应阶段仍有残留，不能宣称 AEC 已完全消除回声。

一次早期 core 0 试验出现 trace 丢块和少数 feed 超过帧周期；一次 core 1、旧 512 KiB trace 队列的强播放试验仍有丢块。以上失败文件保留，但不作为最终音质证据。最终版本三组诊断全部完整且未出现 overrun。

### 云端语音检查

直连云端与恢复用户原代理配置后的 Auto 实机往返均完成，gaps=0。记录分别为 `tmp-audio-tests/cloud-smoke-direct-result.json`、`tmp-audio-tests/cloud-smoke-proxy60-result.json`。服务器转写为“你好，请回答八加五等于多少。”，回复为“八加五等于十三。”或“8加5等于13。”。实际经历 connecting -> listening -> speaking -> listening，并正常结束。此项说明该素材可被识别，不代表所有轻声、距离和环境都通过。

### 正常回复音量调整

用户确认正常回复也偏小后，将扬声器音量从 60 调到 80，保持麦克风 ADC 数字增益 24 dB 和现有降噪配置。之前诊断素材峰值为满幅 0.18，不能用其听感代替正常云端回复的音量判断。

在调试开关保护的现有 `/api/test/chat` 中补齐 `action=volume-1..100`。参数仅接受十进制整数并检查范围，设备空闲时通过 `Application::Schedule()` 调用 Board 音频接口及原有持久化音量 setter。返回 202 仅表示已排队，实际音量通过 stats 确认。本次固件重新构建、烧录通过；实机拒绝了 0、101、负值、带正号、非数字及空值这六类输入。`tmp-audio-tests/volume80-control-result.json` 记录 60 -> 80，输入增益仍为 24；重启后再次确认音量为 80。

`tmp-audio-tests/cloud-smoke-volume80-result.json` 记录音量 80 时正常 Auto 问答成功，设备与代理两条记录均 gaps=0、无截断或中断，回复“8加5等于13。”。该检查确认实际走过回复播放状态，不能据此推断物理声压或主观失真。

音量 80 的追加远端 AEC 录音出现诊断 TCP 断流：`nsnet-volume80-far` 未取得完整播放轨；临时关闭原始 PCM 并直连后的 `nsnet-volume80-clean-far` 仍为 interrupted=true。后者 AFE feed overruns=0、fetch errors=0、无样本积压，但录音不完整，不能作为音质或回声验收证据，后续双讲未执行。以上失败记录保留；前文音量 60 的完整 AEC 结果不能直接外推到 80。尚未定位此次诊断连接断流原因。

测试结束已恢复用户原代理 URL、debug 开关和原始 PCM 采集选项，音量保留 80，设备空闲。最终状态在 `tmp-audio-tests/volume80-final-state.json`。交付近端降噪样本仍来自前述完整录音；此次没有提高采集增益或对交付文件额外降噪。

### 交付文件与边界

- `tmp-audio-tests/deliverables/after-nsnet2.wav`：最终设备处理输出，统一试听语音音量。
- `tmp-audio-tests/deliverables/before-webrtc.wav`：上一版 WebRTC 输出，使用相同语音 RMS 作比较。
- `tmp-audio-tests/deliverables/after-nsnet2-native.wav`：设备输出的原始电平 WAV。
- `tmp-audio-tests/deliverables/comparison.json`：来源文件与固定增益。试听版本只做固定增益，不做电脑端降噪、静音裁剪或噪声门。

原始 ES8311 `Read()` PCM 仍包含模拟输入噪声；本轮改善的是设备软件处理后的语音。客观噪声下降与转写成功不能代替主观试听，也不能承诺“没有任何电流声”。尚未覆盖长时间压力测试、轻声和远距离语音、完整唤醒准确率、Realtime 云端打断及重连压力、其他芯片与网络路径。

2026-11-14，用户在实机试听后反馈“现在声音处理的非常好，aec也处理的特别干净”。此项记录为当前设备和使用环境下的主观验收反馈；前述诊断断流与尚未覆盖的测试范围仍保留。

资源打包专项测试 15 项通过，修改的 C++ 文件通过仓库 clang-format 检查，`git diff --check` 通过。完整主机测试在 UTF-8 环境运行 168 项，其中 6 项错误、33 项跳过：5 项为现有测试临时目录在 Windows 当前目录占用下清理失败，另 1 项为缺少主机 C++ 编译器；未将其记作全套通过。日志在 `tmp-audio-tests/retune-host-tests-utf8.log`。录音及本机测试工具不提交。
