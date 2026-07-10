# PoPOpt

中文 | [English](README_EN.md)

## 介绍

**PoPOpt（PoP Universal Patch）** 是一个面向《Prince of Persia (2008)》PC 版的单文件、32 位、运行时生效的 ASI 补丁。

补丁不会修改磁盘上的游戏 EXE，可自动识别并适配：

- GOG / 未加壳版
- Steam 脱壳版
- Steam 原版 SteamStub 加壳版

主要功能包括文本与语音语言分离、语音包检测和安全回退、窗口与无边框模式、高 DPI 感知、图形设置、CPU affinity、窗口模式输入修复、配置校验、日志和兼容性报告。

> 这是非官方社区补丁。建议保留原始游戏文件，并使用与游戏匹配的 32 位 ASI Loader。

## 核心功能

- 文本、菜单、字幕语言与对话语音语言可独立选择。
- 自动识别 GOG、Steam 脱壳版和 SteamStub 加壳版。
- Package + Resource + Bundle + Name 四点事务式语音补丁。
- 启动时检测语音包；缺失时可安全回退到指定语言。
- 普通窗口和指定分辨率无边框窗口。
- 分辨率、宽高比、VSync 和宽屏设置外置。
- 抗锯齿、总体画质、高分辨率贴图、阴影和后期效果外置。
- System / Per-Monitor / Per-Monitor V2 DPI 感知。
- CPU 逻辑核心数限制，默认 4，最大 10。
- 窗口或无边框模式下恢复 Win 键。
- 游戏失去焦点时释放鼠标，不影响有焦点时的正常捕获。
- 配置文件自动生成、非法值校验和自动修正。
- 可调日志等级和兼容性报告。
- 配置、日志、报告及语音包检测支持 Unicode 路径。

## 安装

1. 准备能够加载 32 位 ASI 的游戏环境。
2. 将 `PoP_UniversalPatch.asi` 放到 ASI Loader 能加载的位置。
3. 将 `PoP_UniversalPatch.ini` 放在 ASI 同一目录。
4. 确认所需的 `DataPC_StreamedSoundsXXX.forge` 位于游戏 EXE 所在目录。
5. 启动游戏。补丁会自动识别发行版、校验配置并应用适配。

如果 `PoP_UniversalPatch.ini` 不存在，补丁会在 ASI 所在目录自动生成一份完整的 UTF-16 默认配置。

## 默认文件

补丁使用以下固定文件名：

- `PoP_UniversalPatch.asi`：补丁本体
- `PoP_UniversalPatch.ini`：配置文件
- `PoP_UniversalPatch.log`：运行日志
- `PoP_CompatibilityReport.txt`：兼容性报告

## 推荐配置：中文文本 + 英文语音

```ini
[Language]
TextLanguage=6
VoiceLanguage=1

[Voice]
Enable=1
AssetPatch=1
RequireAssetPatch=1
AutoFallback=1
FallbackLanguage=1
```

## 语言编号与语音包

| 编号 | 语言 | 语音包后缀 |
|---:|---|---|
| 0 | None / Auto | 仅适用于 `TextLanguage` |
| 1 | English | Eng |
| 2 | French | Fre |
| 3 | Spanish | Spa |
| 4 | Polish | Pol |
| 5 | German | Ger |
| 6 | Chinese | Chi |
| 7 | Hungarian | Hun |
| 8 | Italian | Ita |
| 9 | Japanese | Jap |
| 10 | Czech | Cze |
| 11 | Korean | Kor |
| 12 | Russian | Rus |
| 13 | Dutch | Dut |

语音包命名格式：

```text
DataPC_StreamedSounds<后缀>.forge
```

例如英文语音包为：

```text
DataPC_StreamedSoundsEng.forge
```

`TextLanguage` 可使用 `0`～`13`；`VoiceLanguage` 和 `FallbackLanguage` 只能使用 `1`～`13`。

### 语音包自动检测与安全回退

当 `AutoFallback=1` 时：

