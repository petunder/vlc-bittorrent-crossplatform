#Requires -RunAsAdministrator
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Say($m){ Write-Host ">>> $m" -ForegroundColor Cyan }
function Fail($m){ Write-Host "!!! $m" -ForegroundColor Red; Exit 1 }

# 1) Инструменты
Say "Устанавливаем Chocolatey и инструменты..."
if (-not (Get-Command choco -ErrorAction SilentlyContinue)) {
  Set-ExecutionPolicy Bypass -Scope Process -Force
  Invoke-Expression ((New-Object Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
}
choco install -y git cmake ninja 7zip vcpkg visualstudio2022buildtools

# 2) vcpkg + libtorrent
$env:VCPKG_ROOT = 'C:\vcpkg'
if (-not (Test-Path $env:VCPKG_ROOT)) {
  git clone https://github.com/Microsoft/vcpkg.git $env:VCPKG_ROOT
  & "$env:VCPKG_ROOT\bootstrap-vcpkg.bat"
}
Say "Устанавливаем libtorrent-rasterbar:x64-windows..."
& "$env:VCPKG_ROOT\vcpkg.exe" install libtorrent-rasterbar:x64-windows
& "$env:VCPKG_ROOT\vcpkg.exe" integrate install | Out-Null

# 3) VLC SDK из ZIP
$VlcVer='3.0.21'; 
$Zip="$env:TEMP\vlc-$VlcVer-win64.7z"
$Url="https://download.videolan.org/vlc/$VlcVer/win64/vlc-$VlcVer-win64.7z"
Say "Скачиваем и распаковываем VLC SDK ($VlcVer)..."
Invoke-WebRequest $Url -OutFile $Zip
$Portable='C:\opt\vlc-sdk'
if(Test-Path $Portable){ Remove-Item $Portable -Recurse -Force }
New-Item -Path $Portable -ItemType Directory | Out-Null
& "$env:ProgramFiles\7-Zip\7z.exe" x $Zip "-o$Portable" -y | Out-Null
$SdkDir=Get-ChildItem $Portable | Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName 'sdk' }
if(-not (Test-Path $SdkDir)){ Fail "SDK не найден в распакованном VLC." }

# 4) Путь установки плагина
$vlcReg = 'HKLM:\Software\VideoLAN\VLC'
$inst=(Get-ItemProperty $vlcReg -Name InstallDir -ErrorAction SilentlyContinue).InstallDir
if(-not $inst){
  $vlcReg='HKLM:\Software\Wow6432Node\VideoLAN\VLC'
  $inst=(Get-ItemProperty $vlcReg -Name InstallDir -ErrorAction SilentlyContinue).InstallDir
}
$Plugins = if($inst){ Join-Path $inst 'plugins' } else { Join-Path (Split-Path $SdkDir -Parent) 'plugins' }
New-Item -ItemType Directory -Force -Path $Plugins | Out-Null
Say "Плагин установится в: $Plugins"

# 5) Клонируем и собираем
$Repo='https://github.com/petunder/vlc-bittorrent-crossplatform.git'
$Work='C:\build\vlc-bittorrent'
if(Test-Path $Work){ Set-Location $Work; git pull } else { git clone $Repo $Work; Set-Location $Work }
$Bld=Join-Path $Work 'build'; New-Item -ItemType Directory -Force $Bld | Out-Null; Set-Location $Bld

Say "Конфигурация CMake..."
& cmake .. `
  -G "Ninja" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DCMAKE_PREFIX_PATH="$SdkDir"
Say "Сборка..."
& cmake --build . --config Release

# 6) Установка
$Plug = Get-ChildItem -Path . -Filter 'access_bittorrent_plugin.*.dll' -Recurse | Select-Object -First 1
if(-not $Plug){ Fail "Не найден собранный DLL-плагин." }
Say "Копирование в $Plugins..."
Copy-Item $Plug.FullName $Plugins -Force

# 7) Обновление кэша
$Cache = Join-Path $inst 'vlc-cache-gen.exe'
if(Test-Path $Cache){ Say "Обновляем кэш..."; & $Cache -f $Plugins | Out-Null }

Say "Готово! Перезапустите VLC."
