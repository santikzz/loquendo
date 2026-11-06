# jorge-native

The Loquendo TTS 6 engine (5 x86 Windows DLLs, 2005) recompiled to portable C, so the "Jorge" Spanish voice runs natively on any OS and architecture. The user's main target is a Raspberry Pi (linux-arm64).

## Layout

| Path | Contents | Edit? |
|---|---|---|
| `gen/` | Generated C: one `f_XXXXXXXX(CPU *c)` per guest function, plus `tables.c` (images, exports, imports, cp1252 tables). In git, so a build needs only a C compiler | Never by hand. Regenerate it. |
| `runtime/recomp.h` | CPU state, guest memory access, x87 helpers. Shared by `gen/` and `runtime/` | Yes |
| `runtime/runtime.c` | Guest arena, heap, threads, call dispatch, x87 helpers | Yes |
| `runtime/win32.c` | The Win32 API the engine imports (shim table at the bottom) | Yes |
| `runtime/platform.c` | The only OS-specific code: lock, condition variable, threads, clock, directory listing, UTF-8 argv, exe folder, audio playback | Yes |
| `runtime/main.c` | The `jorge` CLI and the engine call sequence | Yes |
| `tools/gen.py` | The recompiler: x86 to C (capstone + IDA JSON) | Yes |
| `tools/ida_export.py` | Exports functions, blocks and switch tables with IDA 9 idalib | Yes |
| `tools/loqemu.py`, `tools/say.py` | Oracle: runs the original DLLs in Unicorn | Only to fix the oracle |
| `tools/check.py` | Parity check, native output against the oracle | Yes |
| `ida/` | IDA export JSON, one file per DLL | Regenerate only when the DLLs change |
| `voice/` | Voice data and the original DLLs, from the installer's `app/` folder. In git as binary (`.gitattributes`) | No |
| `build.bat`, `build.sh` | Build scripts (Windows; Linux and macOS). Compile only | Yes |

## Rules

- Never edit `gen/`. Fix the generator (`tools/gen.py`) or the runtime, then regenerate.
- After any change to `tools/gen.py` or `runtime/`, save a WAV with `-s` and compare it with one saved before the change. `tools/check.py` fails by design since the license patch, because the oracle speaks the notice.
- `tools/` and `ida/` are not in git (`.gitignore`). They exist only on the local disk, so back them up. After you regenerate `gen/`, commit it.
- Never commit binaries. The repository holds proprietary Loquendo code and data (`gen/`, `voice/`), so it must stay private.
- License bypass implemented (2026-11-01): `tools/gen.py` patches three `__cdecl` functions (f_10001460, f_100272C0, f_10028B70) to return success and properly simulate `ret` (pop return address via `esp += 4`). The case-15 handler in f_10019010 swaps the notice text pointer with user text. Parity check against oracle no longer applies since output differs intentionally.
- Do not use `rm -rf`. The user denies it. Let tools overwrite their output.

## Commands

```sh
# regenerate C (needs capstone and pefile; IDA only if ida/ must change)
python tools/ida_export.py voice/LoqTTS6.dll voice/LoqTTS6_util.dll voice/LoqSpanish6.2.dll voice/loq210.dll voice/LoqAudioFile.dll
#   writes voice/*.json; move them to ida/
python tools/gen.py voice ida gen

# build without CMake (what is tested); Windows cmd does not expand *.c, so use build.bat there
gcc -std=c11 -O1 -w -Iruntime -Igen gen/*.c runtime/*.c -o build/jorge -lm -lpthread
build.bat

# speak, or save a WAV with -s (-o is an alias); build.bat links with -mwindows (no console) and winmm
./build/jorge --data voice Hola, soy Jorge.
./build/jorge --data voice -s out.wav "Hola, soy Jorge."
```

Tested: Windows x64 (MinGW gcc 13), Linux x64 (WSL Ubuntu), linux-arm64 (cross gcc and qemu-aarch64 in a Podman container). All give byte-identical WAVs. CMake and MSVC are not tested.

## How it works

- Guest memory is one flat 32-bit arena `M` (400 MB, lazily committed). Layout is in `runtime/runtime.h`: shims at `0x10000`, TEBs at `0x500000`, stacks at `0x1000000`, DLL images at `0x10000000` + 1 MB each, heap at `0x11000000`.
- Inside a generated function, registers and flags (`cf zf sf of`) are C locals. `SPILL` and `RELOAD` move them to and from `CPU` around calls. Every guest function and shim is entered with the return address on the guest stack, like on x86.
- Indirect calls go through `call_indirect`, which uses a binary search over `func_addr`. Shim addresses (`SHIM_BASE + 16*i`) dispatch to `runtime/win32.c`.
- x87 is a stack of doubles. Threads start with `fcw = 0x27F` (53-bit precision, as on Windows), so double arithmetic matches.
- Guest threads are real host threads. One global lock and condition variable guard every kernel object, the heap and the registry.
- Unsupported instructions become `trap()` calls. They exist only in the dongle license code (`LoqTTS6.dll` around `0x10072000` to `0x10074400`) and in dead CRT code.

## Engine facts found by tracing

- Call sequence: `ttsNewSession` → `ttsNewInstance` → `ttsNewVoice(.., "Jorge", 16000, "loq210")` → `ttsSetInstanceParam("ProcessingMode", "blocking")` → `ttsSetAudio("LoqAudioFile", path)` → `ttsRead(inst, text, 0,0,0,0)`. Text is cp1252.
- The engine reads its DLL files at run time (all of `LoqTTS6.dll`, then 4 KB at `0x7A588`). The DLLs in `voice/` must match the ones in `gen/`.
- The engine reads `DataPath` and `LibraryPath` from `HKLM\Software\Loquendo\LTTS` and its `default.session` subkey. The runtime maps both to `c:\loq`, which is the `--data` folder. `c:\out` maps to the output folder. No other path is reachable.
- `FindFirstFileA` patterns mix `\` and `/` (`c:\loq\SpanishEs/Jorge/Jorge*.bin`).
- The CRT `memcpy` uses jump tables that IDA does not mark as switches. Some are indexed from 1. `gen.py` `jump_table()` recovers them.
- IDA misses some callback functions. `gen.py` `pointer_targets()` adds code addresses that are used as immediates or stored in data sections.
- When a recompiled build traps with "indirect call to unknown address X", check how `X` is referenced (callback, jump table) and extend the generator. Do not add special cases to the runtime.
