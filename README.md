# HudBuddy

把 Codex HUD 任务与 Magpie Codex 额度显示在桌面 AMOLED 小屏上。正式固件基于 LVGL 8.4，电脑端 Bridge 通过 USB 双向同步，当前支持 macOS。

## 硬件

Waveshare **ESP32-S3-Touch-AMOLED-2.16**，480×480、16 MB Flash、8 MB PSRAM。不要混用 2.06 / 1.8 板级配置。厂家源码固定在 `225a62bff11b5d0a0b607873860d39485a9a9685`。

- [厂家仓库](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16)
- [硬件说明](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16)

## 功能与操作

- HUD：进行中／等待中任务，长标题滚动，点击查看完整标题、状态、summary 和更新时间。
- Codex：账号、套餐、额度与重置时间、重置卡明细。5 小时额度缺失时不显示；不消费重置卡。
- 设置：亮度 5%–50%，默认 25%，松手保存；电源说明、设备信息。
- 主屏左右切换，详情只能返回后再切屏；列表和详情上下滚动，顶部状态栏固定。
- BOOT 短按：**工作页 → 小狗屏保 → 息屏 → 原工作页**，GPIO 中断与 30 ms 防抖，释放时触发，超过 800 ms 不视为短按。
- 空闲 60 秒进入屏保，再空闲 60 秒息屏；屏保中触摸／USER 返回，返回触摸不会穿透。正式固件 USER 不唤醒息屏，PWR 保留硬件长按关机。
- 仅关闭显示，不深睡；USB、动画和数据刷新均不会续时或自动亮屏。

Bridge 在线时每 60 秒检查 HUD / Magpie，有变化才推送；心跳 600 秒。USB 拔出停止定时，重连立即同步。Magpie 异常保留缓存，超过 24 小时未成功查询显示红点。卡到期 72/24/12 小时内逐级警示，不足 48 小时显示秒级倒计时。

## 开发环境与构建

验证环境：macOS、Arduino CLI 1.5.1、Arduino-ESP32 3.3.11、Python 3.12、clang。首次准备 Arduino CLI、Python 3 与 Git，然后：

```sh
sh scripts/setup.sh
sh scripts/hud.sh build
sh scripts/arduino.sh board list
sh scripts/hud.sh flash /dev/cu.usbmodem101
```

setup 将依赖放入项目 `.tools/` 与 `vendor/`。正式固件使用 8 MiB 单应用分区，不支持 OTA。烧录前停止 Bridge，并自行备份设备原固件；仓库不包含任何设备备份。独立待机固件使用 `scripts/standby.sh`，保留旧的 60 秒直接息屏、USER 长按 3 秒唤醒规则。

## 开发模式运行 Bridge

需要已安装的 `hud-cli` 及兼容检查 Skill；额度来自本机 Magpie 的 `http://127.0.0.1:3425/v1/magpie/quotas`。服务不直接读取 HUD 数据库。

```sh
.tools/python/bin/python -m serial.tools.list_ports -v
mkdir -p .state
# 将上一步查到的目标设备 USB 序列号写入 .state/device-serial，只有一行。
.tools/python/bin/python -m service.bridge --debug
```

也可用 `--device-serial` 或 `HUDBUDDY_DEVICE_SERIAL` 指定序列号。`.state/` 是本机运行状态，禁止提交。详细协议、数据源容错、连接与轮询逻辑见 [Bridge 文档](docs/bridge.md)。

## 测试

```sh
sh scripts/test.sh
python3 scripts/test-gestures.py
```

前者覆盖 Python 服务、原生电源状态机、按键防抖、点击保护、额度时间策略与小狗轨迹；后者编译同一份 LVGL 并模拟触摸验证分页锁定、滑条与纵向滚动。都不操作设备按键。

实机诊断（先停止 Bridge）：`scripts/verify-device.py`、`scripts/verify-quota-device.py`。脚本会暂时替换设备显示的快照，并恢复真实数据；不修改 HUD 或消费卡。物理按键、显示效果和 USB 拔插仍需人工验证。

## 目录

| 路径 | 用途 |
|---|---|
| `firmware/hud` | LVGL 页面、硬件驱动入口、USB 接收 |
| `firmware/libraries` | 共用电源策略与小狗动画 |
| `service` | HUD / Magpie 数据读取与 macOS USB Bridge |
| `scripts` | 工具安装、构建、素材生成、诊断与测试 |
| `assets` | 字体源文件与小狗序列帧 |
| `tests` | 离线回归测试 |

字体为 Noto Sans CJK 24/28 px、28,447 个码位，OFL 许可见 [字体说明](assets/fonts/README.md)。小狗为 AI 生成的六帧黄线无填充素材，8 FPS；用 `scripts/build-dog-frames.py` 生成 RGB565（含镜像）。字库用 `scripts/build-fonts.py` 生成。生成文件随源码保存，可直接构建。

## 当前限制

Bridge 需要 macOS IOKit；尚未实现 HUD App 内服务管理。缺失字体（Emoji、补充平面汉字）可能显示占位符。Magpie 是本机应用接口，升级后若数据结构改变，需要更新适配。

工具链、厂家源码副本、编译产物、原厂备份、个人配置和运行缓存均被 Git 忽略。第三方字体遵循其单独许可；公开仓库本身不表示给所有素材统一授予再分发许可。
