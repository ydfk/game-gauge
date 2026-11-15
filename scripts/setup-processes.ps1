# 安装和卸载共用：仅停止指定安装目录内的程序，并等待映像文件释放。
function Stop-GameGaugeProcesses {
    param([Parameter(Mandatory=$true)][string]$Directory)
    $taskRoot = [IO.Path]::GetFullPath($Directory).TrimEnd('\') + '\'
    $taskNames = @('GameGauge','GameGauge.Settings','GameGauge.Saves','GameGauge.Diagnostics','GameGauge.CpuProbe')
    if (!('GameGaugeSetup.Windows' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace GameGaugeSetup {
    public static class Windows {
        delegate bool EnumWindow(IntPtr window, IntPtr unused);
        [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindow callback, IntPtr unused);
        [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int count);
        [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
        public static void CloseHost(uint process) {
            EnumWindows(delegate(IntPtr window, IntPtr unused) {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (process != owner) return true;
                var name = new StringBuilder(64);
                GetClassName(window, name, name.Capacity);
                if (name.ToString() != "GameGauge.Host") return true;
                PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
                return false;
            }, IntPtr.Zero);
        }
    }
}
'@
    }
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(22)
    $taskRequested = @{}
    do {
        $taskProcesses = @(Get-Process -Name $taskNames -ErrorAction SilentlyContinue | Where-Object {
            $_.Path -and [String]::Equals([IO.Path]::GetFullPath($_.Path),
                (Join-Path $taskRoot ($_.ProcessName + '.exe')), [StringComparison]::OrdinalIgnoreCase)
        })
        $taskStopping = @($taskProcesses | Where-Object ProcessName -In @('GameGauge','GameGauge.Saves'))
        foreach ($taskProcess in $taskProcesses) {
            try {
                if ($taskProcess.HasExited) { continue }
                if (!$taskRequested.ContainsKey($taskProcess.Id)) {
                    Write-Output "等待程序退出：$($taskProcess.ProcessName) PID $($taskProcess.Id)"
                    $taskRequested[$taskProcess.Id] = $true
                }
                if ($taskProcess.ProcessName -eq 'GameGauge') {
                    [GameGaugeSetup.Windows]::CloseHost([uint32]$taskProcess.Id)
                } elseif ($taskProcess.ProcessName -ne 'GameGauge.Saves') {
                    $taskProcess.Kill()
                    if (!$taskProcess.WaitForExit(5000)) { throw "程序未退出：$($taskProcess.ProcessName)" }
                }
            } finally { $taskProcess.Dispose() }
        }
        $taskLocked = @()
        foreach ($taskName in $taskNames) {
            $taskFile = Join-Path $taskRoot ($taskName + '.exe')
            if (!(Test-Path -LiteralPath $taskFile)) { continue }
            try {
                $taskStream = [IO.File]::Open($taskFile, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                $taskStream.Dispose()
            } catch [IO.IOException] { $taskLocked += $taskFile }
        }
        if (!$taskLocked.Count -and !$taskStopping.Count) { return }
        if ([DateTime]::UtcNow -ge $taskDeadline) { throw '游戏仪表或存档任务尚未安全退出，请关闭程序后重试。安装文件尚未修改。' }
        Start-Sleep -Milliseconds 200
    } while ($true)
}
