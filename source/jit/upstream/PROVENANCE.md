# Vendored: VBA-GX THUMB trace-JIT

These files are copied **verbatim** from Visual Boy Advance GX:

* Repository: https://github.com/dborth/vbagx
* Commit:     `07ee4af4e4f19a3cf5f3d56cb74408857190102a` ("move glyphs back to memalign", 2026-09-02)
* Path:       `source/vba/gba/`
* Author:     Daryl Borth (JIT © 2026)
* Licence:    GNU GPL, no version stated: the files carry no licence
              header, the repository has no licence file, and its README
              says only "GPL licensed". A program that names no GPL
              version may be used under any version (GPL v2 §9, GPL v3
              §14), so these files are used under GPL v3, the licence
              the combined DiiS work is distributed under (repo root
              `LICENSE.txt`; every component in `licenses/THIRD_PARTY.md`).

Files:

| vendored file          | upstream file                    |
|------------------------|----------------------------------|
| `JIT.h`                | `source/vba/gba/JIT.h`           |
| `JITCache.{h,cpp}`     | `source/vba/gba/JITCache.{h,cpp}` |
| `JITPPCEmitter.h`      | `source/vba/gba/JITPPCEmitter.h` |
| `JITTrampoline.S`      | `source/vba/gba/JITTrampoline.S` |
| `JITCompiler.cpp`      | `source/vba/gba/JITCompiler.cpp` |
| `JITDifferential.{h,cpp}` | `source/vba/gba/JITDifferential.{h,cpp}` |
| `JITDebugStateLog.{h,cpp}` | `source/vba/gba/JITDebugStateLog.{h,cpp}` |
| `Profiler.{h,cpp}`     | `source/vba/gba/Profiler.{h,cpp}` |

**Nothing in this `upstream/` directory is compiled.** It is a frozen reference
copy so the DiiS port in `source/jit/` can be diffed against its
origin. The compiled port files (`jit_*.{h,cpp,S}` one level up) are derived
from these and carry their own headers noting the port.

See the repo root for the port plan.