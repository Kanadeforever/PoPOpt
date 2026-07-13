# 构建后 ASI 依赖检查

构建后，在游戏内测试之前验证导入表。

MSVC：

```bat
dumpbin /dependents PoP_UniversalPatch.asi
```

MinGW：

```bat
objdump -p PoP_UniversalPatch.asi | findstr "DLL Name"
```

预期的导入项应为 Windows 系统 DLL。最终的 ASI 不得导入：

```text
zlib1.dll
libwinpthread-1.dll
libstdc++-6.dll
libgcc_s_dw2-1.dll
libgcc_s_sjlj-1.dll
VCRUNTIME*.dll
MSVCP*.dll
```

对于 MSVC，请使用 `/MT` 和 `zlib:x86-windows-static`。对于 MinGW，请使用静态 `libz.a`，配合静态 libgcc/libstdc++，以及（如果工具链生成了的话）静态 winpthread。