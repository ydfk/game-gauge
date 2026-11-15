param(
    [Parameter(Mandatory=$true)][string]$Setup,
    [Parameter(Mandatory=$true)][string]$Portable,
    [string]$PreviousSetup
)
$ErrorActionPreference = 'Stop'
if (Get-Process GameGauge,GameGauge.Settings,GameGauge.Saves -ErrorAction SilentlyContinue) {
    throw '已有游戏仪表进程，请在独立会话执行升级验证。'
}
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskTest = Join-Path $taskRoot ('build/runtime-test/upgrade-saves-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskTest | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class GameGaugePayload {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibraryEx(string name, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr module, IntPtr res);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr res);
    [DllImport("kernel32.dll")] static extern int SizeofResource(IntPtr module, IntPtr res);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    public static byte[] Read(string path) {
        var dll = LoadLibraryEx(path, IntPtr.Zero, 0x22);
        if (dll == IntPtr.Zero) throw new Win32Exception();
        try {
            var resource = FindResource(dll, new IntPtr(101), new IntPtr(10));
            if (resource == IntPtr.Zero) throw new Win32Exception();
            var bytes = new byte[SizeofResource(dll, resource)];
            Marshal.Copy(LockResource(LoadResource(dll, resource)), bytes, 0, bytes.Length);
            return bytes;
        } finally { FreeLibrary(dll); }
    }
}
'@

function Expand-SetupPayload([string]$Path, [string]$Name) {
    $taskZip = Join-Path $taskTest "$Name.zip"
    [IO.File]::WriteAllBytes($taskZip, [GameGaugePayload]::Read((Resolve-Path -LiteralPath $Path).Path))
    $taskDir = Join-Path $taskTest $Name
    Expand-Archive -LiteralPath $taskZip -DestinationPath $taskDir
    Join-Path $taskDir 'app'
}
function Assert-SavesPackage([string]$Directory) {
    $taskManifest = Get-Content -LiteralPath (Join-Path $Directory 'release-manifest.json') -Raw | ConvertFrom-Json
    if ('GameGauge.Saves.exe' -notin $taskManifest.files.name) { throw '清单缺少 GameGauge.Saves.exe' }
    foreach ($taskEntry in $taskManifest.files) {
        $taskFile = Get-Item -LiteralPath (Join-Path $Directory $taskEntry.name)
        if ($taskFile.Length -ne $taskEntry.bytes -or (Get-FileHash -LiteralPath $taskFile.FullName).Hash -ne $taskEntry.sha256) {
            throw "包内哈希失败：$($taskEntry.name)"
        }
        if ($taskEntry.name -in @('GameGauge.exe','GameGauge.Settings.exe','GameGauge.Saves.exe') -and
            $taskFile.VersionInfo.FileVersion -ne $taskManifest.version) { throw "版本不一致：$($taskEntry.name)" }
    }
    if (!(Test-Path -LiteralPath (Join-Path $Directory 'licenses/saves/DEPENDENCIES.txt'))) { throw '缺少 Rust 授权清单' }
}

$taskNew = Expand-SetupPayload $Setup 'new-setup'
$taskPortable = Join-Path $taskTest 'portable'
Expand-Archive -LiteralPath $Portable -DestinationPath $taskPortable
Assert-SavesPackage $taskNew
Assert-SavesPackage $taskPortable
$taskNewWorker = Join-Path $taskNew 'GameGauge.Saves.exe'
$taskPortableWorker = Join-Path $taskPortable 'GameGauge.Saves.exe'
if ((Get-FileHash -LiteralPath $taskNewWorker).Hash -ne (Get-FileHash -LiteralPath $taskPortableWorker).Hash) {
    throw '安装版和便携版的工作进程不同'
}
Write-Output '安装包 PE 资源和便携 ZIP 的版本、工作进程和文件校验和：通过'

if (!$PreviousSetup) { return }
$taskInstalled = Expand-SetupPayload $PreviousSetup 'mock-install'
$taskData = Join-Path $taskTest 'user-data'
New-Item -ItemType Directory -Path "$taskData/history","$taskData/saves" -Force | Out-Null
$taskHistory = Join-Path $taskData 'history/keep.txt'
$taskSaved = Join-Path $taskData 'saves/keep.txt'
[IO.File]::WriteAllText($taskHistory,'isolated old history')
[IO.File]::WriteAllText($taskSaved,'isolated old backup')
$taskExecutable = Join-Path $taskInstalled 'GameGauge.exe'
$taskWorker = Join-Path $taskInstalled 'GameGauge.Saves.exe'
$taskArgs = @('--background','--data-dir',('"' + $taskData + '"'))
. "$PSScriptRoot/setup-processes.ps1"
$taskProcess = Start-Process -FilePath $taskExecutable -ArgumentList $taskArgs -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Seconds 2
    if ($taskProcess.HasExited) { throw '旧版模拟宿主提前退出' }
    Stop-GameGaugeProcesses -Directory $taskInstalled
    if (!$taskProcess.WaitForExit(5000) -or $taskProcess.ExitCode) { throw '旧版模拟宿主未正常退出' }
    Copy-Item -Path (Join-Path $taskNew '*') -Destination $taskInstalled -Recurse -Force
    Assert-SavesPackage $taskInstalled
    $taskProcess = Start-Process -FilePath $taskExecutable -ArgumentList $taskArgs -PassThru -WindowStyle Hidden
    $taskChild = $null
    for ($taskTry = 0; $taskTry -lt 60; ++$taskTry) {
        $taskChild = Get-Process GameGauge.Saves -ErrorAction SilentlyContinue | Where-Object Path -eq $taskWorker | Select-Object -First 1
        if ($taskChild) { break }
        Start-Sleep -Milliseconds 150
    }
    if (!$taskChild) { throw '覆盖升级后工作进程未启动' }
    Stop-GameGaugeProcesses -Directory $taskInstalled
    if (!$taskProcess.WaitForExit(5000) -or $taskProcess.ExitCode -or !$taskChild.WaitForExit(5000)) { throw '关闭时仍有进程运行' }
    if ([IO.File]::ReadAllText($taskHistory) -ne 'isolated old history' -or [IO.File]::ReadAllText($taskSaved) -ne 'isolated old backup') {
        throw '升级修改了隔离历史或存档数据'
    }
    Write-Output '隔离安装目录的旧版退出、覆盖升级、工作进程释放和用户数据保留：通过'
} finally {
    if (!$taskProcess.HasExited) { Stop-GameGaugeProcesses -Directory $taskInstalled }
}
