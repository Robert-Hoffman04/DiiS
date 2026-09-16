# Vendored: VBA-GX THUMB trace-JIT

These files are copied **verbatim** from Visual Boy Advance GX:

* Repository: https://github.com/dborth/vbagx
* Commit:     `07ee4af4e4f19a3cf5f3d56cb74408857190102a` ("move glyphs back to memalign", 2026-09-02)
* Path:       `source/vba/gba/`
* Author:     Daryl Borth (JIT © 2026)
* Licence:    GNU GPL v2 (VBA-GX / VBA-M's own banner text specifies
              "Version 2.0" only, with no "or later" grant -- see
              https://github.com/visualboyadvance-m). DeSmuMEWii's own files
              are licensed "GPL v2 or later", so the combined work is
              distributed under plain GPLv2 (the version both sides permit);
              this is also why the repo root `LICENSE.txt` carries the GPLv2
              text specifically rather than a later version.

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
copy so the DeSmuMEWii port in `source/jit/` can be diffed against its
origin. The compiled port files (`jit_*.{h,cpp,S}` one level up) are derived
from these and carry their own headers noting the port.

See the repo root for the port plan.