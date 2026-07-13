# 构建

## 要求

- Windows x86 构建环境。
- CMake 3.20 或更新版本。
- C++17 编译器。
- 静态 32 位 zlib。
- Windows SDK 库：`user32` 和 `advapi32`。

输出必须是一个 32 位的 ASI。64 位构建无法被《波斯王子（2008）》加载。

## MSVC + vcpkg

打开支持 x86 的 Visual Studio 开发者环境，然后：

```bat
vcpkg install zlib:x86-windows-static
set VCPKG_ROOT=C:\path\to\vcpkg
call tools\build\build_msvc_x86.bat
```

等效的预设命令：

```bat
set VCPKG_ROOT=C:\path\to\vcpkg
cmake --preset msvc-x86-release
cmake --build --preset msvc-x86-release
```

目标使用静态 MSVC 运行时（`/MT`）。

## MinGW-w64

提供对应的 32 位静态 `libz.a`：

```bat
set ZLIB_STATIC_LIB=C:\mingw32\lib\libz.a
call tools\build\build_mingw_x86.bat
```

脚本使用 `-static`、`-static-libgcc` 和 `-static-libstdc++` 进行链接，以避免运行时 DLL 依赖。

## 输出

```text
PoP_UniversalPatch.asi
PoP_UniversalPatch.ini
```

CMake 会将默认 INI 复制到构建后的 ASI 旁边。

## 依赖验证

MSVC：

```bat
dumpbin /dependents PoP_UniversalPatch.asi
```

MinGW：

```bat
objdump -p PoP_UniversalPatch.asi | findstr "DLL Name"
```

预期的依赖项应为 Windows 系统 DLL。最终的 ASI 不得依赖于：

```text
zlib1.dll
libwinpthread-1.dll
libstdc++-6.dll
libgcc_s_*.dll
VCRUNTIME*.dll
MSVCP*.dll
```

## 可选的 D3DX 运行时

纹理加载器不链接 D3DX。启用时，它会从已安装的 `d3dx9_*.dll` 动态解析 `D3DXCreateTextureFromFileInMemoryEx`，优先使用 `d3dx9_43.dll`。

如果 TPF 解析正常但图像替换被禁用，请安装提供 D3DX9 的旧版 DirectX 9 运行时。

## 错误 126

错误 126 发生在 PoPOpt 初始化之前，通常意味着缺少或不兼容的依赖 DLL。请检查 ASI 导入表，并验证所有非系统依赖项均为 32 位。仅放置在加载器子目录中 ASI 旁边的 DLL 可能无法被搜索到；静态链接是首选解决方案。