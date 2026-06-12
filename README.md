# RustDesk HDR Monitor

作者：Jerrell851  代码助手：Deepseek

程序功能：RustDesk 远程连接建立时自动关闭 Windows HDR，断开后恢复。解决远程桌面 HDR 导致画面过曝/颜色异常。

故障现象：通过 RustDesk 连接时画面会严重泛白，关闭 HDR 后恢复正常。 
起因是HDR 内容的亮度范围远超 SDR 的 0–255，RustDesk 捕获后没有做 Tone Mapping（色调映射），直接截断压缩，导致亮部全糊成白色。

现状
这个问题在 2025 年 8 月仍有新的 Issue 被提交（#12707），症状描述为连接到开了 HDR 的被控端时画面异常、对比度失真。 GitHub 上最早的相关 Issue 可以追溯到 2023 年，至今未修复，也没有官方路线图说明何时会支持。


## 用法

```powershell
# 安装为 Windows 服务（需管理员权限）
RustDeskHdrService.exe --install

# 卸载
RustDeskHdrService.exe --uninstall

# 查询 HDR 状态和当前连接数
RustDeskHdrService.exe --action status

# 手动控制 HDR
RustDeskHdrService.exe --action disable
RustDeskHdrService.exe --action enable

# 开启调试日志（写 EXE 同目录）
RustDeskHdrService.exe --action status --debug

# 强制键盘模式（不用 DisplayConfig API）
RustDeskHdrService.exe --force-keyboard --action disable
```

双击 EXE 弹出中文帮助对话框。

## 特性

- **CURRENT.log 尾扫**：只读尾部 8KB 判断当前状态，不遍历全量历史日志
- **事件驱动 + 5s 兜底**：`FindFirstChangeNotificationW` 内核通知，`WaitForMultipleObjects` 阻塞不占 CPU
- **DisplayConfig API**：程序化 HDR 开关，逐显示器查询/设置；API 不可用时回落 `SendInput` Win+Alt+B
- **防抖确认**：连接 500ms / 断开 2s 确认期，5s HDR 冷却
- **Session 0 桥接**：WTS API + `CreateProcessAsUser` 在用户会话执行 HDR 操作
- **单 EXE**：`--install` / `--uninstall` 自管理，无外部依赖。

## 构建

需要 Visual Studio 2022 Build Tools 或完整 VS。

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

GitHub Actions 自动编译 x64。

## 工作原理

```
RustDesk cm 日志变更
    ↓ FindFirstChangeNotificationW + 5s poll
CURRENT.log 尾部扫描 + 增量读
    ↓ "Got new connection" / "cm ipc connection closed"
状态机（确认期 + 防抖 + 冷却）
    ↓
Service 模式: WTS + CreateProcessAsUser → --action disable/enable
交互模式: 直接调 DisplayConfig API
    ↓
HDR ON/OFF
```

## 许可证

MIT
