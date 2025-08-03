# VLC BitTorrent crossplatform plugin (Fork)

**This is a fork of the original `johang/vlc-bittorrent` repository.** Unfortunately, the original project has not been maintained for some time and its build process no longer works reliably on Windows or macOS.

A minimal, cross-platform refactoring has been applied to restore build compatibility across Linux, Windows, and macOS using CMake and platform-specific helper scripts.

---

## What is this?

With **vlc-bittorrent**, you can open a **.torrent** file or **magnet link** with VLC and stream any media that it contains.

---

## Features

* **Linux (Debian/Ubuntu)**: builds via CMake using system `libvlc` and `libtorrent-rasterbar` packages.
* **Windows (10/11)**: builds via CMake + vcpkg + Visual Studio Build Tools; locates VLC SDK from a portable archive.
* **macOS**: builds via CMake using headers and libraries extracted from the VLC.app bundle.

---

## Example Usage

```bash
$ vlc video.torrent
$ vlc http://example.com/video.torrent
$ vlc https://example.com/video.torrent
$ vlc ftp://example.com/video.torrent
$ vlc "magnet:?xt=urn:btih:...&dn=...&tr=..."
$ vlc "magnet://?xt=urn:btih:...&dn=...&tr=..."
```

---

## Installation Scripts

The root of this repository contains one-click installer scripts:

1. Clone your fork:

   ```bash
   git clone https://github.com/petunder/vlc-bittorrent-crossplatform.git
   cd vlc-bittorrent-crossplatform
   ```

2. Run the appropriate script:

   * **Linux (Debian/Ubuntu)**

     ```bash
     sudo ./install_debian.sh
     ```
   * **Windows (PowerShell, administrator)**

     ```powershell
     .\install_windows.ps1
     ```
   * **macOS**

     ```bash
     sudo ./install_macos.sh
     ```

Each script installs dependencies, builds the plugin, and places it in your VLC plugins directory.

---

## Manual Build with CMake

If you prefer manual control, use these steps:

```bash
mkdir build && cd build
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake \  # Windows only
  -DCMAKE_PREFIX_PATH=[path/to/vlc-sdk]
cmake --build . --config Release
```

Adjust flags for your platform as needed.

---

## FAQ

### Does it upload/share/seed while playing?

Yes. It works as a regular Bittorrent client. It will upload as long as it's playing.

### Does it work on Windows, macOS, Android, iOS, etc.?

The original author only tested on Linux, but this fork restores support on Windows and macOS. Android/iOS are not officially supported, but patches are welcome.

---

## Donate

If you find this project useful and would like to support maintenance costs, please consider donating.

Thank you for your support!

