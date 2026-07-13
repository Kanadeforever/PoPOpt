# v28 → v29 模块化迁移说明

## 源码基线

重构使用 `reference/PoP4_UniversalPatch_v28_stable_base.cpp`，这是为稳定分支提供的最新完整源文件。工作区中没有独立的 v29 稳定版源码。

纹理模块基于 `reference/PoP_TPFLoader_v0.1_verified_base.cpp` 中的独立测试实现。其 TPF 解析、D3D9 钩子路径以及 `XBOX.tpf` 中全部十个哈希值在集成前已通过实际游戏日志确认。

## 保留的公开行为

- 固定输出文件名仍为 `PoP_UniversalPatch.ini`、`PoP_UniversalPatch.log` 和 `PoP_CompatibilityReport.txt`。
- 现有的 v28 配置键保留其值和默认值。
- GOG、Steam 未打包和打包 SteamStub 语音 RVA 不变。
- 当 `AssetPatch=1` 且 `RequireAssetPatch=1` 时，四个语音选择器仍保持事务性。
- 窗口模式、DPI、CPU 亲和性、Win 键和焦点丢失鼠标行为保留 v28 逻辑。

## 新行为

- 初始化在一个工作线程上执行，而非在 `DllMain` 内部运行完整的启动路径。
- 所有主模块的 IAT 写入由 `HookManager` 在一次传递中安装。
- 纹理替换是一个可选模块，默认禁用。
- 纹理匹配仅使用已验证的 `top-compact-complement` 哈希值。图像尺寸不是匹配条件。
- 兼容性报告通过要求每个模块贡献自己的部分来汇编。

## 需要在 Windows 上验证的项目

1. 使用真实的 x86 Windows SDK 编译并解决任何编译器特定的警告。
2. 确认打包 Steam 语音的时序在工作线程初始化下仍然足够早。
3. 以 `TextureLoader.Enable=0` 运行一次，并将日志与 v28 进行比较。
4. 以 `TextureLoader.Enable=1` 运行，确认所有十个控制器纹理已被替换。
5. 检查构建的 ASI 导入表，确保未导入 zlib/MinGW 运行时 DLL。