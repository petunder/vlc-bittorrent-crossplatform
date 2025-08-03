#!/usr/bin/env bash
set -euo pipefail

# Вспомогательные функции
say(){ printf "\033[1;34m>>> %s\033[0m\n" "$*"; }
err(){ printf "\033[1;31m!!! ОШИБКА: %s\033[0m\n" "$*" >&2; exit 1; }

# ————— Определяем реального пользователя —————
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

  # путь из pkg-config, если доступен
  if PKG=$(pkg-config --variable=pluginsdir vlc-plugin 2>/dev/null); then
    DIRS+=("$PKG")
  fi

  # общие системные и пользовательские директории
  DIRS+=(
    "/usr/lib/vlc/plugins/access"
    "/usr/lib64/vlc/plugins/access"
    "/usr/lib/x86_64-linux-gnu/vlc/plugins/access"
    "$ORIG_HOME/.local/lib/vlc/plugins"
  )

  for d in "${DIRS[@]}"; do
    if [ -d "$d" ]; then
      say "  – очищаю $d"
      sudo rm -f "$d/libaccess_bittorrent_plugin"*.so || true
    fi
  done

  # чистим профиль от любых строк VLC_PLUGIN_PATH
  PROFILE="$ORIG_HOME/.profile"
  if [ -f "$PROFILE" ]; then
    say "  – удаляю VLC_PLUGIN_PATH из $PROFILE"
    sudo sed -i '/# added by vlc-bittorrent install_debian.sh/,/+export VLC_PLUGIN_PATH/d' "$PROFILE" || true
  fi
}

clean_old

# ————— Проверяем наличие VLC —————
if ! command -v vlc &>/dev/null; then
  err "Не найден vlc в PATH. Установите VLC перед запуском этого скрипта."
fi
VLC_BIN="$(command -v vlc)"

# ————— Отказываемся от snap-версии —————
if [[ "$VLC_BIN" == /snap/* ]]; then
  err "Snap-версия VLC не поддерживает плагины. Пожалуйста, установите классическую версию VLC (apt/flatpak и т.п.)."
fi

say "Обнаружен классический VLC: $VLC_BIN"

# ————— Находим собранный плагин —————
PLUGIN="$(find src -maxdepth 1 -name 'libaccess_bittorrent_plugin.*.so' -o -name 'libaccess_bittorrent_plugin.so' | head -n1)"
[ -f "$PLUGIN" ] || err "Не найден libaccess_bittorrent_plugin.so в каталоге src. Соберите проект перед установкой."

# ————— Определяем директорию плагинов —————
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

say "Обновляем кэш плагинов (если доступно)…"
if command -v vlc-cache-gen &>/dev/null; then
  sudo vlc-cache-gen -f "$BASE" || true
fi

say "Установка завершена для классического VLC."
say "Перезапустите VLC под пользователем $ORIG_USER для проверки."
