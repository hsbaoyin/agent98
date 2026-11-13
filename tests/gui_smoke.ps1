# GUI smoke test for Agent98.
#
# Expects a prepared directory (default: build\testrun) that contains
# agent98.exe and agent98.ini pointing at tests\mock_openai.js.
#
# Covers:
#   * streamed replies, tool calls/results and the confirmation dialog
#   * auto-saved session files and the History combo box
#   * the native status bar (class and layout)
#   * DOSKEY-style Alt+Up / Alt+Down prompt history
#   * New conversation and loading a session from the combo

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

    [DllImport("user32.dll", CharSet=CharSet.Ansi)]
    public static extern int GetClassNameA(IntPtr h, StringBuilder sb, int max);

    [DllImport("user32.dll", CharSet=CharSet.Ansi, EntryPoint="SendMessageA")]
    public static extern IntPtr SendText(IntPtr h, uint msg, IntPtr w, string l);

    [DllImport("user32.dll", CharSet=CharSet.Ansi, EntryPoint="SendMessageA")]
    public static extern IntPtr SendBuf(IntPtr h, uint msg, IntPtr w, StringBuilder l);

    [DllImport("user32.dll", EntryPoint="SendMessageA")]
    public static extern IntPtr SendInt(IntPtr h, uint msg, IntPtr w, IntPtr l);

    [DllImport("user32.dll")]
    public static extern bool PostMessageA(IntPtr h, uint msg, IntPtr w, IntPtr l);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);

    public struct RECT { public int Left, Top, Right, Bottom; }

    public const uint WM_SETTEXT = 0x000C;
    public const uint WM_GETTEXT = 0x000D;
    public const uint WM_GETTEXTLENGTH = 0x000E;
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_SYSKEYDOWN = 0x0104;

    public const uint CB_GETCOUNT = 0x0146;
    public const uint CB_SETCURSEL = 0x014E;
    public const int VK_UP = 0x26;
    public const int VK_DOWN = 0x28;
    public const int CBN_SELCHANGE = 1;

    public const int IDC_TRANSCRIPT = 1000;
    public const int IDC_INPUT = 1001;
    public const int IDC_SEND = 1002;
    public const int IDC_NEW = 1005;
    public const int IDC_STATUS = 1006;
    public const int IDC_SESSION = 1008;
    public const int IDC_OPEN_WORKSPACE = 1009;

    public static string Text(IntPtr h) {
        int n = (int)SendInt(h, WM_GETTEXTLENGTH, IntPtr.Zero, IntPtr.Zero);
        /* RichEdit expands CR paragraph marks to CRLF in WM_GETTEXT. */
        StringBuilder sb = new StringBuilder(n * 2 + 2);
        SendBuf(h, WM_GETTEXT, (IntPtr)(n * 2 + 1), sb);
        return sb.ToString();
    }

    public static string ClassName(IntPtr h) {
        StringBuilder sb = new StringBuilder(128);
        GetClassNameA(h, sb, sb.Capacity);
        return sb.ToString();
    }
}
"@

$exe = Join-Path $Dir 'agent98.exe'
if (-not (Test-Path $exe)) {
    throw "missing $exe"
}
$DirAbs = (Resolve-Path $Dir).Path
$sessions = Join-Path $DirAbs 'sessions'
New-Item -ItemType Directory -Force $sessions | Out-Null
Get-ChildItem $sessions -Filter *.json -ErrorAction SilentlyContinue |
    Remove-Item -Force

$root = Split-Path -Parent $PSScriptRoot
$mockProcess = $null
$needMock = $true
try {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 8787)
    $c.Close()
    $needMock = $false
} catch {}

