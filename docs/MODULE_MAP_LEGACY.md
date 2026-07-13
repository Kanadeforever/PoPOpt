# 模块映射

| 原始 v28 职责 | v29 模块 |
|---|---|
| 路径、日志、INI 辅助函数 | `src/core/Core.*` |
| PE 节区、RVA 解析、原子补丁 | `src/core/PeUtils.*` |
| `LNG_Language` 和注册表支持的设置映射 | `SettingsRegistryModule.*` |
| 图形配置验证/报告 | `GraphicsModule.*` |
| 语音包/四选择器/SteamStub 逻辑 | `VoiceModule.*` |
| 窗口模式字符串和无边框窗口 | `DisplayModule.*` |
| DPI 感知 | `DpiModule.*` |
| CPU 亲和性 | `CpuModule.*` |
| DirectInput Win 键和焦点鼠标释放 | `InputModule.*` |
| TPF 解析器和 D3D9 替换 | `TextureLoaderModule.*`、`texture/TpfArchive.*` |
| 兼容性报告 | `DiagnosticsModule.*` |
| 仅启动序列 | `Main.cpp` |

## 钩子所有权

`HookManager` 是唯一写入主模块 IAT 槽位的组件。模块在安装前注册规范：

- `SettingsRegistry`：`RegQueryValueExA/W`
- `Voice`：`CreateFileA/W`
- `Input`：`DirectInput8Create` 以及一个动态的 `GetProcAddress` 解析器
- `TextureLoader`：`Direct3DCreate9`

当至少注册了一个动态解析器时，管理器会一次性安装 `GetProcAddress`。