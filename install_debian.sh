#!/usr/bin/env bash
set -euo pipefail

say(){ printf "\033[1;34m>>> %s\033[0m\n" "$*"; }
err(){ printf "\033[1;31m!!! ОШИБКА: %s\033[0m\n" "$*" >&2; exit 1; }

# Проверяем sudo
if ! command -v sudo &>/dev/null; then
  err "Требуется sudo для установки зависимостей."
fi

say "Устанавливаем системные пакеты..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
  build-essential git cmake pkg-config \
  libvlc-dev libvlccore-dev libtorrent-rasterbar-dev

# Клонируем форк
WORKDIR="$(mktemp -d)"
trap 'rm -rf -- "$WORKDIR"' EXIT
say "Клонирование репозитория в $WORKDIR"
git clone --depth 1 https://github.com/petunder/vlc-bittorrent-crossplatform.git "$WORKDIR" >/dev/null
cd "$WORKDIR"

# Сборка через CMake
say "Создаём папку сборки..."
mkdir -p build && cd build
say "Конфигурация CMake..."
cmake .. 
say "Сборка…"
make -j"$(nproc)"

# Копирование плагина
say "Определяем каталог плагинов VLC..."
if PKG=$(pkg-config --variable=pluginsdir vlc-plugin 2>/dev/null); then
  DEST="$PKG"
else
  ARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "x86_64-linux-gnu")
  DEST="/usr/lib/${ARCH}/vlc/plugins"
fi
sudo mkdir -p "$DEST"

PLUGIN=$(find src -maxdepth 1 -name "libaccess_bittorrent_plugin.*.so" -o -name "libaccess_bittorrent_plugin.so" | head -n1)
[ -f "$PLUGIN" ] || err "Не найден файл плагина в сборке."

say "Установка плагина в $DEST..."
sudo install -m644 "$PLUGIN" "$DEST/"

say "Обновляем кэш плагинов (если есть)…"
if command -v vlc-cache-gen &>/dev/null; then
  sudo vlc-cache-gen -f "$DEST" || true
fi

say "Готово! Перезапустите VLC."
