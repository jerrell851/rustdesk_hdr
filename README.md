# RustDesk HDR Monitor

作者：Jerrell851  代码助手：Deepseek

程序功能：RustDesk 远程连接建立时自动关闭 Windows HDR，断开后恢复。解决远程桌面 HDR 导致画面过曝/颜色异常。

故障起因是HDR 内容的亮度范围远超 SDR 的 0–255，RustDesk 捕获后没有做 Tone Mapping（色调映射），直接截断压缩，导致亮部全糊成白色。

现状
这个问题在 2025 年 8 月仍有新的 Issue 被提交（#12707），症状描述为连接到开了 HDR 的被控端时画面异常、对比度失真。 GitHub 上最早的相关 Issue 可以追溯到 2023 年，至今未修复，也没有官方路线图说明何时会支持。

## 用法

所有命令需管理员权限。

### 服务管理

```powershell
RustDeskHdrService.exe --install        # 安装并启动服务
RustDeskHdrService.exe --uninstall      # 停止并删除服务
```

### 手动控制 HDR

```powershell
RustDeskHdrService.exe --action status    # 查询 HDR 状态和活跃连接数
RustDeskHdrService.exe --action disable   # 关闭 HDR
RustDeskHdrService.exe --action enable    # 开启 HDR
```

### 调试日志（持久化服务配置）

```powershell
RustDeskHdrService.exe --debug           # 开启调试日志（写入服务命令行并重启服务）
RustDeskHdrService.exe --no-debug        # 关闭调试日志
```

- 日志文件：EXE 同目录 `RustDeskHdrService_debug.log`（无写权限时落 `%TEMP%`）
- 服务未安装时，`--debug` / `--no-debug` 提示先 `--install`

### HDR 切换方式（持久化服务配置）

```powershell
RustDeskHdrService.exe --force-keyboard      # 键盘模式：SendInput Win+Alt+B
RustDeskHdrService.exe --no-force-keyboard   # API 模式：DisplayConfig API（默认）
```

- 键盘模式适用于老系统/显卡驱动不支持 DisplayConfig API 的场景
- 服务未安装时提示先 `--install`
- `--action status` 可查看当前使用的模式

### 组合使用

```powershell
# 安装时就指定键盘模式
RustDeskHdrService.exe --install --force-keyboard

# 安装时开启调试日志
RustDeskHdrService.exe --install --debug

# 手动关闭 HDR（本次用键盘），同时将服务切换为键盘模式
RustDeskHdrService.exe --force-keyboard --action disable
```

`--action` 总是先于服务重配置执行——组合使用时 HDR 操作立即生效，然后服务在后台重启。

### 参数一览

| 参数 | 作用 |
| ---- | ---- |
| `--install` | 安装并启动 Windows 服务 |
| `--uninstall` | 停止并删除服务 |
| `--action status/disable/enable` | 手动查询/关闭/开启 HDR |
| `--debug` | 开启调试日志（重启服务持久化） |
| `--no-debug` | 关闭调试日志 |
| `--force-keyboard` | HDR 切换方式改为键盘（重启服务持久化） |
| `--no-force-keyboard` | HDR 切换方式恢复为 API（重启服务持久化） |

## 运行逻辑

**1. 它是怎么知道 RustDesk 有没有人连进来的？**

RustDesk 每次连接/断开都会在日志目录 `cm\CURRENT.log` 里写下 "Got new connection" 或 "cm ipc connection closed"。程序盯着这个文件的变化，读到连接就关 HDR，读到断开就恢复。

只读文件尾部 8KB，不翻全量历史，不会因为日志大了就卡。监控靠 Windows 内核的文件变更通知，不轮询，不占 CPU。

**2. 为什么需要确认期和冷却？**

网络抖动或者 RustDesk 短暂重连，会产生瞬间的"断开→连接"波动。程序用 500ms 确认连接、2s 确认断开，5s 内不重复切换 HDR。这样屏幕不会闪。

用户断开后 2s 内又重新连入，程序取消恢复 HDR，保持关闭状态。

**3. 它怎么关 HDR？**

默认用 Windows 的 DisplayConfig API 程序化开关，直接告诉显卡驱动。如果老系统/老显卡不支持这个 API，可以用 `--force-keyboard` 切到键盘模式——程序模拟按下 Win+Alt+B（Windows 系统级 HDR 快捷键）。

**4. 服务在后台怎么操作我的显示器？**

Windows 服务跑在 Session 0，没有桌面、碰不到你的显示设置。程序通过 WTS API 找到你的登录会话，获取你的用户身份，然后启动一个自身副本（`--action` 模式）在你的桌面会话里操作。完成后退出，不留痕迹。

**5. 先装服务、后装 RustDesk 怎么办？**

服务启动时找不到 RustDesk 的 cm 日志目录，不会放弃。它每隔 30 秒探测一次 RustDesk 的 IPC 管道 `\\.\pipe\RustDesk\query`。只要 RustDesk 一运行，管道就会存在，服务立刻发现、启动日志监控。同时也会响应用户登录/注销事件。整个过程零 CPU 轮询。

**6. 怎么管理？**

单 EXE，无依赖。`--install` 注册为 Windows 服务自动启动，`--uninstall` 卸载。`--debug`、`--force-keyboard` 等配置直接修改服务启动参数，无需配置文件。

## 构建

需要 Visual Studio 2022 Build Tools 或完整 VS。

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

GitHub Actions 自动编译 x64。

## 工作原理

```text
RustDesk cm 日志变更
    ↓ FindFirstChangeNotificationW + 5s poll
CURRENT.log 尾部扫描 + 增量读
    ↓ "Got new connection" / "cm ipc connection closed"
状态机（确认期 + 防抖 + 冷却）
    ↓
Service 模式: WTS + CreateProcessAsUser → --action disable/enable
交互模式: 直接调 DisplayConfig API（或 SendInput 键盘模式）
    ↓
HDR ON/OFF
```

## 许可证

MIT
