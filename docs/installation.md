# macOS Bridge 安装与维护

安装为当前用户的 LaunchAgent，不需要 sudo。登录后由 launchd 启动，异常退出自动重启；退出终端或移动开发源码不影响安装版。Mac 注销／关机时停止，电脑睡眠期间不保证同步。

## 首次安装

前提：Python 3.10+（验证版本 3.12）、可运行的 hud-cli 与 HUD compatibility Skill、本机 Magpie。构建固件用的 Arduino 工具链不是运行 Bridge 的依赖。

```sh
# 从项目根目录运行；若开发环境已准备好，用 .tools/python/bin/python 替换 python3。
python3 scripts/install-bridge.py install --device-serial '目标设备的序列号'
```

如果 `.state/device-serial` 已配置，可省略参数。第一次安装读取顺序：命令参数、`HUDBUDDY_DEVICE_SERIAL`、开发目录配置。存在安装版 config.json 时保留它，不能用安装参数覆盖。安装会创建独立 venv 并安装 pyserial，需要能访问 Python 包源。

安装器先验证配置和 hud-cli 兼容性，然后停止 `.state/bridge.pid` 指向且命令身份匹配的开发进程。其他手动启动的 Bridge 需要自行停止，不能两个进程占用同一串口。系统若要求允许后台项目，请到「系统设置 → 通用 → 登录项与扩展」允许 HudBuddy 对应项目。

## 文件位置

| 内容 | 路径 |
|---|---|
| 程序和独立 venv | `~/Library/Application Support/HudBuddy/bridge/releases/<版本目录>/` |
| 当前版本快捷链接 | `~/Library/Application Support/HudBuddy/bridge/current` |
| 配置 | `~/Library/Application Support/HudBuddy/config.json` |
| 持久状态 | `~/Library/Application Support/HudBuddy/state/magpie-state.json` |
| 运行日志 | `~/Library/Logs/HudBuddy/bridge.log` |
| 启动异常日志 | `~/Library/Logs/HudBuddy/launcher.log` |
| LaunchAgent | `~/Library/LaunchAgents/com.hudbuddy.bridge.plist` |

主日志达到 5 MiB 轮转，保留 3 个历史文件。首次迁移配置与 Magpie 缓存，权限 0600；源文件保留但安装版不再读取。配置中的 `state_dir` 是绝对路径。

## 配置

配置在进程启动时读取，修改后重启生效。示例：

```json
{
  "device_serial": "AA:BB:CC:00:11:22",
  "hud_cli": "/Users/yourname/.local/bin/hud-cli",
  "compatibility": "/Users/yourname/.codex/skills/hud/scripts/check_compatibility.py",
  "state_dir": "/Users/yourname/Library/Application Support/HudBuddy/state",
  "poll_interval": 60,
  "heartbeat_interval": 600
}
```

运行参数优先于配置文件；配置文件优先于开发模式默认值（序列号的开发默认值来自环境变量或 `.state/device-serial`）。不使用当前工作目录猜测安装版配置。不需要修改／烧录固件。

## 管理与更新

```sh
python3 scripts/install-bridge.py status
python3 scripts/install-bridge.py stop
python3 scripts/install-bridge.py start
python3 scripts/install-bridge.py restart
python3 scripts/install-bridge.py install    # 更新：新建独立版本目录，保留配置、状态
python3 scripts/install-bridge.py uninstall  # 删除服务和程序，保留配置、状态和日志
```

stop 会卸载并禁用服务，防止 KeepAlive 立即拉起；start 重新启用。无需依赖脚本查询状态：`launchctl print gui/$(id -u)/com.hudbuddy.bridge`。

更新前验证失败不会停止旧服务。新 LaunchAgent 启动失败会尝试恢复旧 plist 和旧服务。旧版本目录保留，便于排查，不自动删除；当前版本以 plist 的绝对程序路径为准。安装版 venv 依赖创建它的系统／Homebrew Python，升级或删除该 Python 后可能需要重新安装。

## 验证边界

状态显示 running 只代表进程已运行。还应检查日志是否出现设备连接和 snapshot 发送；查看设备显示／ACK 才能确认端到端同步。USB 未连接时 Bridge 等待事件，不执行定时数据查询。需要烧录固件时先 stop，烧录完 start。
