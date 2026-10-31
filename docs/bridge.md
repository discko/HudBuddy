# 电脑端 HUD 同步服务

`service/bridge.py` 是独立 Python 进程，通过已安装的 `hud-cli` 只读获取任务，并从本机 Magpie Gateway 只读获取 Codex 额度，通过 USB CDC JSON Lines 同步到 HudBuddy。不依赖 Codex HUD 窗口运行，不访问 SQLite，不读取或发送 Item，也不修改 HUD。

## 运行

首次使用先配置设备序列号：`--device-serial` 参数优先，其次 `HUDBUDDY_DEVICE_SERIAL` 环境变量，再其次项目内 `.state/device-serial` 文件。匹配忽略大小写；没有配置时拒绝连接，不会自动选中其他 ESP32。`--once` 不需要配置设备。用 `python -m serial.tools.list_ports -v` 查看 USB 序列号。

在项目根目录运行（现有 `.tools/python` 虚拟环境已有 pyserial）：

```sh
.tools/python/bin/python -m service.bridge
```

默认通过 macOS IOKit USB 增删事件识别 VID `303a`、PID `1001`、用户配置的 USB 序列号 的设备，串口优先 `/dev/cu.*`。只有目标 Buddy 物理在线时才尝试连接；`--port` 可指定串口节点，仍由同一目标 USB 事件启停，不绕过身份判断。

```sh
.tools/python/bin/python -m service.bridge --port /dev/cu.usbmodem1101 --poll-interval 60 --heartbeat-interval 600 --cli "$HOME/.local/bin/hud-cli"
```

HUD 和 Magpie 默认每 **60 秒**各检查一次，心跳独立每 **600 秒**发送一次。任务、额度和健康状态未变化时不发送 snapshot；snapshot 和 heartbeat 都携带当前时间。首次连接/重连成功时立即查询 HUD/Magpie 并发快照，同时发送一次初始心跳。CLI 暂时失败时保留旧数据，心跳仍按独立周期发送。

`--poll-interval` 和 `--heartbeat-interval` 可分别指定秒数，均须至少 1 秒；`--interval` 保留为 HUD/Magpie 轮询周期的别名。加 `--debug` 会记录两个数据源检查、心跳发送、ACK 和 USB registryID，便于核对时间间隔。串口打开使用 115200、无软硬件流控、独占锁；不调用 pyserial 的 DTR/RTS 控制 ioctl，关闭 HUPCL，避免进程打开/关闭串口主动复位设备。

USB 离线后立即关闭串口、清除 HUD、Magpie、心跳和重连定时，主循环阻塞等待 IOKit 通知；离线期间不执行 `hud-cli request` 或 Magpie HTTP 请求，也不循环调用 `list_ports`。`service/usb_events.py` 通过 ctypes 订阅系统 IOKit 的 first-match/terminated 通知，后台 CFRunLoop 收事件，使用 registryID 缓存识别属性已经消失的移除事件。首次 drain 同时识别服务启动前已接入的 Buddy 并激活通知；HostDevice/legacy 重复报告仅产生一次状态变化。

USB 已出现但串口节点尚未就绪、串口被占用、写失败或 ACK 超时时，先按 0.5、1、2、4 秒重试，再降为每 30 秒重试。仅收到有效 ACK 才重置失败退避；设备物理离线会立即取消重试。连接失败期间也暂停 HUD、Magpie 和心跳定时，成功打开后重新开始两个周期。IOKit 不可用则报错退出，不改为离线端口轮询。

只读诊断一次，无需设备，也不会导入或打开串口：

```sh
.tools/python/bin/python -m service.bridge --once --stdout
```

stdout 仅一行完整 snapshot JSON；日志和错误均走 stderr。省略 `--stdout` 只输出任务数、额度状态和报文大小。HUD 兼容检查失败时不生成快照；单个业务源失败时仍输出另一源及缓存数据的快照，并以退出码 1 标记诊断异常。

需要独立后台运行时，可在项目根目录由终端启动：

```sh
nohup .tools/python/bin/python -m service.bridge > /tmp/hudbuddy-bridge.log 2>&1 &
```

该命令不安装 LaunchAgent。使用 `Ctrl-C` 或向该进程发送 `SIGTERM` 停止。烧录前先停止同步服务，释放串口。

## HUD 数据与失败处理

服务启动先调用当前 HUD Skill 的 `scripts/check_compatibility.py --cli <绝对路径>`，通过 `info --json` 校验 Skill manifest 与 CLI/API 版本，兼容才调用业务数据。默认 Skill 检查器路径是 `~/.codex/skills/hud/scripts/check_compatibility.py`；默认 CLI 是 `~/.local/bin/hud-cli`。兼容检查失败退出，不发起 `list_tasks`。

