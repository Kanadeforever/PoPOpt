# 本工作区执行的源码审计

- 确认了所有源文件/头文件的括号平衡。
- 确认存在且仅有一个安装主模块 IAT 钩子的调用点。
- 确认只有 `Core.cpp` 写入主模块 IAT 槽位。
- 确认没有遗留的菜单快进/时间钩子实验代码。
- 确认集成的纹理模块没有尺寸过滤，仅使用 `~CRC32(top mip compact rows)`。
- 确认 `TextureLoader.Enable` 默认为 `0`。
- 针对本地的 Windows/D3D9 API 存根运行了 Clang C++17 语法和警告检查；未产生任何 C++ 语法或警告诊断。

这不等于针对真实的 Windows SDK 进行编译。仍然需要真实的 32 位 Windows 构建来验证 ABI、SDK 声明、静态 zlib 选择和运行时时序。