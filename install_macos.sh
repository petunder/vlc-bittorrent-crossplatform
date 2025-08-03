#!/usr/bin/env bash
set -euo pipefail

say(){ printf "\033[1;34m>>> %s\033[0m\n" "$*"; }
err(){ printf "\033[1;31m!!! ОШИБКА: %s\033[0m\n" "$*" >&2; exit 1; }

# 1) Homebrew-зависимости
if ! command -v brew &>/dev/null; then
  say "Устанавливаю Homebrew..."
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi
say "Обновление brew и установка git, cmake, pkg-config, libtorrent-rasterbar..."
brew update
brew install git cmake pkg-config libtorrent-rasterbar

# 2) VLC.app
VLC_APP="/Applications/VLC.app"
if [[ ! -d "$VLC_APP" ]]; then
  say "Устанавливаю VLC.app…"
  brew install --cask vlc
fi

# 3) Форк
WORK="$HOME/src/vlc-bittorrent"
mkdir -p "$(dirname "$WORK")"
if [[ -d "$WORK/.git" ]]; then
  say "Обновление репозитория…"
  git -C "$WORK" pull --ff-only
else
  say "Клонирование репозитория…"
  git clone --depth 1 https://github.com/petunder/vlc-bittorrent-crossplatform.git "$WORK"
fi
cd "$WORK"

# 4) Извлечение SDK
SDK="$WORK/vlc-sdk"
rm -rf "$SDK"
mkdir -p "$SDK/include" "$SDK/lib"
cp -R "${VLC_APP}/Contents/MacOS/include/." "$SDK/include/" 2>/dev/null || true
cp -R "${VLC_APP}/Contents/MacOS/lib/."     "$SDK/lib/"     2>/dev/null || true

# 5) Сборка CMake
say "Создаём папку build…"
mkdir -p build && cd build
say "Конфигурация CMake..."
cmake .. -DCMAKE_PREFIX_PATH="$SDK"
say "Сборка…"
make -j"$(sysctl -n hw.ncpu)"

# 6) Копирование плагина
PLUGIN=$(find src -maxdepth 1 -name "libaccess_bittorrent_plugin.*.so" -print -quit)
[[ -n "$PLUGIN" ]] || err "Плагин не найден."
DEST="${VLC_APP}/Contents/MacOS/plugins"
say "Установка плагина в $DEST..."
sudo mkdir -p "$DEST"
sudo install -m755 "$PLUGIN" "$DEST/"

# 7) Обновление кэша
CACHE="${VLC_APP}/Contents/MacOS/vlc-cache-gen"
if [[ -x "$CACHE" ]]; then
  say "Обновление кэша…"
  sudo "$CACHE" -f "$DEST" || true
fi

say "Готово! Перезапустите VLC."
