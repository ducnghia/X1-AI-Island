# X1 AI Island v0.2

A tiny native Windows overlay made for the ThinkPad X1 Extreme Gen 4 + NVIDIA RTX 3080 Laptop GPU use case.

## What v0.2 shows

- NVIDIA GPU utilization
- Dedicated VRAM used / total
- GPU temperature
- GPU power draw when the driver exposes it
- NVIDIA memory-engine utilization in expanded view
- NVIDIA performance state (`P0` through `P8` when exposed by the driver)
- A thin rounded status border driven by the higher of VRAM usage or dGPU load:
  - NVIDIA-green breathing pulse below 50%
  - yellow-dominant RGB pulse from 50% through 79%
  - red-biased RGB breathing pulse from 80% upward
- GPU wins ties; expanded view identifies whether GPU load or VRAM fill is driving the border
- The `RTX 3080` text is fixed NVIDIA green below 50% GPU load, then uses the yellow/red warning animations based only on GPU load
- Embedded NVIDIA logo image

It deliberately queries NVIDIA telemetry through `nvml.dll` instead of treating Windows "GPU 0/GPU 1" as a stable identity. It prefers a device whose NVIDIA name contains `RTX 3080`, otherwise it uses the first NVIDIA device.

## Why this architecture

The UI is plain Win32/GDI. It does not create a Direct3D rendering context on the RTX just to draw the overlay. NVIDIA is queried only for telemetry.

No CUDA Toolkit, Python, .NET, Electron, LibreHardwareMonitor, installer, or Windows service is required. `nvml.dll` normally comes with the NVIDIA display driver.

## Build

Recommended:
1. Install Visual Studio 2022 Build Tools with **Desktop development with C++**.
2. Open **x64 Native Tools Command Prompt for VS 2022**.
3. `cd` into this folder.
4. Run `build.bat`.
5. Run `X1-AI-Island.exe`.

## Controls

- Default position: top-center of the primary screen.
- Drag: move Island.
- Double-click: expand/collapse.
- **Ctrl+D**: hide/show globally (default).
- Right-click: expand/collapse, hide, change the global shortcut, or exit.
- Hover for one second: hide the Island for five seconds so content beneath it can be seen, then return automatically.
- Refresh interval: 1 second.

## Verify

Run `nvidia-smi` beside the Island and compare GPU utilization, VRAM, temperature, and power.

## v0.2 candidates

- CPU + RAM + battery
- tray icon and Start with Windows
- selectable NVIDIA device
- compact/LLM layouts
- hide when fullscreen
- position persistence
