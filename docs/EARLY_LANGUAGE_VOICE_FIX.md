# v29 模块化早期语言/语音修复

此修订版直接基于用户提供的 `PoPOpt_v29_modular.zip`。

保留的本地更改：

- `InputModule.cpp` 保留 `#include <unknwn.h>`。
- `Main.cpp` 保留 MinGW `__mingw_SEH_error_handler` 工作区。
- `build_msvc_x86.bat` 保留末尾的 `pause`。

更改后的启动顺序：

1. `DllMain` 解析 ASI/配置/日志路径。
2. 日志记录和 EXE 风格检测同步初始化。
3. 语音包检测/后备同步解析。
4. 在 `DllMain` 返回之前安装 `RegQueryValueExA/W` 和语音 `CreateFileA/W` 钩子。
5. GOG/Steam 未打包的语音选择器同步打补丁。打包的 Steam 准备好进行运行时补丁。
6. 显示、DPI、CPU、输入、图形、纹理加载和诊断仍保留在工作线程中。

SettingsRegistry 和 Voice 的生命周期阶段是幂等的，因此正常的模块化过程不会重复注册或应用它们两次。

新增的诊断行包括首次真实的 `LNG_Language` 拦截以及返回给游戏的值。