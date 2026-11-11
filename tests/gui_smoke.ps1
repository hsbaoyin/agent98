# GUI smoke test for Agent98.
#
# Expects a prepared directory (default: build\testrun) that contains
# agent98.exe and agent98.ini pointing at tests\mock_openai.js.

param(
    [string]$Dir = "build\testrun",
    [int]$TimeoutSec = 30
)

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public class A98Ui {
    [DllImport("user32.dll", CharSet=CharSet.Ansi)]
    public static extern IntPtr FindWindowA(string cls, string title);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr parent, int id);

    [DllImport("user32.dll", CharSet=CharSet.Ansi, EntryPoint="SendMessageA")]
    public static extern IntPtr SendText(IntPtr h, uint msg, IntPtr w, string l);

    [DllImport("user32.dll", CharSet=CharSet.Ansi, EntryPoint="SendMessageA")]
    public static extern IntPtr SendBuf(IntPtr h, uint msg, IntPtr w, StringBuilder l);

    [DllImport("user32.dll", EntryPoint="SendMessageA")]
    public static extern IntPtr SendInt(IntPtr h, uint msg, IntPtr w, IntPtr l);

    [DllImport("user32.dll")]
    public static extern bool PostMessageA(IntPtr h, uint msg, IntPtr w, IntPtr l);

    public const uint WM_SETTEXT = 0x000C;
    public const uint WM_GETTEXT = 0x000D;
    public const uint WM_GETTEXTLENGTH = 0x000E;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_CLOSE = 0x0010;

    public const int IDC_TRANSCRIPT = 1000;
    public const int IDC_INPUT = 1001;
    public const int IDC_SEND = 1002;

    public static string Text(IntPtr h) {
        int n = (int)SendInt(h, WM_GETTEXTLENGTH, IntPtr.Zero, IntPtr.Zero);
        StringBuilder sb = new StringBuilder(n + 2);
        SendBuf(h, WM_GETTEXT, (IntPtr)(n + 1), sb);
        return sb.ToString();
    }
}
"@

$exe = Join-Path $Dir 'agent98.exe'
if (-not (Test-Path $exe)) {
    throw "missing $exe"
}

$p = Start-Process -FilePath $exe -WorkingDirectory (Resolve-Path $Dir) -PassThru
try {
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 200
        $hwnd = [A98Ui]::FindWindowA('Agent98Window', [NullString]::Value)
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        throw 'main window not found'
    }

    $inputBox = [A98Ui]::GetDlgItem($hwnd, [A98Ui]::IDC_INPUT)
    $transcript = [A98Ui]::GetDlgItem($hwnd, [A98Ui]::IDC_TRANSCRIPT)
    if ($inputBox -eq [IntPtr]::Zero -or $transcript -eq [IntPtr]::Zero) {
        throw 'input or transcript control not found'
    }

    function Start-Chat([string]$text) {
        [void][A98Ui]::SendText($inputBox, [A98Ui]::WM_SETTEXT, [IntPtr]::Zero, $text)
        [void][A98Ui]::SendInt($hwnd, [A98Ui]::WM_COMMAND, [IntPtr][A98Ui]::IDC_SEND, [IntPtr]::Zero)
    }

    function Wait-Transcript([string]$expect) {
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
            $t = [A98Ui]::Text($transcript)
            if ($t.Contains($expect)) {
                return $t
            }
            if ($p.HasExited) {
                throw "GUI exited while waiting for '$expect'"
            }
        }
        throw "timed out waiting for '$expect'; transcript:`n" + [A98Ui]::Text($transcript)
    }

    function Wait-ConfirmDialog {
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ((Get-Date) -lt $deadline) {
            $dlg = [A98Ui]::FindWindowA('#32770', 'Agent98 - Confirm tool call')
            if ($dlg -ne [IntPtr]::Zero) {
                return $dlg
            }
            Start-Sleep -Milliseconds 100
        }
        throw 'confirmation dialog did not appear'
    }

    Start-Chat 'hello'
    $t1 = Wait-Transcript 'Hello from the mock server.'
    Write-Host 'PASS: streamed chat response rendered'

    Start-Chat 'list files'
    $t2 = Wait-Transcript 'I listed the workspace files for you.'
    if (-not $t2.Contains('[tool]')) {
        throw 'tool call line missing from transcript'
    }
    if (-not $t2.Contains('Directory:')) {
        throw 'tool result missing from transcript'
    }
    Write-Host 'PASS: tool call and result rendered'

    Start-Chat 'write file'
    $dlg = Wait-ConfirmDialog
    [void][A98Ui]::SendInt($dlg, [A98Ui]::WM_COMMAND, [IntPtr]1, [IntPtr]::Zero)
    $t3 = Wait-Transcript 'I wrote the file.'
    if (-not $t3.Contains('Wrote ')) {
        throw 'allowed write result missing from transcript'
    }
    if (-not (Test-Path (Join-Path $Dir 'workspace\mock.txt'))) {
        throw 'write_file did not create workspace\mock.txt'
    }
    Write-Host 'PASS: confirmation allowed and write_file executed'

    Start-Chat 'write file'
    $dlg = Wait-ConfirmDialog
    [void][A98Ui]::SendInt($dlg, [A98Ui]::WM_COMMAND, [IntPtr]2, [IntPtr]::Zero)
    $t4 = Wait-Transcript 'The write was denied.'
    if (-not $t4.Contains('denied by the user')) {
        throw 'denial message missing from transcript'
    }
    Write-Host 'PASS: confirmation denied and tool skipped'

    [void][A98Ui]::PostMessageA($hwnd, [A98Ui]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Seconds 2
    if (-not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
    }
    Write-Host 'GUI smoke test passed'
}
finally {
    if (-not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
    }
}

