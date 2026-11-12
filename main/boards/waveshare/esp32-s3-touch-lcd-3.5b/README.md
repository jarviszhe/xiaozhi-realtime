新增 微雪 开发板: ESP32-S3-Touch-LCD-3.5B
产品链接：
https://www.waveshare.net/shop/ESP32-S3-Touch-LCD-3.5B.htm

## 对话模式与 BOOT 按键

- BOOT（GPIO 0）双击依次切换 Manual → Auto → Realtime → Manual。
- 待机屏幕显示当前模式；对话时显示模式与连接/聆听/回复状态。
- 对话中双击会结束当前会话，切换后回到待机。启动、配网、升级等特殊状态不切换模式。
- Manual 下单击开始录音，再次单击结束并提交；Auto 下自动轮流对话；Realtime 下播放回复期间持续上传，支持插话。
- 当前阿里云默认 Auto；模式选择在本次运行内有效，重启后恢复默认。

Realtime 仅在音频引擎具备双工和有效 AEC 参考时提供。本板使用软件播放参考；Manual/Auto 关闭会话 AEC，Realtime 在播放及短暂尾音期间启用，无播放时旁路。该资源策略的效果以实机验证为准。

当前 Realtime 仍需继续验证和优化：长回复实测中出现过设备残余回声触发新话轮、近端插话漏识别及语音开始事件延迟。完整插话体验尚未通过稳定性验收；实体 BOOT 单击/双击识别与屏幕显示也需补充实机检查。

模式控制属于应用公共层，其他板子可将自己的双击事件绑定到 `Application::CycleListeningMode()`。现有启动阶段的 BOOT 配网行为保留。
