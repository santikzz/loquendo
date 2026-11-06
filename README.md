# jorge

The Loquendo TTS 6 "Jorge" Spanish voice from 2005, as a native command-line program. The original engine is a set of 32-bit x86 Windows DLLs. This project recompiles them to portable C, so the voice runs on Windows, Linux and macOS, on x64 and arm64.

```sh
jorge Hola, soy Jorge.
```

You need only a C compiler. The recompiled engine (`gen/`) and the voice files from the original installer (`voice/`) are in this repository.

## Usage

```sh
jorge Hola, soy Jorge.                    # speak the text, then exit
jorge -s hola.wav "Hola, soy Jorge."      # save a WAV file, no sound
```

All arguments that are not options become the text, so quotes are optional.

| Option | Default | Meaning |
|---|---|---|
| `-s FILE`, `--save FILE` | off | Save the audio as a WAV file (16 kHz, mono, 16-bit) and print its path. Nothing is played. |
| `--data DIR` | `voice` next to the binary | The voice folder. |
| `--trace` | off | Write the engine debug log to stderr. Use it two times to log each Windows API call. |

Exit codes: `0` success, `1` engine or audio error, `2` bad arguments. Errors go to stderr.

### Use from agents and scripts

The program waits until the audio ends, then exits. On Windows it has no console window, so a call from a background agent does not open a terminal.

```python
import subprocess
subprocess.run(["jorge", "Tarea terminada."], check=True)
```

When you type the command in cmd or PowerShell on Windows, the prompt returns at once and the voice speaks in the background. Error messages are not visible there. Scripts and agents that read stderr get them.

## Build

Each platform has one build script. The engine code is large, so the compile takes about one minute on a PC and longer on a Raspberry Pi. The binary goes next to `voice/`.

### Windows

1. Install [MinGW-w64 gcc](https://winlibs.com) (or MSYS2) and add it to `PATH`.
2. Run the build script:

   ```bat
   build.bat
   ```

The result is `jorge.exe`.

### Linux, Raspberry Pi

```sh
sudo apt install gcc alsa-utils
./build.sh
```

The result is `./jorge`. Playback uses `aplay` from `alsa-utils`. The compiler needs about 600 MB of free memory, so a board with 512 MB or less needs swap.

### macOS

```sh
xcode-select --install
./build.sh
```

The result is `./jorge`. Playback uses `afplay`, which is part of macOS. On macOS the program cannot find its own folder, so run it from the repository folder or give `--data`.

### Manual commands

The scripts run these commands. Use them if you cannot run a script.

```sh
# Linux, macOS
cc -std=c11 -O1 -w -Iruntime -Igen gen/*.c runtime/*.c -o jorge -lm -lpthread

# Windows, from Git Bash or MSYS2 (cmd does not expand *.c)
gcc -std=c11 -O1 -w -mwindows -Iruntime -Igen gen/*.c runtime/*.c -o jorge.exe -lm -lwinmm
```

`-mwindows` removes the console window. Without it, you get a console program that works the same way.

### CMake

CMake works with gcc, clang and Visual Studio 2022.

```sh
cmake -B build
cmake --build build --config Release -j8
```

The binary is `build/jorge`, or `build\Release\jorge.exe` with Visual Studio. Run it from the repository folder, or give `--data voice`.

### Tested platforms

| Platform | Status |
|---|---|
| Windows x64, MinGW gcc 13, `build.bat` | Tested |
| Linux x64 (WSL Ubuntu), gcc, `build.sh` | Tested |
| Linux arm64, cross gcc and qemu-aarch64 | Tested |
| Raspberry Pi (native build), macOS, CMake, Visual Studio | Not tested |

## How it works

- A static recompiler disassembled each function of the five engine DLLs and wrote one C function for it. The result is `gen/`. The DLL images are in `gen/tables.c` as data.
- The engine runs in one flat 32-bit memory arena, so the code works the same on 64-bit and ARM hosts.
- `runtime/` holds the small set of Windows API functions that the engine uses (files, registry, threads, heap), written in portable C.
- The recompiler and the emulator that verified its output are not in this repository. You need them only to regenerate `gen/`.

[CLAUDE.md](CLAUDE.md) has the full technical notes.

## Repository layout

| Path | Contents |
|---|---|
| `runtime/` | CPU state, memory, Windows API shims, OS layer, command-line program |
| `voice/` | The voice data and engine DLLs, from the `app/` folder of the installer. The engine also reads its DLLs as data. |
| `gen/` | The recompiled engine. Generated C, do not edit it by hand |
| `build.bat`, `build.sh` | Build scripts for Windows, and for Linux and macOS |

## Legal

Loquendo TTS is proprietary software. This repository contains Loquendo files in `voice/`. It is for private, personal use. Do not publish the repository or binaries built from it.
