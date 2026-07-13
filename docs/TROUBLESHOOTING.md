# 故障排除

## ASI 加载器错误 126

ASI 未被加载。检查依赖项并使用静态 zlib/运行时重新构建。不要通过随意分发 `zlib1.dll` 或 MinGW 运行时 DLL 来解决发布版本的问题。

## 文本语言已配置但游戏使用其他语言

请查找：

```text
LNG_Language intercepted through RegQueryValueExA: returned=6
```

或 W 变体。仅显示 `TextLanguage=6` 的消息仅确认了配置值；拦截行才能确认游戏收到了该值。

## 文本正常但没有语音音频

- 确认请求的 `DataPC_StreamedSoundsXXX.forge` 存在于游戏目录中。
- 检查报告中的请求和生效语音语言。
- 检查 `VoiceFullPatch` 以及所有四个选择器匹配情况。
- 除非有意测试旧的局部模式，否则请保持 `AssetPatch=1` 和 `RequireAssetPatch=1`。

## TPF 已加载但没有纹理被替换

- 确认 `TextureLoader.Enable=1`。
- 确认包路径是从 ASI 目录解析的。
- 确认 D3DX9 运行时已解析。
- 使用 `LogLevel=3`，并仅用于诊断时临时启用 `TextureLogUnmatched=1`。
- 不要要求替换图像与原始纹理具有相同的尺寸。

## Win 键仍然被屏蔽

该项修复仅适用于 `WindowMode=1` 或 `2`。检查 DirectInput 钩子和 `DISCL_NOWINKEY cleared` 日志消息。

## Alt+Tab 后鼠标仍然被限制

检查 `ReleaseMouseOnFocusLoss=1`，并确保游戏处于窗口化/无边框模式。