# 配置参考

运行时配置文件固定为 `PoP_UniversalPatch.ini`，从 ASI 目录读取。如果文件不存在，PoPOpt 会生成一个 UTF-16 编码的默认文件。

## 语言

| 键 | 范围 | 默认值 | 说明 |
|---|---|---:|---:|
| `TextLanguage` | 0–13 | 6 | 文本、UI 和字幕语言。`0` 为无/自动。 |
| `VoiceLanguage` | 1–13 | 1 | 请求的语音包语言。 |

语言 ID：1 英语、2 法语、3 西班牙语、4 波兰语、5 德语、6 中文、7 匈牙利语、8 意大利语、9 日语、10 捷克语、11 韩语、12 俄语、13 荷兰语。

## 语音

| 键 | 值 | 默认值 | 说明 |
|---|---|---|---:|---:|
| `Enable` | 0/1 | 1 | 启用独立的语音语言处理。 |
| `AssetPatch` | 0/1 | 1 | 启用资源、包和名称选择器。 |
| `RequireAssetPatch` | 0/1 | 1 | 要求所有四个选择器匹配后才写入。 |
| `AutoFallback` | 0/1 | 1 | 如果请求的 forge 文件缺失，则使用 `FallbackLanguage`。 |
| `FallbackLanguage` | 1–13 | 1 | 安全的备用语音语言。 |

语音包使用 `DataPC_StreamedSounds<suffix>.forge` 格式，例如 `DataPC_StreamedSoundsEng.forge`。

## 显示

| 键 | 范围 | 默认值 | 说明 |
|---|---|---|---:|---:|
| `WindowMode` | 0–2 | 2 | 0 全屏、1 窗口化、2 按配置分辨率无边框窗口。 |
| `BorderlessCenter` | 0/1 | 0 | 将无边框窗口在其显示器上居中。 |
| `Width` | 320–16384 | 1920 | 请求的宽度。 |
| `Height` | 240–16384 | 1080 | 请求的高度。 |
| `VSync` | 0/1 | 1 | 垂直同步注册表值。 |
| `Widescreen` | 0/1 | 1 | 启用宽屏覆盖。 |
| `AspectRatio` | 0–999 | 0 | `0` 时计算 `floor((Width/Height)*100)`。 |

无边框模式并非强制性的无边框全屏；它使用配置的分辨率。

## DPI

| 键 | 值 | 默认值 | 说明 |
|---|---|---|---:|---:|
| `DPIAware` | 0/1 | 1 | 启用进程 DPI 感知。 |
| `DPIAwareness` | 1–3 | 1 | 1 系统、2 每显示器、3 每显示器 V2。 |

## 性能

| 键 | 范围 | 默认值 | 说明 |
|---|---|---|---:|---:|
| `LimitCpuCores` | 0/1 | 1 | 限制进程亲和性。 |
| `MaxCpuCores` | 1–10 | 4 | 使用当前允许的前 N 个逻辑处理器。 |

## 输入

| 键 | 值 | 默认值 | 说明 |
|---|---|---|---:|---:|
| `AllowWinKeyInWindowed` | 0/1 | 1 | 在窗口化/无边框模式下清除 `DISCL_NOWINKEY`。 |
| `ReleaseMouseOnFocusLoss` | 0/1 | 1 | 游戏失去焦点时调用 `ClipCursor(NULL)`。 |

两项输入修正均保持全屏行为不变。

## 图形

| 键 | 值 | 默认值 |
|---|---|---:|
| `Quality` | 0 低、1 中、2 高 | 2 |
| `AntiAliasing` | 0、2、4、8 | 4 |
| `HighResolutionTextures` | 0/1 | 1 |
| `ShadowQuality` | 0–2 | 2 |
| `PostEffects` | 0–2 | 2 |

## 纹理加载器

| 键 | 值 | 默认值 | 说明 |
|---|---|---|---:|---:|
| `Enable` | 0/1 | 0 | 启用 TPF/ZIP 解析和 D3D9 纹理钩子。 |
| `LaterPackageWins` | 0/1 | 1 | 靠后的 `PackageN` 条目替换靠前条目中具有相同哈希的定义。 |
| `Package1` … `Package64` | 路径 | — | 显式的包加载顺序。相对路径从 ASI 目录开始。 |

纹理匹配仅基于哈希值。替换图像的尺寸可能与原始纹理不同。

## 调试

| 键 | 范围 | 默认值 | 说明 |
|---|---|---|---:|---:|
| `LogLevel` | 0–3 | 2 | 0 关闭、1 仅警告/错误、2 正常、3 详细文件跟踪。 |
| `CompatibilityReport` | 0/1 | 1 | 写入 `PoP_CompatibilityReport.txt`。 |
| `PackedSteamPatchTries` | 1–60000 | 10000 | SteamStub 运行时补丁尝试次数。 |
| `PackedSteamPatchIntervalMs` | 1–1000 | 1 | 尝试之间的延迟（毫秒）。 |
| `PackedSteamPatchStatusEvery` | 1–60000 | 1000 | 进度日志输出间隔。 |
| `TextureLogUnmatched` | 0/1 | 0 | 诊断性未匹配纹理日志记录。 |
| `TextureMaxUnmatchedLogs` | 0–100000 | 256 | 诊断日志上限。 |