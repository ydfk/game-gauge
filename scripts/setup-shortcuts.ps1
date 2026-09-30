# 保留英文文件名供搜索索引使用，通过 Shell 本地化名称显示中文。
function Initialize-GameGaugeShellNames {
    if ('GameGauge.ShellNames' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace GameGauge {
    public static class ShellNames {
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        public static extern int SHSetLocalizedName(string path, string module, int resource);
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        public static extern int SHRemoveLocalizedName(string path);
    }
}
'@
}
function Set-GameGaugeShortcutName([string]$Path, [string]$Executable) {
    Initialize-GameGaugeShellNames
    $taskResult = [GameGauge.ShellNames]::SHSetLocalizedName($Path, $Executable, 201)
    if ($taskResult -ne 0) { throw "设置快捷方式中文名称失败：$taskResult" }
}
function Remove-GameGaugeShortcutName([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return }
    Initialize-GameGaugeShellNames
    [GameGauge.ShellNames]::SHRemoveLocalizedName($Path) | Out-Null
}
