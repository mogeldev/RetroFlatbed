<img src="assets/retroflatbed.png" width="96" align="right" alt="RetroFlatbed icon">

# RetroFlatbed

**Use your Canon CanoScan LiDE 20 scanner on Windows 11 again.**

RetroFlatbed adds WIA and TWAIN support for the LiDE 20 on 64-bit Windows. Version **0.1.0**.

## Install

1. Download **[RetroFlatbedSetup-0.1.0.exe](https://github.com/mogeldev/RetroFlatbed/releases/latest)** from *Assets*.
2. Start it and approve the Windows permission prompt. If SmartScreen appears, choose *More info* → *Run anyway*.
3. Choose **Both** if you are unsure, then click **Install**:

   | Option | Use it with |
   |---|---|
   | **WIA** | Windows Scan, Paint, Windows Fax and Scan |
   | **TWAIN** | NAPS2 and other TWAIN applications |
   | **Both** | Everything |

4. If asked, unplug and reconnect the scanner. Wait for **Scanner ready**.

The installer can switch modes later and is also available under *Settings → Apps → Installed apps*.

## Scan

* In Windows Scan or Paint, select **Canon CanoScan LiDE 20**.
* In NAPS2, create a profile using the **TWAIN** driver and select the same scanner.
* Place the page face down on the glass.
* The first scan may take up to a minute while the lamp warms up and calibration runs.
* Only one scan application can use the scanner at a time.

## Troubleshooting

* Not listed? Unplug/reconnect the USB cable and try a direct USB port instead of a hub.
* NAPS2 cannot find it? Install **TWAIN** or **Both**, then restart NAPS2.
* The installer asks for a replug? Do that before scanning.
* Still stuck? [Open an issue](https://github.com/mogeldev/RetroFlatbed/issues) and include the installer log.

## Uninstall

Use *Settings → Apps → Installed apps → RetroFlatbed → Uninstall*, or start the installer and click **Uninstall**. Then unplug and reconnect the scanner.

## Windows security note

This free project is not commercially code-signed, so SmartScreen may warn. The installer adds the local certificate needed for Windows to accept the driver and removes it again during uninstall. The scanner uses Windows' own WinUSB driver; RetroFlatbed installs no kernel-mode code. See the [release page](https://github.com/mogeldev/RetroFlatbed/releases/latest) for the download hash.

> RetroFlatbed is independent and is not affiliated with Canon or the SANE project. Canon, CanoScan and LiDE are Canon trademarks. The scanner engine is derived from SANE's GPL `plustek` backend.

---

# For developers

RetroFlatbed is a Windows 11 x64 user-space driver for the Canon CanoScan LiDE 20 (`04a9:220d`). It uses Windows WinUSB and a patched port of SANE's `plustek` backend. It includes:

* WIA 2.0 driver (`RetroFlatbedWIA.dll`)
* 32- and 64-bit TWAIN data source (`RetroFlatbed.ds`)
* CLI scanner and test tool
* Bundled TWAIN DSM 2.5.1
* GUI installer/uninstaller

## Build

Requires Visual Studio 2022 Build Tools (C++) and the Windows 10/11 SDK:

```bat
build.bat
build.bat x86
powershell -ExecutionPolicy Bypass -File make-installer.ps1
```

The installer is written to `dist\RetroFlatbedSetup.exe`. Build output is intentionally ignored by Git.

## Developer install

The GUI installer is recommended. For a developer install from a local build:

```powershell
powershell -ExecutionPolicy Bypass -File driver\install.ps1
powershell -ExecutionPolicy Bypass -File driver\uninstall.ps1
```

`install.ps1` supports `-SkipTwain`, `-SkipDsm`, `-WinUsbOnly` and `-TwainOnly`.

The installer also supports:

```text
RetroFlatbedSetup.exe /install:wia|twain|both /quiet [/log:file]
RetroFlatbedSetup.exe /uninstall /quiet
```

The self-signed driver certificate is imported into LocalMachine `Root` and `TrustedPublisher`; uninstall removes it. The installer EXE is unsigned. A download hash verifies file integrity, not safety.

Close scan applications before updating the driver and reconnect the scanner if needed. TWAIN applications only see sources of the same bitness; both `twain_32` and `twain_64` are installed.

## CLI and simulation

```bat
build\retroflatbed-scan.exe --list
build\retroflatbed-scan.exe -m color -r 300 -o page.ppm
build\retroflatbed-scan.exe -m gray -d 16 -r 600 -x 10 -y 10 -w 100 -h 150 -o crop.pgm
```

Modes are `color`, `gray` and `lineart`; resolutions are 50–1200 dpi. For hardware-free tests:

```bat
set RETROFLATBED_SIMULATE=1
build\twain_test.exe -ds build\RetroFlatbed.ds -x memory -m gray -d 16 -r 150 -o out.pgm
```

`RETROFLATBED_DEBUG=<0..30>` enables logging and `RETROFLATBED_LOG=<file>` redirects it. Calibration data is stored under `%LOCALAPPDATA%\RetroFlatbed`.

## Source layout

| Path | Purpose |
|---|---|
| `src/` | WinUSB transport, core API and TWAIN source |
| `wia/` | WIA mini-driver |
| `third_party/sane/` | Patched SANE `plustek` engine |
| `third_party/twain-dsm/` | Official TWAIN DSM binaries and licences |
| `driver/` | INF and developer install scripts |
| `installer/` | GUI installer resources and source |
| `tools/` | CLI, tests and icon generator |

## License and credits

The scanner engine comes from the SANE project and its `plustek` backend (Gerhard Jaeger and contributors). GPL-2.0-or-later with the SANE linking exception; the bundled TWAIN DSM is LGPL. See `LICENSE` and `NOTICE.md`.