每次 HUD 检查只调用 `list_tasks`，参数为 `{"filter":"all"}`，再筛选 `doing`、`waiting`，保留 API 返回的 HUD 原顺序，不按状态、标题或更新时间重排。每项仅传 `id`、`title`、`status`、`summary`、`updated_at` 及状态显示字段。状态名与色值来自 CodexHud `apps/desktop/src/state.ts` 和 `styles.css` 深色主题：

| status | status_label | status_color | status_background |
| --- | --- | --- | --- |
| doing | 进行中 | #a5c4ee | #2c3c52 |
| waiting | 等待中 | #e2c48b | #443a27 |

summary 不生成摘要、不截断。为小屏阅读移除明确的 Markdown 标题/引用/代码围栏标记，列表换成项目符号或勾选符号，单反引号保留其中内容；简单 HTTP(S)/mailto 行内链接和图片保留可见文字/替代文字，移除目标地址。复杂嵌套链接和其他不明确的 Markdown 保留原样。代码围栏内部内容保留。CRLF/CR 归一化为 LF，去除不可见 ASCII 控制字符（保留 LF 和 TAB）。

整个 snapshot 最多 **64 个任务**、**102400 字节 UTF-8（含末尾 LF）**。超限直接报错，不截断任务或文字。CLI 非零退出码、`ok=false`、超时、JSON/结构错误和报文超限都保留进程内最后一次成功快照；既有屏幕数据保持。HUD 首次读取失败时省略 `tasks`，同时发送 `hud_ok:false`；设备保留之前任务，额度仍可更新。已有成功任务缓存则携带缓存及 `hud_ok:false`。只有成功读取确实没有 doing/waiting 时才发送 `tasks:[]`。

CLI 业务调用超时上限 1 秒，USB 写入超时 1 秒。HUD/Magpie 轮询、心跳和 ACK 使用独立单调时钟截止时间；ACK 检查不会等待下一次 60 秒 HUD 轮询。心跳表示通信存活，不能据此判断 HUD 数据新鲜度；查询错误明确记录到 stderr，恢复时记录恢复日志。HUD 任务缓存仅保存在进程内，重启服务后需要重新读取 CLI；Magpie 缓存持久化方式见下文。

## Magpie 额度与最后连通时间

默认只请求 `GET http://127.0.0.1:3425/v1/magpie/quotas`；使用不带代理的 urllib opener，禁止 HTTP 重定向，超时 2 秒、响应体上限 1 MiB。不调用重置卡接口或任何 POST。

只选择 `provider` 精确为 `codex` 且 `kind` 为 `subscription` 的条目。唯一候选直接使用；多个候选时必须恰有一条 `last:true`，否则明确报歧义并保留旧额度，不按列表顺序换账号。无候选是成功的“无 Codex 账号”状态：`available:false,stale:false`。

7 days/weekly 和 5 hours/5h 窗口分别投影为 `weekly`、`five_hour`。缺失窗口为 null，不能据此推断无限额；剩余百分比只使用明确的 `remaining`，仅接受 0–100 的有限数，缺失/无效为 null。ISO 时间必须自带时区，转换为 Unix 毫秒；缺失/无效时间为 null，`read_at_ms` 的未知值用 0。时间范围为 0–253402300799999 毫秒。

重置卡 `count` 必须是非负整数，最多 64；缺少 count 时整个 cards 为 null，不从日期数量推断。`each` 最多 64 项且不能多于 count；逐项读取 until，已知到期日期升序排列、未知日期 null 放末尾并保留。日期数组短于 count 代表剩余卡的日期未知。仅 count=1 且 each 缺失/null 时允许用顶层 until 兜底，不用一个总日期伪造多张卡到期时间。account 最多 512 UTF-8 字节，plan 最多 128 字节；超限报错，不截断。

HUD 与 Magpie 在同一轮询周期分别读取和处理错误：任一源失败不会阻止另一源更新。Magpie 失败保留最近成功值并置 `stale:true`；首次失败为 `available:false,stale:true`。恢复成功清除 stale。业务比较忽略 `read_at_ms`、`last_success_ms`、`monitor_started_ms`，所以每分钟仅更新时间不会推 snapshot；额度/账号/健康状态变化才推。

每次成功 HTTP + 有效 schema 读取均更新 `last_success_ms`，包括“无 Codex 账号”的成功响应；`monitor_started_ms` 是首次尝试时间。`.state/magpie-state.json` 通过同目录临时文件原子替换、权限 0600，保存这两个时间与最近成功的规范化额度，无 token。该目录独立于 Arduino 编译目录，重新编译固件不会清掉缓存。重启后旧缓存先视为 stale，当前读取成功后恢复；首次失败的开始时间也持久化，避免重启重置 24 小时计时。

