# build-win.ps1 - build Opal for Windows on the PC. Run from a normal PowerShell in the unpacked folder:
#   powershell -ExecutionPolicy Bypass -File tools\win\build-win.ps1
# Needs: Visual Studio 2022 (or Build Tools) with "Desktop development with C++" + a Windows 11 SDK, CMake 3.20+, Git.
# Needs Opal's camera assets in tools\vendor (opal-fw, opal-template): they come in the zip made on the Mac by
# tools/win/pack-for-windows.sh (they are extracted from Opal.app, so they are not in git).
# Result: tools\win\dist\ (then run Opal.exe from there; it installs itself).
param([switch]$Clean)
$ErrorActionPreference = 'Stop'
$Win = $PSScriptRoot
$Tools = Split-Path $Win -Parent
$Vendor = Join-Path $Tools 'vendor'
$Src = Join-Path $Vendor 'depthai-core-opal'
$DaiBuild = Join-Path $Vendor 'depthai-build-win'
$AppBuild = Join-Path $Vendor 'opal-win-build'
$Base = 'a3c01e446cc94fd4bc6dc668da11b8cbca4de979'  # see tools/win/README.md
function Step($m) { Write-Host "`n==> $m" -ForegroundColor Cyan }
function Need($cmd, $hint) { if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { throw "$cmd not found. $hint" } }

Step 'Checking tools'
Need git 'Install Git for Windows: https://git-scm.com/download/win'
Need cmake 'Install CMake: https://cmake.org/download/ (or the one bundled with Visual Studio)'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio 2022 / Build Tools not found (needs "Desktop development with C++").' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio is missing the C++ tools ("Desktop development with C++").' }
Write-Host "Visual Studio: $vs"
$gen = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
$env:CMAKE_POLICY_VERSION_MINIMUM = '3.5'  # CMake 4 vs. Hunter's old packages

Step 'Checking Opal assets'
$fw = Get-ChildItem (Join-Path $Vendor 'opal-fw') -Filter 'depthai-device-fwp-*.tar.xz' -ErrorAction SilentlyContinue | Select-Object -First 1
$bl = Get-ChildItem (Join-Path $Vendor 'opal-fw') -Filter 'depthai-bootloader-fwp-*.tar.xz' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $fw -or -not $bl -or -not (Test-Path (Join-Path $Vendor 'opal-template\full.json'))) {
  throw "Opal assets missing in $Vendor (opal-fw, opal-template). Unzip the package made by pack-for-windows.sh on the Mac."
}
if (-not (Test-Path (Join-Path $Vendor 'opal-template\luts\index.json'))) { Write-Warning 'Looks (luts) missing: the app will run without looks.' }
$devHash = $fw.Name -replace '.*fwp-([0-9a-f]{40}).*', '$1'
$blHash = $bl.Name -replace '.*fwp-([0-9a-f]{40}).*', '$1'

if ($Clean) { Remove-Item -Recurse -Force $DaiBuild, $AppBuild -ErrorAction SilentlyContinue }

Step 'Getting depthai-core (public source Opal is based on)'
if (-not (Test-Path $Src)) {
  git clone -q https://github.com/luxonis/depthai-core.git $Src
  git -C $Src fetch -q origin custom_uvc_uac_scripting
  git -C $Src checkout -q $Base
  git -C $Src submodule update -q --init shared/depthai-shared shared/depthai-bootloader-shared
}
# same patches as build-depthai.sh: Opal's firmware commits, restored message payload copy
$f = Join-Path $Src 'cmake\Depthai\DepthaiDeviceSideConfig.cmake'
(Get-Content $f) -replace 'set\(DEPTHAI_DEVICE_SIDE_COMMIT "[0-9a-f]*"\)', "set(DEPTHAI_DEVICE_SIDE_COMMIT `"$devHash`")" | Set-Content $f
$f = Join-Path $Src 'cmake\Depthai\DepthaiBootloaderConfig.cmake'
(Get-Content $f) -replace '^set\(DEPTHAI_BOOTLOADER_VERSION "[0-9a-f]*"\)', "set(DEPTHAI_BOOTLOADER_VERSION `"$blHash`")" | Set-Content $f
$f = Join-Path $Src 'src\pipeline\datatype\StreamMessageParser.cpp'
$t = Get-Content $f -Raw
$t = $t.Replace("    // std::vector<uint8_t> data(packet->data, packet->data + bufferLength);`n    std::vector<uint8_t> data;",
                "    std::vector<uint8_t> data(packet->data, packet->data + bufferLength);  // opal-win: restored")
$t = $t.Replace("    // std::vector<uint8_t> data(packet->data, packet->data + bufferLength);`r`n    std::vector<uint8_t> data;",
                "    std::vector<uint8_t> data(packet->data, packet->data + bufferLength);  // opal-win: restored")
Set-Content $f $t -NoNewline
if (-not (Select-String -Path $f -Pattern 'opal-win: restored' -Quiet)) { throw 'StreamMessageParser patch did not apply' }

Step 'Building depthai-core with Opal firmware (first time: 10-20 min, downloads dependencies)'
$res = Join-Path $DaiBuild 'resources'
New-Item -ItemType Directory -Force $res | Out-Null
Copy-Item $fw.FullName, $bl.FullName $res -Force
(git -C (Join-Path $Src 'shared\depthai-shared') rev-parse HEAD) | Set-Content (Join-Path $res "depthai-shared-commit-hash-$devHash.txt")
(git -C (Join-Path $Src 'shared\depthai-bootloader-shared') rev-parse HEAD) | Set-Content (Join-Path $res "depthai-bootloader-shared-commit-hash-$blHash.txt")
$daiInstall = Join-Path $DaiBuild 'install'
if (-not (Test-Path (Join-Path $DaiBuild 'CMakeCache.txt'))) {
  cmake -S $Src -B $DaiBuild @gen -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 `
    -DDEPTHAI_OPENCV_SUPPORT=OFF -DDEPTHAI_ENABLE_BACKWARD=OFF -DDEPTHAI_BUILD_EXAMPLES=OFF -DDEPTHAI_BUILD_TESTS=OFF `
    -DDEPTHAI_CLANG_FORMAT=OFF "-DCMAKE_INSTALL_PREFIX=$daiInstall"
  if ($LASTEXITCODE) { throw 'depthai-core configure failed' }
}
cmake --build $DaiBuild --config Release --parallel
if ($LASTEXITCODE) { throw 'depthai-core build failed' }
cmake --install $DaiBuild --config Release
if ($LASTEXITCODE) { throw 'depthai-core install failed' }

Step 'Building Opal for Windows'
cmake -S $Win -B $AppBuild @gen "-Ddepthai_DIR=$daiInstall\lib\cmake\depthai"
if ($LASTEXITCODE) { throw 'app configure failed' }
cmake --build $AppBuild --config Release --parallel
if ($LASTEXITCODE) { throw 'app build failed' }

Step 'Done'
Write-Host "Built: $Win\dist"
Write-Host "Next: run $Win\dist\Opal.exe and click Install."
