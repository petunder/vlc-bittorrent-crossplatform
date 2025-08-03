#!/usr/bin/env bash
set -euo pipefail

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

# Проверяем sudo (нужен для apt и копирования в системные каталоги)
if ! command -v sudo &>/dev/null; then
  err "Требуется sudo для установки зависимостей и копирования плагина."
fi

# ————— Устанавливаем зависимости —————
say "Устанавливаем системные пакеты..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
  build-essential git cmake pkg-config \
  libvlc-dev libvlccore-dev libtorrent-rasterbar-dev

# ————— Сборка плагина —————
WORKDIR="$(mktemp -d)"
trap 'rm -rf -- "$WORKDIR"' EXIT
say "Клонируем репозиторий в $WORKDIR"
git clone --depth 1 https://github.com/petunder/vlc-bittorrent-crossplatform.git "$WORKDIR" >/dev/null
cd "$WORKDIR"

say "Собираем плагин..."
mkdir -p build && cd build
cmake .. 
make -j"$(nproc)"

PLUGIN="$(find src -maxdepth 1 -name 'libaccess_bittorrent_plugin.*.so' -o -name 'libaccess_bittorrent_plugin.so' | head -n1)"
[ -f "$PLUGIN" ] || err "Не найден libaccess_bittorrent_plugin.so"

# ————— Определяем VLC —————
if ! command -v vlc &>/dev/null; then
  err "Не удалось найти исполняемый файл vlc в PATH."
fi
VLC_BIN="$(command -v vlc)"

# ————— Snap vs Classic —————
if [[ "$VLC_BIN" == /snap/* ]]; then
  say "Обнаружен snap-VLC ($VLC_BIN)"
  # Кладём в ~/.local/lib/vlc/plugins (уровнем выше — без /access)
  DEST="$ORIG_HOME/.local/lib/vlc/plugins"

  say "Создаём папку для плагинов: $DEST"
  sudo mkdir -p "$DEST"

  say "Устанавливаем права на каталог $DEST → $ORIG_USER"
  sudo chown -R "$ORIG_USER":"$ORIG_USER" "$DEST"

  say "Копируем плагин в $DEST"
  sudo install -o "$ORIG_USER" -g "$ORIG_USER" -m644 "$PLUGIN" "$DEST/"

  # Обновляем .profile у оригинального пользователя
  PROFILE="$ORIG_HOME/.profile"
  if ! sudo grep -q "VLC_PLUGIN_PATH=.*\$HOME/.local/lib/vlc/plugins" "$PROFILE"; then
    say "Добавляем VLC_PLUGIN_PATH в $PROFILE"
    printf "\n# added by vlc-bittorrent install_debian.sh\nexport VLC_PLUGIN_PATH=\"$DEST:\$VLC_PLUGIN_PATH\"\n" \
      | sudo tee -a "$PROFILE" >/dev/null
    sudo chown "$ORIG_USER":"$ORIG_USER" "$PROFILE"
    say "Для применения: su - $ORIG_USER или source $PROFILE"
  else
    say "VLC_PLUGIN_PATH уже есть в $PROFILE"
  fi

  say "Установка завершена для snap-VLC."
  say "Запускайте: snap run vlc"

else
  say "Обнаружен классический VLC ($VLC_BIN)"
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
fi
