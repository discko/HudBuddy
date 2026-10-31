# HudBuddy 开发约定

- 实机为 Waveshare ESP32-S3-Touch-AMOLED-2.16，480×480，16 MB Flash、8 MB PSRAM。屏幕时钟 GPIO38、复位 GPIO39，自定义 USER 按钮 GPIO18（低电平按下）。不要使用 2.06 或 1.8 的板级配置。
- 正式 HUD 固件采用两段空闲计时：任务界面 60 秒无操作进入小狗屏保，再过 60 秒无操作关闭屏幕。屏保中触摸／USER 返回原页面，返回触摸不穿透为任务点击。正式固件息屏后仅 BOOT 短按唤醒，USER 长按不再唤醒。USB 数据、定时刷新、动画均不能续时或自动唤醒。独立小狗待机固件仍为 60 秒直接息屏、USER 长按 3 秒唤醒。
- 共用 `firmware/libraries/HudBuddyPower` 的计时逻辑。息屏关闭 AMOLED 显示，不进入 MCU 深睡眠，保持 USB 通信；唤醒后重新绘制并恢复低亮度。
- 正式 HUD 固件构建／烧录入口是 `scripts/hud.sh`；`scripts/standby.sh` 保留为小狗待机固件入口。不要直接烧录不含息屏逻辑的厂家示例，除非用户明确要求临时测试；修改其他固件时也须保留上述行为。
- 正式界面使用 LVGL 8.4，固定 480×480，不随加速度旋转。状态栏固定，横向切换页面，列表／详情纵向滚动。详情状态位于完整标题与 summary 之间。
- 电脑端 `service/bridge.py` 仅通过 hud-cli API 读取任务，筛选 doing/waiting 并保留 HUD 顺序；不直接查询 SQLite。USB JSONL 协议及限制见 `docs/bridge.md`。
- Bridge 仅在目标 Buddy 在线时每 60 秒查询 HUD，有变化才推送，心跳周期 600 秒。macOS IOKit USB 通知按 VID/PID/序列号识别设备，拔出后停止定时，重新接入立即查询并推送。设备用有效心跳或快照的接收时间更新通信状态灯；不做每 10 秒周期诊断上报。
- 第二屏只读查询本机 Magpie `/v1/magpie/quotas`，与 HUD 同为在线时每 60 秒检查；不得调用重置卡消费接口。snapshot 可独立包含 tasks 或 quota，某源不可用不清空另一源。仅读取时间变化不推送；Magpie 最后成功时间通过心跳补充同步并开发模式持久保存于 `.state/magpie-state.json`，安装模式使用 config.json 的 state_dir（不要放会被编译清理的 build 目录）。
- 额度重置／卡到期不足 48 小时显示秒级倒计时，其余显示日期时间。重置卡按 each 日期排序；72 小时内黄色、24 小时内红色、12 小时内强化提醒。Magpie 超过 24 小时未成功读取，在 Codex 后显示小红点；重置卡警示、倒计时和断连红点均不能续时或唤醒屏幕。
- 厂家原固件完整备份为 `backups/factory-20261031-16MB.bin`，保留且不要提交到版本库。烧录授权不等于允许丢弃备份。

- 详情页（任务、重置卡、设置）禁用外层横向分页；点击返回后才能切屏。第三屏为设置，亮度滑条 5%–50%，默认 25%，释放后保存 NVS，重启／唤醒恢复该低亮度设置。

- 正式固件 BOOT（GPIO0，低电平按下）通过 CHANGE 中断与 30 ms 防抖识别短按，释放时触发；工作页／详情 → 小狗屏保 → 息屏 → 原工作页循环。运行中超过 800 ms 的长按不触发切换。取消 PWR 软件短按逻辑和轮询，不修改 AXP2101 硬件长按关机配置。小狗采用 assets/puppy-v2 的 6 帧黄线无填充素材，经 scripts/build-dog-frames.py 转 RGB565，左右镜像播放。

- macOS 安装使用 scripts/install-bridge.py，用户级 LaunchAgent com.hudbuddy.bridge。程序位于 ~/Library/Application Support/HudBuddy/bridge，配置和 state 独立于程序，日志位于 ~/Library/Logs/HudBuddy。更新保留配置与缓存，卸载默认保留用户数据；不把个人序列号、配置、缓存或日志提交 Git。
