#!/usr/bin/env bash
set -euo pipefail

# ————— Утилиты для вывода —————
say(){ printf "\033[1;34m>>> %s\033[0m\n" "$*"; }
err(){ printf "\033[1;31m!!! ОШИБКА: %s\033[0m\n" "$*" >&2; exit 1; }

# ————— Определяем пользователя и домашний каталог —————
if [ -n "${SUDO_USER-}" ]; then
  ORIG_USER="$SUDO_USER"
  ORIG_HOME=$(getent passwd "$ORIG_USER" | cut -d: -f6)
else
  ORIG_USER=$(whoami)
  ORIG_HOME="$HOME"
fi

say "Устанавливаем плагин от имени пользователя: $ORIG_USER ($ORIG_HOME)"

# ————— Функция очистки предыдущих установок —————
clean_old(){
  say "Удаление старых плагинов vlc-bittorrent..."
  declare -a DIRS

  # Берём pluginsdir из pkg-config, если доступен
  if PKG=$(pkg-config --variable=pluginsdir vlc-plugin 2>/dev/null); then
    DIRS+=("$PKG")
  fi

  # Традиционные пути
  DIRS+=(
    "/usr/lib/vlc/plugins/access"
    "/usr/lib64/vlc/plugins/access"
    "/usr/lib/x86_64-linux-gnu/vlc/plugins/access"
    "$ORIG_HOME/.local/lib/vlc/plugins/access"
  )

  for d in "${DIRS[@]}"; do
    if [ -d "$d" ]; then
      say "  – очищаю $d"
      sudo rm -f "$d/libaccess_bittorrent_plugin"*.so || true
    fi
  done

  # Чистим VLC_PLUGIN_PATH в профиле
  PROFILE="$ORIG_HOME/.profile"
  if [ -f "$PROFILE" ]; then
    say "  – удаляю VLC_PLUGIN_PATH из $PROFILE"
    sudo sed -i '/# added by vlc-bittorrent install_debian.sh/,/+export VLC_PLUGIN_PATH/d' "$PROFILE" || true
  fi
}

clean_old

# ————— Проверяем VLC —————
if ! command -v vlc &>/dev/null; then
  err "Не найден vlc в PATH. Установите VLC перед запуском этого скрипта."
fi
VLC_BIN="$(command -v vlc)"
if [[ "$VLC_BIN" == /snap/* ]]; then
  err "Snap-версия VLC не поддерживает плагины. Пожалуйста, установите классическую версию (apt/flatpak и т.п.)."
fi
say "Обнаружен классический VLC: $VLC_BIN"

# ————— Устанавливаем зависимости для сборки —————
say "Устанавливаем зависимости для сборки плагина..."
sudo apt update
sudo apt install -y build-essential cmake pkg-config libvlc-dev libtorrent-rasterbar-dev

# ————— Сборка плагина —————
say "Собираем проект..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"

# Ищем скомпилированный .so
PLUGIN="$(find "$BUILD_DIR/src" -maxdepth 1 -type f -name 'libaccess_bittorrent_plugin*.so' | head -n1)"
[ -f "$PLUGIN" ] || err "Не найден скомпилированный плагин в $BUILD_DIR/src. Проверьте вывод make."

# ————— Подготовка директории установки —————
if PKG=$(pkg-config --variable=pluginsdir vlc-plugin 2>/dev/null); then
  BASE="$PKG"
else
  ARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "x86_64-linux-gnu")
  BASE="/usr/lib/${ARCH}/vlc/plugins"
fi
DEST="$BASE/access"

say "Создаём папку для плагинов: $DEST"
sudo mkdir -p "$DEST"

say "Копируем плагин в $DEST"
sudo install -m644 "$PLUGIN" "$DEST/"

# ————— Обновление кэша VLC (если доступно) —————
say "Обновляем кэш плагинов (если доступно)…"
if command -v vlc-cache-gen &>/dev/null; then
  sudo vlc-cache-gen -f "$BASE" || true
fi

say "Установка завершена. Перезапустите VLC под пользователем $ORIG_USER и проверьте доступность торрентов." 