1. 先检查 `VoiceLanguage` 对应的语音包。
2. 如果目标语音包不存在，则检查 `FallbackLanguage`。
3. 如果回退语音包存在，本次运行会安全使用回退语言。
4. 如果目标与回退语音包都不存在，整套语音补丁会停用，避免产生不一致状态。

自动回退只改变本次运行的实际语音语言，不会改写 `[Language] VoiceLanguage`。

建议保持以下两项为 `1`：

```ini
AssetPatch=1
RequireAssetPatch=1
```

此时 Package、Resource、Bundle 和 Name 四处必须全部匹配，才会一起写入。

## 显示设置

默认显示配置：

```ini
[Display]
WindowMode=2
BorderlessCenter=0
Width=1920
Height=1080
VSync=1
Widescreen=1
AspectRatio=0
```

`WindowMode`：

- `0`：全屏
- `1`：普通窗口
- `2`：无边框窗口，默认；按 `Width × Height` 运行，不是无边框全屏

`BorderlessCenter` 仅对无边框模式生效：

- `0`：保留游戏窗口位置
- `1`：在当前显示器工作区居中

有效分辨率范围：

- `Width`：320～16384
- `Height`：240～16384

`AspectRatio=0` 时，补丁会自动计算：

```text
floor((Width / Height) × 100)
```

例如：

- 1920×1080 → 177
- 3840×2160 → 177
- 3440×1440 → 238

手动填写非零值时，有效范围为 50～500。

## DPI 设置

```ini
[DPI]
DPIAware=1
DPIAwareness=1
```

`DPIAwareness`：

- `1`：System DPI Aware，推荐默认
- `2`：Per-Monitor DPI Aware
- `3`：Per-Monitor V2

较新的 Per-Monitor 模式可能改变老游戏的窗口尺寸或缩放行为，遇到问题时请改回 `1`。

## 图形设置

```ini
[Graphics]
Quality=2
AntiAliasing=4
HighResolutionTextures=1

[AdvancedGraphics]
ShadowQuality=2
PostEffects=2
```

设置范围：

- `Quality`：`0=Low`、`1=Medium`、`2=High`
- `AntiAliasing`：`0=关闭`、`2=x2`、`4=x4`、`8=x8`
- `HighResolutionTextures`：`0=低分辨率贴图`、`1=高分辨率贴图`
- `ShadowQuality`：0～2
- `PostEffects`：0～2

不合法的抗锯齿值会自动修正到最接近的 `0`、`2`、`4` 或 `8`。

## 性能与输入

```ini
[Performance]
LimitCpuCores=1
MaxCpuCores=4

[Input]
AllowWinKeyInWindowed=1
ReleaseMouseOnFocusLoss=1
```

### CPU affinity

`LimitCpuCores=1` 时，补丁会在当前系统允许的 affinity 范围内选择前 N 个逻辑核心。

`MaxCpuCores` 的有效范围为 1～10，默认 4。补丁不会扩大 Windows 或其他工具已经施加的 affinity 限制。

### Win 键修复

`AllowWinKeyInWindowed=1` 时，补丁会在普通窗口或无边框窗口模式下清除 DirectInput 的 `DISCL_NOWINKEY` 标志。

全屏模式不处理，保留游戏原始行为。

### 失焦释放鼠标

`ReleaseMouseOnFocusLoss=1` 时，游戏保持焦点期间仍可正常捕获鼠标；Alt+Tab 或切换到其他程序后，补丁会释放 `ClipCursor`。

该功能只在普通窗口或无边框窗口模式下生效。

## 配置校验与自动修正

启动时补丁会校验主要配置项：

- 布尔值统一修正为 `0` 或 `1`
- 语言编号限制到允许范围
- `WindowMode` 限制为 0～2
- 分辨率限制到安全范围
- `AspectRatio` 非正数归零，正数限制为 50～500
- `MaxCpuCores` 限制为 1～10
- 抗锯齿修正到最接近的有效值
- 图形、DPI、日志和 Steam 运行时参数限制到允许范围

如果整个配置文件不存在，会自动生成完整默认配置；如果配置文件已经存在，只会把检测到的非法或越界值写回修正，不会自动补齐所有缺失项。

