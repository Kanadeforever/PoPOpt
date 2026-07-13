# Third-party notices

## zlib

PoPOpt links zlib statically for TPF/ZIP decompression. zlib source or binaries are not bundled in this repository. Obtain it from a trusted package manager or the official project and comply with its license.

## Microsoft Direct3D 9 / D3DX9

PoPOpt uses Windows Direct3D 9 interfaces. The optional texture loader dynamically resolves a D3DX9 image-loading function at runtime; no D3DX DLL is redistributed here.

## TexMod-compatible TPF format

The texture module implements compatibility with the observable TPF container/definition format and TexMod texture hashes. TexMod.exe, `tmldr.dll`, and `tmrls.dll` are not included.
