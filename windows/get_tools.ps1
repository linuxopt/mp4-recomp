# Fetches the pinned Windows toolchain into the folders build.bat expects, checking each download's SHA-256:
#   gcc\Library\           winlibs GCC 14.2.0 (UCRT, POSIX threads, x64)
#   python\                Python 3.12 embeddable
#   third_party\glslang\   glslang 16.6.0 (ScarletNX's SPIR-V shaders)
#   redist\                SDL2.dll and zlib1.dll, shipped beside the game EXE
# Downloads are kept in _dl\; a folder that is already there is left as it is.
#   powershell -ExecutionPolicy Bypass -File windows\get_tools.ps1
#
# The pins and this layout come from PlagueDrRiess's Windows build of this project (PlagueDrRiess/salsburysteaks).
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is many times slower with its progress bar
$root = Split-Path -Parent $PSScriptRoot
$dl = Join-Path $root '_dl'
New-Item -ItemType Directory -Force $dl | Out-Null

$tools = @(
    @{ name = 'gcc.zip'; sha = '88868d745b807f083a117ff69348d8bc021ad7389aa503379dbed1866efcaeb9'
       url = 'https://github.com/brechtsanders/winlibs_mingw/releases/download/14.2.0posix-12.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-14.2.0-mingw-w64ucrt-12.0.0-r3.zip' },
    @{ name = 'py.zip'; sha = '4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3'
       url = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip' },
    @{ name = 'glslang.zip'; sha = '82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b'
       url = 'https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip' },
    @{ name = 'sdl2.zip'; sha = '6cf9706eefd0a4a06dc764007934d428afaf029fabdd408a9e646048c91e18fb'
       url = 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10-win32-x64.zip' }
)

function Get-Tool($t) {
    $f = Join-Path $dl $t.name
    if ((Test-Path $f) -and (Get-FileHash $f -Algorithm SHA256).Hash -eq $t.sha) { return $f }
    Write-Host "Downloading $($t.url)"
    Invoke-WebRequest -Uri $t.url -OutFile $f -UseBasicParsing
    $h = (Get-FileHash $f -Algorithm SHA256).Hash
    if ($h -ne $t.sha) { Remove-Item $f; throw "$($t.name): SHA-256 is $h, expected $($t.sha)" }
    return $f
}

function Expand-To($zip, $dest) {
    New-Item -ItemType Directory -Force $dest | Out-Null
    tar -xf $zip -C $dest   # Windows' bsdtar: much faster than Expand-Archive on the 1.5 GB GCC tree
    if ($LASTEXITCODE) { throw "could not extract $zip" }
}

$gcc = Join-Path $root 'gcc\Library'
if (-not (Test-Path (Join-Path $gcc 'bin\gcc.exe'))) {
    $tmp = Join-Path $dl 'gcc_x'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-To (Get-Tool $tools[0]) $tmp
    New-Item -ItemType Directory -Force (Join-Path $root 'gcc') | Out-Null
    Move-Item (Join-Path $tmp 'mingw64') $gcc
    Remove-Item -Recurse -Force $tmp
}

$py = Join-Path $root 'python'
if (-not (Test-Path (Join-Path $py 'python.exe'))) {
    Expand-To (Get-Tool $tools[1]) $py
    Copy-Item (Join-Path $py 'LICENSE.txt') (Join-Path $py 'LICENSE_PYTHON.txt')   # make_release.py's licence list
}

$gl = Join-Path $root 'third_party\glslang'
if (-not (Test-Path (Join-Path $gl 'bin\glslang.exe'))) { Expand-To (Get-Tool $tools[2]) $gl }

$rd = Join-Path $root 'redist'
New-Item -ItemType Directory -Force $rd | Out-Null
if (-not (Test-Path (Join-Path $rd 'SDL2.dll'))) {
    $tmp = Join-Path $dl 'sdl2_x'
    Expand-To (Get-Tool $tools[3]) $tmp
    Copy-Item (Join-Path $tmp 'SDL2.dll') $rd
    Remove-Item -Recurse -Force $tmp
}
if (-not (Test-Path (Join-Path $rd 'zlib1.dll'))) { Copy-Item (Join-Path $gcc 'bin\zlib1.dll') $rd }

Write-Host "Tools ready. Build with:"
Write-Host "  set GLSLANG=$gl\bin\glslang.exe"
Write-Host "  build.bat release X.Y.Z"
