# X1 AI Island v1.0.1

A tiny native Windows overlay made for the ThinkPad X1 Extreme Gen 4 + NVIDIA RTX 3080 Laptop GPU use case.

## What v1.0.1 shows

- NVIDIA GPU utilization
- Dedicated VRAM used / total
- GPU temperature
- GPU power draw when the driver exposes it
- NVIDIA performance state (`P0` through `P8` when exposed by the driver)
- Fan 1 and Fan 2 RPM in both compact and expanded views through `X1FanService`
- A thin rounded status border driven by the higher of VRAM usage or dGPU load:
  - NVIDIA-green breathing pulse below 50%
  - yellow-dominant RGB pulse from 50% through 79%
  - red-biased RGB breathing pulse from 80% upward
- GPU wins ties when GPU load and VRAM fill are equal
- The `RTX 3080` text is fixed NVIDIA green below 50% GPU load, then uses the yellow/red warning animations based only on GPU load
- Embedded NVIDIA logo image

It deliberately queries NVIDIA telemetry through `nvml.dll` instead of treating Windows "GPU 0/GPU 1" as a stable identity. It prefers a device whose NVIDIA name contains `RTX 3080`, otherwise it uses the first NVIDIA device.

## Why this architecture

The UI is plain Win32/GDI. It does not create a Direct3D rendering context on the RTX just to draw the overlay. NVIDIA is queried only for telemetry.

No CUDA Toolkit, Python, .NET, Electron, or LibreHardwareMonitor is required. `nvml.dll` normally comes with the NVIDIA display driver.

## Fan telemetry service

`X1FanService.exe` is a LocalSystem service and a small fan-mode controller for
the ThinkPad X1 Extreme Gen 4 machine type `20Y6`. It reads both tachometers
through the open-source PawnIO driver and publishes telemetry to the Island.

- It always starts in **BIOS Auto**, the default and safest mode.
- **Cool** starts both fans earlier.
- **Aggressive** starts both fans earlier and at higher EC levels.
- At high temperature, custom modes return control to BIOS.
- Stop, shutdown, and normal service exit also return control to BIOS.
- It restores the fan selector to Fan 1 after each sample.
- Missing service/driver data appears as `N/A`; the Island continues normally.

Install PawnIO 2.2.0 first, build the project, then run
`install_fan_service.bat` as Administrator. Use
`uninstall_fan_service.bat` to remove only X1FanService.

Run `X1FanService.exe` normally, or use the **Fan Control** submenu in the
Island's right-click menu, to choose BIOS Auto, Cool, or Aggressive directly.

Press **Ctrl+Shift+F** to open the Fan Control submenu directly, then use the
arrow keys and Enter. Compact mode stays unchanged in BIOS Auto, shows a fixed
NVIDIA-green `COOL` badge in Cool mode, and shows an animated yellow/red
`AGGR` badge in Aggressive mode.

## Build

Recommended:
1. Install Visual Studio 2022 Build Tools with **Desktop development with C++**.
2. Open **x64 Native Tools Command Prompt for VS 2022**.
3. `cd` into this folder.
4. Run `build.bat`.
5. Optionally install the fan service.
6. Run `X1-AI-Island.exe`.

## Controls

- Default position: top-center of the primary screen.
- Drag: move Island.
- Double-click: expand/collapse.
- **Ctrl+D**: hide/show globally (default).
- Right-click: open Fan Control, expand/collapse, hide, change the global
  shortcut, view About information, or exit.
- The Island uses 85% window opacity to soften the dark background while
  keeping compact telemetry and the animated border easy to read.
- Animation runs at a lightweight 10 FPS and pauses while the Island is hidden.
- Telemetry text is cached and rebuilt only when the one-second readings update.
- Expanded view is intentionally concise: GPU status, VRAM use, fan mode, and
  both fan RPM values. Its aligned columns use explicit labels such as
  `GPU Load` and `Performance State`. It omits duplicate border and
  memory-engine details.
- Hover for one second: hide the Island for five seconds so content beneath it can be seen, then return automatically.
- Refresh interval: 1 second.

## Verify

Run `nvidia-smi` beside the Island and compare GPU utilization, VRAM, temperature, and power.

## Version

Version **1.0.1** is a visual-polish update to the feature-complete v1.0
baseline. The centered, compact About dialog identifies the app as a
`Local LLM AI Workstation Monitor`.
