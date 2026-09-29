param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [string]$Version, [string]$Repository = '')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if (!$Version) { $Version = (Get-Content -LiteralPath "$taskRoot/VERSION" -Raw).Trim() }
if ($Version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') { throw '版本必须是 major.minor.patch' }
if ($Repository -and $Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw '仓库必须是 owner/repo' }
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$taskVs = & $taskVsWhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$taskVs) { throw '需要安装 Visual Studio 2022 C++ 桌面工具链' }
$taskCmake = Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $taskCmake -S $taskRoot -B (Join-Path $taskRoot 'build/core') -G 'Visual Studio 17 2022' -A x64 "-DGAMEGAUGE_VERSION=$Version" "-DGAMEGAUGE_REPOSITORY=$Repository"
if ($LASTEXITCODE) { throw 'CMake 配置失败' }
& $taskCmake --build (Join-Path $taskRoot 'build/core') --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'C++ 构建失败' }
& (Join-Path (Split-Path $taskCmake -Parent) 'ctest.exe') --test-dir (Join-Path $taskRoot 'build/core') -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw '核心契约测试失败' }