if ($needMock) {
    $mockScript = Join-Path $root 'tests\mock_openai.js'
    $mockProcess = Start-Process -FilePath 'node' -ArgumentList @("`"$mockScript`"", "8787") -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 800
}

$p = Start-Process -FilePath $exe -WorkingDirectory $DirAbs -PassThru
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
    $combo = [A98Ui]::GetDlgItem($hwnd, [A98Ui]::IDC_SESSION)
    $statusbar = [A98Ui]::GetDlgItem($hwnd, [A98Ui]::IDC_STATUS)
    $workspace = [A98Ui]::GetDlgItem($hwnd, [A98Ui]::IDC_OPEN_WORKSPACE)
    if ($inputBox -eq [IntPtr]::Zero -or $transcript -eq [IntPtr]::Zero) {
        throw 'input or transcript control not found'
    }
    if ($combo -eq [IntPtr]::Zero) {
        throw 'session history combo not found'
    }
    if ($statusbar -eq [IntPtr]::Zero -or
        [A98Ui]::ClassName($statusbar) -ne 'msctls_statusbar32') {
        throw 'native status bar not found'
    }
    if ($workspace -eq [IntPtr]::Zero) {
        throw 'Workspace button not found'
    }
    $sr = New-Object A98Ui+RECT
    [void][A98Ui]::GetWindowRect($statusbar, [ref]$sr)
    if (($sr.Right - $sr.Left) -lt 400 -or ($sr.Bottom - $sr.Top) -lt 18) {
        throw 'status bar was not laid out across the window'
    }
    Write-Host 'PASS: session combo, status bar and Workspace button created'

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

    function Wait-SessionFile {
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while ((Get-Date) -lt $deadline) {
            $f = @(Get-ChildItem $sessions -Filter *.json -ErrorAction SilentlyContinue)
            if ($f.Count -ge 1) {
                return $f
            }
            if ($p.HasExited) {
                throw 'GUI exited before the session was auto-saved'
            }
            Start-Sleep -Milliseconds 200
        }
        throw 'session file was not auto-saved'
    }

    Start-Chat 'hello'
    $t1 = Wait-Transcript 'Hello from the mock server.'
    Write-Host 'PASS: streamed chat response rendered'

    $files = Wait-SessionFile
    if ($files[0].Name -notmatch '^\d{8}_\d{4}_.+\.json$') {
        throw "unexpected session filename: $($files[0].Name)"
    }
    if ($files[0].Name -notmatch 'hello') {
        throw "session filename is missing the prompt summary: $($files[0].Name)"
    }
    $count = [int]([A98Ui]::SendInt($combo, [A98Ui]::CB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero))
    if ($count -lt 1) {
        throw 'session combo was not populated'
    }
    Write-Host 'PASS: auto-saved session and combo populated'

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
    if (-not (Test-Path (Join-Path $DirAbs 'workspace\mock.txt'))) {
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

    # One conversation keeps one file; later rounds overwrite it.
    Start-Sleep -Milliseconds 300
    $all = @(Get-ChildItem $sessions -Filter *.json)
    if ($all.Count -ne 1) {
        throw "expected one session file, found $($all.Count)"
    }
    Write-Host 'PASS: one session file per conversation'

    # Alt+Up recalls the last prompt, Alt+Down returns to an empty box.
    [void][A98Ui]::SendInt($inputBox, [A98Ui]::WM_SYSKEYDOWN,
                           [IntPtr][A98Ui]::VK_UP, [IntPtr]::Zero)
    if ([A98Ui]::Text($inputBox) -ne 'write file') {
        throw 'Alt+Up did not recall the previous prompt'
    }
    [void][A98Ui]::SendInt($inputBox, [A98Ui]::WM_SYSKEYDOWN,
                           [IntPtr][A98Ui]::VK_DOWN, [IntPtr]::Zero)
    if ([A98Ui]::Text($inputBox) -ne '') {
        throw 'Alt+Down did not return to the empty prompt'
    }
    Write-Host 'PASS: Alt+Up / Alt+Down prompt history'

    # New clears the transcript; selecting the saved session reloads it.
    [void][A98Ui]::SendInt($hwnd, [A98Ui]::WM_COMMAND,
                           [IntPtr][A98Ui]::IDC_NEW, [IntPtr]::Zero)
    if ([A98Ui]::Text($transcript).Length -ne 0) {
        throw 'New did not clear the transcript'
    }
    [void][A98Ui]::SendInt($combo, [A98Ui]::CB_SETCURSEL, [IntPtr]::Zero, [IntPtr]::Zero)
    $sel = [IntPtr]([A98Ui]::IDC_SESSION -bor ([A98Ui]::CBN_SELCHANGE -shl 16))
    [void][A98Ui]::SendInt($hwnd, [A98Ui]::WM_COMMAND, $sel, $combo)
    $t5 = Wait-Transcript 'History (round'
    if (-not $t5.Contains('You: hello')) {
        throw 'loaded session is missing the first prompt'
    }
    Write-Host 'PASS: New conversation and combo session load'

    [void][A98Ui]::PostMessageA($hwnd, [A98Ui]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Seconds 2
    if (-not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
    }
    Write-Host 'GUI smoke test passed'
}
finally {
    if ($p -and -not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
    }
    if ($mockProcess -and -not $mockProcess.HasExited) {
        Stop-Process -Id $mockProcess.Id -Force
    }
}
