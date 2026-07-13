# 发布检查清单

- [ ] 使用静态 zlib 和静态 C/C++ 运行时构建 x86 Release 版本。
- [ ] 检查 ASI 依赖项；不得包含 zlib 或编译器运行时 DLL。
- [ ] 首先以 `TextureLoader.Enable=0` 测试。
- [ ] 验证实际的 `LNG_Language intercepted` 日志条目。
- [ ] 验证请求/生效的语音语言以及所有四个选择器。
- [ ] 测试维护者可用的 GOG/类似未打包版本和 Steam 构建版本。
- [ ] 启用 TPF 加载器并确认所有预期的哈希值。
- [ ] 检查 D3D9 Reset 和关闭行为。
- [ ] 确认默认 INI 与源码默认值一致。
- [ ] 移除私人日志、游戏可执行文件、已编译的第三方 DLL 和不相关的资源。
- [ ] 决定并添加仓库许可证。
- [ ] 标记发布版本，并附上 `PoP_UniversalPatch.asi`、`PoP_UniversalPatch.ini`、README 和校验值。