#!/usr/bin/env bash
set -euo pipefail

say(){ printf "\033[1;34m>>> %s\033[0m\n" "$*"; }
err(){ printf "\033[1;31m!!! ОШИБКА: %s\033[0m\n" "$*" >&2; exit 1; }

# Проверяем sudo (для установки зависимостей и копирования в системные каталоги)
if ! command -v sudo &>/dev/null; then
  err "Требуется sudo для установки зависимостей и копирования плагина."
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

# Находим скомпилированный плагин
PLUGIN="$(find src -maxdepth 1 -name 'libaccess_bittorrent_plugin.*.so' -o -name 'libaccess_bittorrent_plugin.so' | head -n1)"
[ -f "$PLUGIN" ] || err "Не найден файл плагина в сборке."

# Определяем способ установки VLC
if ! command -v vlc &>/dev/null; then
  err "Не удалось найти исполняемый файл vlc. Убедитесь, что VLC установлен."
fi

VLC_BIN="$(command -v vlc)"

# Теперь смотрим на сам путь к бинарнику, а не на readlink -f
if [[ "$VLC_BIN" == /snap/* ]]; then
  ### Snap-установка VLC ###
  say "Обнаружен VLC установленный через snap ($VLC_BIN)"

  DEST="$HOME/.local/lib/vlc/plugins/access"
  say "Создаём директорию плагинов для snap-VLC: $DEST"
  mkdir -p "$DEST"

  say "Копируем плагин в $DEST"
  install -m644 "$PLUGIN" "$DEST/"

  # Добавляем VLC_PLUGIN_PATH, если ещё не добавлен
  if ! grep -q "VLC_PLUGIN_PATH=.*$DEST" "$HOME/.profile"; then
    say "Добавляем VLC_PLUGIN_PATH в ~/.profile"
    printf "\n# added by vlc-bittorrent install_debian.sh\nexport VLC_PLUGIN_PATH=\"$DEST:\$VLC_PLUGIN_PATH\"\n" \
      >> "$HOME/.profile"
    say "Для применения изменений выполните: source ~/.profile или выйдите/войдите в систему"
  else
    say "VLC_PLUGIN_PATH уже добавлен в ~/.profile"
  fi

  say "Установка плагина завершена для snap-VLC."
  say "Запускайте VLC командой 'snap run vlc' — плагин будет подхвачен автоматически."

else
  ### Классическая (deb/ppa) установка VLC ###
  say "Обнаружен классический VLC ($VLC_BIN)"

  if PKG=$(pkg-config --variable=pluginsdir vlc-plugin 2>/dev/null); then
    BASE="$PKG"
  else
    ARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "x86_64-linux-gnu")
    BASE="/usr/lib/${ARCH}/vlc/plugins"
  fi
  DEST="$BASE/access"

  say "Создаём директорию плагинов: $DEST"
  sudo mkdir -p "$DEST"

  say "Копируем плагин в $DEST"
  sudo install -m644 "$PLUGIN" "$DEST/"

  say "Обновляем кэш плагинов (если доступно)…"
  if command -v vlc-cache-gen &>/dev/null; then
    sudo vlc-cache-gen -f "$BASE" || true
  fi

  say "Установка плагина завершена для классического VLC."
  say "Перезапустите VLC для применения изменений."
fi
