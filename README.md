# Moonlight-XP 🌙

A native, lightweight **Moonlight (GameStream / Sunshine / Wolf)** client written in pure C for **Windows XP (NT 5.1 / 32-bit x86)** and later versions of Windows.

Moonlight-XP provides a period-accurate native Win32 GUI while featuring modern networking and video streaming capabilities: modern Mutual TLS 1.2/1.3 authentication, hardware-accelerated Direct3D 9 video rendering with pure GDI `StretchDIBits` fallback, DirectSound/waveOut audio, and low-latency input capture.

---

## ✨ Features

- **Native Windows XP Target**: Fully compatible with Windows XP SP3, Windows XP x64 Edition, Windows Server 2003, and later Windows versions (Vista / 7 / 8 / 10 / 11).
- **Zero Vista+ CRT Dependencies**: Custom XP compatibility layer replacing Vista+ MSVCRT functions (`_vsnprintf_s`, `strtok_s`, `mbstowcs_s`, `wcstombs_s`).
- **Mutual TLS Handshake (mTLS)**: Embedded mbedTLS crypto engine supporting TLS 1.2 and TLS 1.3 ECDHE/RSA certificates and 4-phase pairing with Sunshine and Wolf (Games-on-Whales).
- **Dual Video Renderers**:
  - **Direct3D 9**: Hardware YV12/NV12 offscreen surface rendering with bicubic scaling.
  - **GDI Fallback**: Software YUV420P $\to$ RGB conversion and `StretchDIBits` for VMs without 3D acceleration.
- **Resilient Audio Subsystem**:
  - **DirectSound**: Low-latency circular buffer playback.
  - **waveOut**: Universal Win32 audio fallback.
  - **Null Audio Sink**: Seamless fallback if the VM or system lacks an audio device.
- **Low-Latency Input & Gamepad Support**:
  - Raw mouse delta tracking and absolute coordinate synchronization.
  - Full keyboard mapping using standard Win32 Virtual Key codes with modifier support.
  - Dynamic XInput gamepad polling.

---

## ⌨️ Shortcuts & Hotkeys

| Shortcut | Description |
|---|---|
| `Ctrl + Alt + Shift + Z` | Toggle mouse capture / cursor lock mode |
| `Ctrl + Alt + Shift + Q` | Disconnect and exit active stream |

---

## 🛠️ Building from Source

Moonlight-XP is compiled using the `i686-w64-mingw32` toolchain targeting the Windows subsystem `5.1` (Windows XP).

### Prerequisites
- Linux or Windows (WSL2 / MSYS2)
- CMake $\ge$ 3.10 & Ninja
- LLVM-MinGW (`i686-w64-mingw32`)

### Build Commands

```bash
# Clone the repository
git clone https://github.com/<your-username>/moonlight-xp.git
cd moonlight-xp

# Configure with CMake using the XP toolchain
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-xp-i686.cmake

# Compile MoonlightXP
ninja -C build

# Strip debug symbols for minimal binary size (~4MB standalone)
llvm-strip -o dist/MoonlightXP.exe build/MoonlightXP.exe
```

The resulting `dist/MoonlightXP.exe` is a completely standalone, static executable with no external DLL requirements beyond standard Windows XP system libraries (`kernel32.dll`, `user32.dll`, `gdi32.dll`, `ws2_32.dll`, `winmm.dll`, `comctl32.dll`).

---

## 📦 Project Structure

```
moonlight-xp/
├── CMakeLists.txt              # CMake build configuration
├── toolchain-xp-i686.cmake     # Target toolchain definition for Windows XP (subsystem 5.1)
├── src/
│   ├── main.c                  # Win32 GUI, configuration persistence, and session controller
│   ├── http_client.c           # HTTP/HTTPS engine with mbedTLS mTLS support
│   ├── nv_pairing.c            # 4-Phase GameStream pairing and XML metadata parser
│   ├── video_renderer.c        # Direct3D 9 & GDI video rendering pipeline (FFmpeg H.264)
│   ├── audio_renderer.c        # DirectSound, waveOut, and Opus multi-stream audio pipeline
│   ├── input_handler.c         # Keyboard, mouse capture, and XInput gamepad dispatcher
│   ├── xp_compat.c             # MSVCRT Vista+ shim functions for NT 5.1
│   ├── manifest.xml            # Common Controls v6 (Visual Styles / Luna) manifest
│   └── resources.rc            # Application resources
├── sysroot/                    # Precompiled static libraries and headers for NT 5.1
└── third_party/                # moonlight-common-c core engine
```

---

## 📜 License

This project is licensed under the [GNU General Public License v3.0](LICENSE) matching the Moonlight GameStream project.