## 日志与兼容性报告

```ini
[Debug]
LogLevel=2
CompatibilityReport=1
```

`LogLevel`：

- `0`：完全关闭日志，并删除上一轮旧日志
- `1`：仅记录警告、错误和配置修正
- `2`：记录正常启动、识别与补丁状态，推荐默认
- `3`：详细调试，包括 DataPC、forge 和声音相关文件打开跟踪

`CompatibilityReport=1` 时会生成：

```text
PoP_CompatibilityReport.txt
```

报告内容包括：

- 游戏 EXE 路径、大小、入口点和识别类型
- 配置文件路径和本次配置修正数量
- 请求与实际生效的文本、语音语言
- 窗口模式、分辨率、宽高比和 CPU affinity
- 语音补丁、窗口补丁和各类 Hook 状态
- 检测到的所有语音包

### SteamStub 高级参数

```ini
[Debug]
PackedSteamPatchTries=10000
PackedSteamPatchIntervalMs=1
PackedSteamPatchStatusEvery=1000
```

这些参数控制 SteamStub 加壳版运行时语音补丁的重试行为。普通用户应保留默认值。

## 安全设计

- 不修改磁盘上的游戏 EXE。
- 所有修改仅在游戏进程内存中生效，退出游戏后消失。
- 语音四处必须全部匹配后才会一起写入。
- 缺少目标语音包时会安全回退或停用语音补丁。
- 关键字节不匹配时会跳过对应补丁，而不是强行写入。
- SteamStub 加壳版只在运行时代码准备完成后检查已确认的固定 RVA。

## 常见问题

### ASI Loader 提示 Error 126

这通常表示 ASI 存在缺失的硬依赖。请确认：

- ASI 编译为 32 位
- MSVC 使用 `/MT` 静态运行库
- 未链接 `dinput8.lib`、`dxguid.lib` 或 `xinput.lib`

### 文本正常，但没有语音

检查以下项目：

1. `VoiceLanguage` 对应的 `DataPC_StreamedSoundsXXX.forge` 是否存在。
2. `[Voice] Enable`、`AssetPatch` 和 `RequireAssetPatch` 是否为 `1`。
3. 查看 `PoP_UniversalPatch.log` 和 `PoP_CompatibilityReport.txt` 中的实际语音语言及语音补丁状态。

### 语音自动回退到英文

目标语音包不存在，并且：

```ini
AutoFallback=1
FallbackLanguage=1
```

因此本次运行安全使用了英文语音。原始 `VoiceLanguage` 配置不会被改写。

### Win 键仍然无效

确认：

- `WindowMode=1` 或 `WindowMode=2`
- `AllowWinKeyInWindowed=1`
- 日志中 DirectInput Hook 已成功安装

全屏模式下补丁不会恢复 Win 键。

### Alt+Tab 后鼠标仍被限制

确认：

- `WindowMode=1` 或 `WindowMode=2`
- `ReleaseMouseOnFocusLoss=1`

若问题仍然存在，请使用 `LogLevel=2` 或 `3` 重新测试。

### 配置值被自动改写

这是配置校验功能。非法或越界值会自动修正并写回 `PoP_UniversalPatch.ini`；当 `LogLevel` 大于等于 1 时，日志会记录修正内容。

## 问题反馈

反馈问题时请附上：

- `PoP_UniversalPatch.ini`
- `PoP_UniversalPatch.log`，建议使用 `LogLevel=2` 或 `3`
- `PoP_CompatibilityReport.txt`
- 游戏发行版及 EXE 是否经过脱壳或修改

## 编译

必须编译成 32 位 ASI，推荐使用静态运行库。

MSVC：

```bat
cl /LD /O2 /EHsc /MT PoP_UniversalPatch.cpp /link /OUT:PoP_UniversalPatch.asi /MACHINE:X86 user32.lib advapi32.lib
```

不要额外链接：

```text
dinput8.lib
dxguid.lib
xinput.lib
```

DirectInput Win 键修复通过动态解析和 COM vtable Hook 实现，不需要这些硬依赖。

## 依赖项

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
