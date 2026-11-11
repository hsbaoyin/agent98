# Settings dialog smoke test for Agent98.
#
# Exercises provider presets, the new API fields and "Test connection"
# against a private mock server on port 8788. The test INI is restored
# afterwards, so it can be run repeatedly.

param(
    [string]$Dir = "build\testrun",
    [int]$Port = 8788,
    [int]$TimeoutSec = 30
)

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public class A98SettingsUi {
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
    public const uint CB_GETCURSEL = 0x0147;
    public const uint CB_SETCURSEL = 0x014E;

    public const int IDOK = 1;
    public const int IDCANCEL = 2;
    public const int IDM_SETTINGS = 40005;
    public const int IDC_PROVIDER = 2000;
    public const int IDC_API_BASE = 2001;
    public const int IDC_API_KEY = 2002;
    public const int IDC_MODEL = 2003;
    public const int IDC_SYSTEM_PROMPT = 2010;
    public const int IDC_TEMPERATURE = 2011;
    public const int IDC_EXTRA_HEADERS = 2012;
    public const int IDC_TEST = 2013;
    public const int IDC_TEST_RESULT = 2014;
    public const int CBN_SELCHANGE = 1;

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
$ini = Join-Path $Dir 'agent98.ini'
$iniBackup = [IO.File]::ReadAllText((Resolve-Path $ini))

$mock = $null
$p = $null
try {
    $mock = Start-Process -FilePath 'node' `
        -ArgumentList @('tests\mock_openai.js', "$Port") `
        -WorkingDirectory (Resolve-Path '.') -PassThru
    Start-Sleep -Milliseconds 800

    $p = Start-Process -FilePath $exe -WorkingDirectory (Resolve-Path $Dir) -PassThru

    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 200
        $hwnd = [A98SettingsUi]::FindWindowA('Agent98Window', [NullString]::Value)
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        throw 'main window not found'
    }

    [void][A98SettingsUi]::PostMessageA($hwnd, [A98SettingsUi]::WM_COMMAND,
                                        [IntPtr][A98SettingsUi]::IDM_SETTINGS,
                                        [IntPtr]::Zero)

    $dlg = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $dlg -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $dlg = [A98SettingsUi]::FindWindowA('#32770', 'Agent98 Settings')
    }
    if ($dlg -eq [IntPtr]::Zero) {
        throw 'settings dialog not found'
    }

    $provider = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_PROVIDER)
    $base = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_API_BASE)
    $key = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_API_KEY)
    $model = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_MODEL)
    $prompt = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_SYSTEM_PROMPT)
    $temp = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_TEMPERATURE)
    $headers = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_EXTRA_HEADERS)
    $test = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_TEST)
    $result = [A98SettingsUi]::GetDlgItem($dlg, [A98SettingsUi]::IDC_TEST_RESULT)

    $idx = [int]([A98SettingsUi]::SendInt($provider, [A98SettingsUi]::CB_GETCURSEL,
                                         [IntPtr]::Zero, [IntPtr]::Zero))
    if ($idx -ne 5) {
        throw "expected Custom preset for the test config, got index $idx"
    }
    Write-Host 'PASS: initial provider preset detected as Custom'

    [void][A98SettingsUi]::SendInt($provider, [A98SettingsUi]::CB_SETCURSEL,
                                   [IntPtr]1, [IntPtr]::Zero)
    $notify = [IntPtr](((1 -shl 16) -bor [A98SettingsUi]::IDC_PROVIDER))
    [void][A98SettingsUi]::SendInt($dlg, [A98SettingsUi]::WM_COMMAND, $notify,
                                   [IntPtr]::Zero)
    if ([A98SettingsUi]::Text($base) -ne 'https://api.deepseek.com/v1') {
        throw 'DeepSeek preset did not set the API base'
    }
    if ([A98SettingsUi]::Text($model) -ne 'deepseek-chat') {
        throw 'DeepSeek preset did not set the model'
    }
    Write-Host 'PASS: provider preset fills API base and model'

    [void][A98SettingsUi]::SendText($base, [A98SettingsUi]::WM_SETTEXT,
                                    [IntPtr]::Zero, "http://127.0.0.1:$Port/v1")
    [void][A98SettingsUi]::SendText($key, [A98SettingsUi]::WM_SETTEXT,
                                    [IntPtr]::Zero, 'test')
    [void][A98SettingsUi]::SendText($model, [A98SettingsUi]::WM_SETTEXT,
                                    [IntPtr]::Zero, 'mock-model')
    [void][A98SettingsUi]::SendInt($dlg, [A98SettingsUi]::WM_COMMAND,
                                   [IntPtr][A98SettingsUi]::IDC_TEST,
                                   [IntPtr]::Zero)

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $connected = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        if ([A98SettingsUi]::Text($result).Contains('Connected successfully')) {
            $connected = $true
            break
        }
    }
    if (-not $connected) {
        throw ('test connection failed: ' + [A98SettingsUi]::Text($result))
    }
    Write-Host 'PASS: Test connection reports HTTP 200'

    [void][A98SettingsUi]::SendText($prompt, [A98SettingsUi]::WM_SETTEXT,
                                    [IntPtr]::Zero, 'Smoke system prompt')
    [void][A98SettingsUi]::SendText($temp, [A98SettingsUi]::WM_SETTEXT,
                                    [IntPtr]::Zero, '1.25')
    [void][A98SettingsUi]::SendText($headers, [A98SettingsUi]::WM_SETTEXT,
                                    [IntPtr]::Zero, 'X-Smoke: yes')
    [void][A98SettingsUi]::SendInt($dlg, [A98SettingsUi]::WM_COMMAND,
                                   [IntPtr][A98SettingsUi]::IDOK,
                                   [IntPtr]::Zero)
    Start-Sleep -Milliseconds 500

    $saved = [IO.File]::ReadAllText((Resolve-Path $ini))
    foreach ($needle in @("base=http://127.0.0.1:$Port/v1",
                          'model=mock-model',
                          'temperature=1.25',
                          'system_prompt=Smoke system prompt',
                          'extra_headers=X-Smoke: yes')) {
        if (-not $saved.Contains($needle)) {
            throw "settings were not saved: missing '$needle'"
        }
    }
    Write-Host 'PASS: system prompt, temperature and headers saved to INI'

    [void][A98SettingsUi]::PostMessageA($hwnd, [A98SettingsUi]::WM_CLOSE,
                                        [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Seconds 2
    if (-not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
    }
    Write-Host 'Settings smoke test passed'
}
finally {
    if ($p -ne $null -and -not $p.HasExited) {
        Stop-Process -Id $p.Id -Force
    }
    if ($mock -ne $null -and -not $mock.HasExited) {
        Stop-Process -Id $mock.Id -Force
    }
    [IO.File]::WriteAllText((Resolve-Path $ini), $iniBackup)
}
