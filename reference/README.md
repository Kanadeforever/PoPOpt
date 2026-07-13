# 参考快照

这些文件保存了在模块化 v29 时用于对比行为的源代码快照。它们**不会被**根 CMake 项目编译。

```text
v28-single-file/
  PoP4_UniversalPatch_v28_stable_base.cpp

tpf-loader/
  PoP_TPFLoader_v0.1_verified_base.cpp
  TpfArchive.cpp
  TpfArchive.h
```

在将其逻辑移入 `src/modules/TextureLoaderModule.cpp` 之前，TPF 加载器快照成功生成了游戏内替换。活动代码位于 `src/` 下。
