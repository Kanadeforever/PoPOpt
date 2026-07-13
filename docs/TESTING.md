# 测试计划

## 第一阶段：基础回归测试

使用 `TextureLoader.Enable=0`。

1. 确认 `LNG_Language intercepted ... returned=<TextLanguage>` 出现。
2. 确认菜单/字幕中的文本语言。
3. 确认请求的语音包及所有四个语音选择器。
4. 测试窗口模式 0、1 和 2。
5. 测试 DPI 模式 1。
6. 确认日志中的 CPU 亲和性。
7. 在窗口化/无边框模式下测试 Win 键和 Alt+Tab 鼠标释放。
8. 检查 `PoP_CompatibilityReport.txt`。

## 第二阶段：纹理加载器

1. 将 `test-assets/XBOX.tpf` 复制到 ASI 旁边，或更新 `Package1`。
2. 设置 `TextureLoader.Enable=1`。
3. 启动游戏并进入包含控制器提示的 UI 场景。
4. 确认出现十条 `Texture replaced` 条目，模式为 `mode=top-compact-complement`。
5. 如果可行，通过更改显示模式触发 D3D9 Reset。
6. 正常退出并确认关闭过程中出现 `Direct3D device released`。

已验证的控制器提示哈希值：

```text
0x33F889E2  0x7D4AF4C4  0xBC546F62  0x3E7C995A  0x47D26FD4
0x9BA66594  0x06F2129B  0xEA31EB5A  0xD56ACE57  0xD9CCF205
```

## 第三阶段：分发矩阵

在以下版本上重复语言/语音测试：

- GOG/类似未打包的可执行文件。
- Steam 未打包的可执行文件。
- 原始打包的 SteamStub 可执行文件。

对于每次失败，请附上配置、日志、兼容性报告和 EXE 风格信息。