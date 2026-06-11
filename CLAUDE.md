# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

An OBS Studio output plugin (`raw-tcp-udp-output`) written in C that streams raw encoded video frames over TCP or UDP to a remote receiver. The companion tools are a Python playback server and a Lua control script for OBS scripting.

## Build

This project uses CMake and targets Windows (primary) and Linux. The CI builds OBS Studio v30.2.3 from source first, then links against it.

**Local build (Windows, requires OBS Studio already built/installed):**
```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The output is a plugin DLL installed to:
- `obs-plugins/64bit/raw-tcp-udp-output.dll`
- `data/obs-plugins/raw-tcp-udp-output/locale/*.ini`

See `.github/workflows/build-and-release.yml` for the exact CI sequence (builds OBS from source, then builds the plugin against it).

## Architecture

```
src/raw-tcp-udp-output.c     — OBS output module (C11)
tools/obs_raw_output_control.lua  — OBS scripting UI (Lua)
tools/raw_video_server.py    — Receiver / playback server (Python 3)
data/locale/en-US.ini        — UI strings
```

### Wire Protocol

Every encoded packet is wrapped in a 17-byte header:

| Bytes | Field |
|-------|-------|
| 0–3   | Magic word `RAWV` (0x52415756) |
| 4–7   | Payload size (big-endian uint32) |
| 8     | Frame type (1 = keyframe) |
| 9–16  | DTS (int64, big-endian) |

- **TCP**: records sent back-to-back on a persistent connection.
- **UDP**: records chunked into 1400-byte datagrams; no reliability or sequencing.

### Plugin (`src/raw-tcp-udp-output.c`)

Implements the OBS `obs_output_t` interface. Key callbacks: `raw_output_start`, `raw_output_stop`, `raw_encoded_packet`. Socket I/O is mutex-protected; cross-platform via `#ifdef _WIN32` / POSIX.

Connection URL format: `tcp://host:port` or `udp://host:port`.

### Lua Control Script (`tools/obs_raw_output_control.lua`)

OBS front-end script. Configures an x264 encoder (ultrafast/zerolatency preset), lets the user set bitrate/resolution (auto/base/custom), and calls the plugin output to start/stop. Enforces header repetition on the raw stream.

### Python Playback Server (`tools/raw_video_server.py`)

Listens on TCP or UDP, reads the 13-byte framing header, reassembles frames, and pipes to `ffplay` for zero-buffer low-latency playback. Supports H.264, HEVC, and AV1. Reports FPS/bitrate stats.

## Release Process

Tagging `v*` (e.g. `v0.1.0`) triggers the CI workflow which:
1. Compiles OBS Studio v30.2.3 and the plugin on `windows-latest`.
2. Packages the DLL, locale files, and tools into a `.zip`.
3. Publishes a GitHub Release (pre-release if the tag contains `rc` or `beta`).