snapshot 带额度时间字段；每 600 秒 heartbeat 额外携带 `magpie_last_success_ms` 与 `magpie_monitor_started_ms`，允许最多约 10 分钟传播延迟。设备以 `last_success_ms ?? monitor_started_ms` 为基准，当前时间**超过**基准 24 小时才在 Codex 后显示红点；从未尝试且两个时间均为 null 时不显示红点。该红点表示距上次成功读取 Magpie 已超过 24 小时，与顶部 USB 通信状态独立。

## USB 协议 1

所有报文为 UTF-8 单行 JSON，最后一个字节为 LF。报文中的字符串换行使用 JSON 转义，不形成额外物理行。每条消息的最大长度同样为 102400 字节，含 LF。

电脑 → 设备 snapshot（字段顺序无意义）：

```json
{"type":"snapshot","protocol":1,"unix_ms":1793400000000,"tz_offset_min":480,"seq":1,"tasks":[{"id":"task-id","title":"任务标题","status":"doing","summary":"完整任务说明","updated_at":1793400000000,"status_label":"进行中","status_color":"#a5c4ee","status_background":"#2c3c52"}]}
```

`unix_ms` 为当前 Unix 毫秒数，`tz_offset_min` 为电脑当前时区距 UTC 的分钟偏移（上海为 480）；`updated_at` 原样保留 HUD Task 的时间字段。`seq` 在单个服务进程内按实际发送的快照递增。`tasks` 与 `quota` 均可选，但至少包含一个；缺省字段在设备上保留原值。`hud_ok` 表示当前 HUD 读取是否成功。任务或额度变化、首次连接/重连、设备重启发来 hello 时发送完整快照，不做增量更新；普通源检查无业务变化时不发送。已有待确认快照时合并 hello，不重复发送。重连时即使 CLI 暂时失败，也会重新发送已有的最后成功快照。

snapshot 可额外包含 quota（以下为只更新额度、HUD 暂不可用的示例）：

```json
{"type":"snapshot","protocol":1,"unix_ms":1793437200000,"tz_offset_min":480,"seq":2,"hud_ok":false,"quota":{"available":true,"stale":false,"account":"user@example.test","plan":"prolite","read_at_ms":1793437200000,"last_success_ms":1793437200000,"monitor_started_ms":1793437100000,"weekly":{"remaining":66,"resets_at_ms":1793968200000},"five_hour":null,"cards":{"count":1,"expires_at_ms":[1796083200000]}}}
```

电脑 → 设备 heartbeat：

```json
{"type":"heartbeat","protocol":1,"unix_ms":1793437200000,"tz_offset_min":480,"magpie_last_success_ms":1793437200000,"magpie_monitor_started_ms":1793437100000}
```

设备 → 电脑启动时（若此前已收到有效 snapshot，则省略握手；USB 重接由电脑端 IOKit 触发推送）：

```json
{"type":"hello","protocol":1,"device":"HudBuddy"}
```

设备 → 电脑仅在完整校验并应用 snapshot 后：

```json
{"type":"ack","seq":1}
```

ack 的 seq 必须匹配当前最近一次发送的 snapshot。无效快照不确认。确认计时从完整串口写入完成后开始，不包含 CLI 查询和发送等待时间；等待确认超过固定 **6 秒**后，服务关闭连接，按上述独立重连退避重新发现/连接并重发快照；连续 HUD 更新不会无限延长首次待确认的期限。写超时或部分写入也会关闭连接并重试完整快照。心跳无需确认。设备启动诊断等非 JSON 行忽略；过长输入行丢弃到下一 LF 后恢复解析，避免串口缓存无限增长。

有效 snapshot 与 heartbeat 都以设备接收时刻更新“上次通信”时间，顶部小点按距此时刻的时间差显示：少于 1 分钟蓝色、1–10 分钟绿色、10–60 分钟黄色、1 小时起红色，尚未通信为灰色。普通 HUD 查询未推送时不会更新这个时间。设备不再每 10 秒发送 health 诊断，保留 ACK 及屏保／息屏等事件上报。

USB 数据和心跳不属于用户活动；固件遵守空闲 60 秒小狗屏保、再过 60 秒息屏、息屏后仅短按 BOOT 唤醒的策略。

## 验证

```sh
.tools/python/bin/python -m unittest discover -s tests -p 'test_*.py' -v
```

测试使用临时 fake CLI、mock serial 和操作系统虚拟 PTY，覆盖额度窗口/卡片/账号选择、缓存权限和原子替换、失败恢复、跨重启最后连通时间、两源独立容错、仅观察时间变化不推送、协议边界、失败保留、任务过滤和顺序、60/600 秒独立计时、离线停止所有工作、目标 USB 身份及增删去重、快速拔插事件保序、移除时 registryID 缓存、6 秒 ACK、重连退避和控制线处理；不访问实机串口。
