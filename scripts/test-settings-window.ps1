$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskBin = Join-Path $taskRoot 'build/core/bin/Release'
if (Get-Process GameGauge,GameGauge.Settings -ErrorAction SilentlyContinue) { throw '请先退出游戏仪表，避免干扰真实会话。' }
$taskData = Join-Path $taskRoot ('build/runtime-test/settings-window-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskData -Force | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SettingsNative {
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct MonitorInfo { public int Size; public Rect Monitor, Work; public uint Flags; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref Point p);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int command);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr rect, IntPtr region, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int index);
    [DllImport("user32.dll")] public static extern IntPtr GetSystemMenu(IntPtr h, bool reset);
    [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr menu, uint id, uint flags);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr h, ref MonitorInfo info);
    public static IntPtr Position(int x, int y) { return new IntPtr(unchecked((int)(((uint)y & 65535) << 16 | ((uint)x & 65535)))); }
    public static Rect Client(IntPtr h) { Rect r; GetClientRect(h, out r); return r; }
    public static Rect Work(IntPtr h) { var m = new MonitorInfo { Size = Marshal.SizeOf(typeof(MonitorInfo)) }; GetMonitorInfo(MonitorFromWindow(h, 2), ref m); return m.Work; }
    public static double Scale(IntPtr h) {
        var r = Client(h); var w = Work(h);
        var s = Math.Max(.75, Math.Min(Math.Min(GetDpiForWindow(h) / 96.0, 1.4), Math.Min((w.Right-w.Left-48)/1180.0, (w.Bottom-w.Top-48)/810.0)));
        return Math.Max(.1, Math.Min(s, Math.Min(r.Right/1164.0, r.Bottom/770.0)));
    }
    public static IntPtr ScreenPosition(IntPtr h, int x, int y) { var p = new Point { X=x, Y=y }; ClientToScreen(h, ref p); return Position(p.X, p.Y); }
    public static long Hit(IntPtr h, int x, int y) { return SendMessage(h, 0x84, IntPtr.Zero, ScreenPosition(h,x,y)).ToInt64(); }
    public static void MaximizeButton(IntPtr h) {
        var r = Client(h); var s = Scale(h); var p = ScreenPosition(h, (int)(r.Right-86*s), (int)(29*s));
        SendMessage(h, 0xA1, new IntPtr(9), p); SendMessage(h, 0xA2, new IntPtr(9), p);
    }
    public static void CloseButton(IntPtr h) {
        var r = Client(h); var s = Scale(h);
        var p = Position((int)(r.Right-38*s), (int)(29*s));
        SendMessage(h, 0x201, new IntPtr(1), p); SendMessage(h, 0x202, IntPtr.Zero, p);
    }
    public static void ExitButton(IntPtr h) {
        var r = Client(h); var s = Scale(h);
        var p = Position((int)(100*s), (int)(r.Bottom-132*s));
        SendMessage(h, 0x201, new IntPtr(1), p); SendMessage(h, 0x202, IntPtr.Zero, p);
    }
}
'@
$taskPreviousDpi = [SettingsNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
function Wait-SettingsWindow {
    for ($taskTry = 0; $taskTry -lt 50; $taskTry++) {
        $taskWindow = [SettingsNative]::FindWindow('GameGauge.Settings', [NullString]::Value)
        if ($taskWindow -ne [IntPtr]::Zero) {
            [SettingsNative]::ShowWindow($taskWindow,4) | Out-Null
            [SettingsNative]::RedrawWindow($taskWindow,[IntPtr]::Zero,[IntPtr]::Zero,0x101) | Out-Null
            Start-Sleep -Milliseconds 250; return $taskWindow
        }
        Start-Sleep -Milliseconds 100
    }
    throw '设置窗口未就绪。'
}
function Invoke-TestRequest([string]$Command) {
    $taskJson = '{"command":"' + $Command + '"}'
    # Windows PowerShell 的原生命令参数会去掉双引号，需要额外转义。
    if ($PSVersionTable.PSVersion.Major -lt 7 -or $PSNativeCommandArgumentPassing -eq 'Legacy') { $taskJson = $taskJson.Replace('"','\"') }
    & "$taskBin/GameGauge.Diagnostics.exe" --request $taskJson | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "诊断命令 $Command 失败。" }
}
$taskHost = Start-Process -FilePath "$taskBin/GameGauge.exe" -ArgumentList @('--background','--data-dir',('"'+$taskData+'"')) -WindowStyle Hidden -PassThru
$taskSettings = $null
try {
    Start-Sleep -Seconds 2
    $taskStatus = & "$taskBin/GameGauge.Diagnostics.exe" --host-status | ConvertFrom-Json
    if (!$taskStatus.ok -or $taskStatus.host_pid -ne $taskHost.Id) { throw '隔离宿主未就绪。' }
    $taskSettings = Start-Process -FilePath "$taskBin/GameGauge.Settings.exe" -ArgumentList @('--host-pid', $taskHost.Id) -WindowStyle Hidden -PassThru
    $taskWindow = Wait-SettingsWindow
    if (([SettingsNative]::GetWindowLong($taskWindow, -16) -band 0x20000) -ne 0) { throw '仍包含最小化窗口样式。' }
    if ([SettingsNative]::GetMenuState([SettingsNative]::GetSystemMenu($taskWindow,$false),0xF020,0) -ne [uint32]::MaxValue) { throw '系统菜单仍包含最小化。' }
    [SettingsNative]::SendMessage($taskWindow,0x112,[IntPtr]0xF020,[IntPtr]::Zero) | Out-Null
    if ([SettingsNative]::IsIconic($taskWindow)) { throw '最小化指令没有被禁用。' }
    $taskClient = [SettingsNative]::Client($taskWindow); $taskScale = [SettingsNative]::Scale($taskWindow)
    if ([SettingsNative]::Hit($taskWindow, [int]($taskClient.Right-86*$taskScale), [int](29*$taskScale)) -ne 9) { throw '最大化按钮没有使用系统命中类型。' }
    if ([SettingsNative]::Hit($taskWindow, [int](400*$taskScale), [int](60*$taskScale)) -ne 2) { throw '标题区不能拖动。' }
    if ([SettingsNative]::Hit($taskWindow, 1, 1) -ne 13) { throw '窗口边缘不能调整大小。' }
    [SettingsNative]::MaximizeButton($taskWindow)
    if (![SettingsNative]::IsZoomed($taskWindow)) { throw '最大化按钮无效。' }
    $taskClient = [SettingsNative]::Client($taskWindow); $taskWork = [SettingsNative]::Work($taskWindow)
    $taskOrigin = [SettingsNative+Point]::new(); [SettingsNative]::ClientToScreen($taskWindow, [ref]$taskOrigin) | Out-Null
    if ($taskOrigin.X -ne $taskWork.Left -or $taskOrigin.Y -ne $taskWork.Top -or $taskClient.Right -ne ($taskWork.Right-$taskWork.Left) -or $taskClient.Bottom -ne ($taskWork.Bottom-$taskWork.Top)) { throw '最大化后的客户区没有覆盖工作区，或遮住任务栏。' }
    Invoke-TestRequest 'settings'
    Start-Sleep -Milliseconds 700
    if (![SettingsNative]::IsZoomed($taskWindow)) { throw '托盘重复打开丢失了最大化状态。' }
    [SettingsNative]::MaximizeButton($taskWindow)
    if ([SettingsNative]::IsZoomed($taskWindow)) { throw '还原按钮无效。' }
    [SettingsNative]::CloseButton($taskWindow)
    if (!$taskSettings.WaitForExit(3000)) { throw '关闭按钮未退出设置。' }
    if ($taskHost.HasExited) { throw '关闭设置错误地退出了宿主。' }
    Invoke-TestRequest 'settings'
    $taskWindow = Wait-SettingsWindow
    $taskSettings = Get-Process GameGauge.Settings | Where-Object { $_.Path -eq "$taskBin\GameGauge.Settings.exe" } | Select-Object -First 1
    if (!$taskSettings) { throw '托盘没有重新打开设置。' }
    [SettingsNative]::ExitButton($taskWindow)
    if (!$taskHost.WaitForExit(5000) -or $taskHost.ExitCode -ne 0) { throw '退出程序按钮没有正常结束宿主。' }
    if (!$taskSettings.WaitForExit(3000)) { throw '退出程序后设置未关闭。' }
    Write-Host '最小化禁用、标题拖动/边缘命中、最大化/还原、任务栏边界、托盘保持最大化、关闭后重开和退出程序：通过。'
} finally {
    if (!$taskHost.HasExited) { Invoke-TestRequest 'quit'; $taskHost.WaitForExit(3000) | Out-Null }
    if ($taskSettings -and !$taskSettings.HasExited) { Stop-Process -Id $taskSettings.Id -ErrorAction SilentlyContinue }
    if (!$taskHost.HasExited) { Stop-Process -Id $taskHost.Id -ErrorAction SilentlyContinue }
    [SettingsNative]::SetThreadDpiAwarenessContext($taskPreviousDpi) | Out-Null
}
