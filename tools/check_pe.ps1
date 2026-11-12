# Check that a PE image is loadable on Windows 9x.
#
# Verifies the invariants required by tools\gen_ow_build.ps1:
#   * image base 0x00400000
#   * relocation table stripped (no .reloc, IMAGE_FILE_RELOCS_STRIPPED)
#   * subsystem version 4.0
#   * 256 KB stack reserve
#   * no NT-only imports (ntdll, api-ms-win-*, bcrypt, ...)
#
# Usage: powershell -File tools\check_pe.ps1 build\ow\a98.exe ...

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = 'Stop'

$blacklist = @(
    'ntdll.dll', 'bcrypt.dll', 'ncrypt.dll', 'crypt32.dll', 'secur32.dll',
    'shlwapi.dll', 'ole32.dll', 'oleaut32.dll', 'psapi.dll', 'userenv.dll'
)

function Get-U16([byte[]]$b, [int]$o) { return [BitConverter]::ToUInt16($b, $o) }
function Get-U32([byte[]]$b, [int]$o) { return [BitConverter]::ToUInt32($b, $o) }

function Test-Pe([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $fail = @()

    if ($b.Length -lt 0x100 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) {
        throw "$path is not a PE file"
    }
    $pe = Get-U32 $b 0x3C
    if ($b[$pe] -ne 0x50 -or $b[$pe + 1] -ne 0x45) {
        throw "$path has no PE signature"
    }
    $coff = $pe + 4
    $chars = Get-U16 $b ($coff + 18)
    $nsec = Get-U16 $b ($coff + 2)
    $optsize = Get-U16 $b ($coff + 16)
    $opt = $coff + 20
    $magic = Get-U16 $b $opt
    if ($magic -ne 0x10B) {
        throw "$path is not PE32"
    }

    $imagebase = Get-U32 $b ($opt + 28)
    $subsys_major = Get-U16 $b ($opt + 48)
    $stack = Get-U32 $b ($opt + 72)

    if ($imagebase -ne 0x400000) { $fail += ("image base 0x{0:X}" -f $imagebase) }
    if (($chars -band 0x0001) -eq 0) { $fail += 'relocations not stripped' }
    if ($subsys_major -ne 4) { $fail += "subsystem version $subsys_major" }
    if ($stack -ne 0x40000) { $fail += ("stack reserve 0x{0:X}" -f $stack) }

    $sec = $opt + $optsize
    $sections = @()
    for ($i = 0; $i -lt $nsec; $i++) {
        $s = $sec + $i * 40
        $name = [Text.Encoding]::ASCII.GetString($b, $s, 8).TrimEnd([char]0)
        $vsize = Get-U32 $b ($s + 8)
        $vaddr = Get-U32 $b ($s + 12)
        $rawsize = Get-U32 $b ($s + 16)
        $rawptr = Get-U32 $b ($s + 20)
        $sections += [pscustomobject]@{
            Name = $name; VAddr = $vaddr; VSize = $vsize
            RawPtr = $rawptr; RawSize = $rawsize
        }
        if ($name -eq '.reloc') { $fail += 'has a .reloc section' }
    }

    $imp_rva = Get-U32 $b ($opt + 104)
    if ($imp_rva -ne 0) {
        $imp_off = $null
        foreach ($s in $sections) {
            if ($imp_rva -ge $s.VAddr -and $imp_rva -lt ($s.VAddr + [Math]::Max($s.VSize, $s.RawSize))) {
                $imp_off = $s.RawPtr + ($imp_rva - $s.VAddr)
                break
            }
        }
        if ($imp_off -eq $null) {
            $fail += 'import directory outside the sections'
        } else {
            $d = $imp_off
            while ($d + 20 -le $b.Length) {
                $name_rva = Get-U32 $b ($d + 12)
                $first_thunk = Get-U32 $b $d
                if ($name_rva -eq 0 -and $first_thunk -eq 0) { break }
                $name_off = $null
                foreach ($s in $sections) {
                    if ($name_rva -ge $s.VAddr -and $name_rva -lt ($s.VAddr + [Math]::Max($s.VSize, $s.RawSize))) {
                        $name_off = $s.RawPtr + ($name_rva - $s.VAddr)
                        break
                    }
                }
                if ($name_off -ne $null) {
                    $end = $name_off
                    while ($end -lt $b.Length -and $b[$end] -ne 0) { $end++ }
                    $dll = [Text.Encoding]::ASCII.GetString($b, $name_off, $end - $name_off).ToLower()
                    if ($blacklist -contains $dll -or $dll -like 'api-ms-win-*') {
                        $fail += "NT-only import $dll"
                    }
                }
                $d += 20
            }
        }
    }

    if ($fail.Count -eq 0) {
        Write-Host ("PASS: {0} (base 0x{1:X}, subsystem {2}.0, stack 0x{3:X})" -f `
            (Split-Path -Leaf $path), $imagebase, $subsys_major, $stack)
        return $true
    }
    Write-Host ("FAIL: {0}: {1}" -f (Split-Path -Leaf $path), ($fail -join '; '))
    return $false
}

if ($Files.Count -eq 0) {
    throw 'usage: check_pe.ps1 <exe> [exe...]'
}

$ok = $true
foreach ($f in $Files) {
    if (-not (Test-Pe $f)) { $ok = $false }
}
if (-not $ok) { exit 1 }
