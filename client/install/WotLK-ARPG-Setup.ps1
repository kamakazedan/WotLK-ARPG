<#
.SYNOPSIS
    Makes a WoW 3.3.5a client load WotLK-ARPG.dll at start-up, or undoes that.

.DESCRIPTION
    Clients patched for awesome_wotlk (by its own patcher, or by St0ny's AIO
    patcher, patch "Load AwesomeWotlkLib.dll at startup") already contain a
    small start-up routine that loads one DLL by name. This script changes that
    name, and nothing else: 20 bytes inside Wow.exe, written in place.

      install:    AwesomeWotlkLib.dll  ->  WotLK-ARPG.dll
      uninstall:  WotLK-ARPG.dll       ->  AwesomeWotlkLib.dll

    WotLK-ARPG.dll loads AwesomeWotlkLib.dll itself when that file is present,
    so awesome_wotlk keeps working alongside it.

    Installing first copies Wow.exe to Wow.exe.before-WotLK-ARPG. The script
    refuses to touch a file that does not look exactly as expected, and it
    reads the file back after writing.

.PARAMETER GameDir
    Folder that contains Wow.exe. Defaults to the folder this script is in.

.PARAMETER Uninstall
    Put the original DLL name back.
#>
param(
    [string]$GameDir,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$OurName = 'WotLK-ARPG.dll'
$StockName = 'AwesomeWotlkLib.dll'
$NameSize = 20          # bytes reserved for the name inside Wow.exe, including the final zero
$BuildTimestamp = 0x4C2452FE

# Virtual addresses inside Wow.exe (image base 0x00400000).
$SpliceAddress = 0x0040B7D0
$LoaderAddress = 0x004E5CB0
$NameAddress = 0x004E5CE0

# jmp to the loader, at the start of the routine that runs the game's main function
[byte[]]$Splice = 0xE9, 0xDB, 0xA4, 0x0D, 0x00
# the loader: mov eax,1 / mov [flag],eax / push offset name ...
[byte[]]$LoaderHead = 0xB8, 0x01, 0x00, 0x00, 0x00, 0xA3, 0x74, 0xB4, 0xB6, 0x00, 0x68, 0xE0, 0x5C, 0x4E, 0x00
# ... then LoadLibraryA, reached in one of two ways depending on which patcher wrote the loader:
# through a routine of the client (awesome_wotlk's patcher, older St0ny) or through the import table (St0ny)
[byte[]]$LoaderCallClient = 0xE8, 0x1C, 0x68, 0x38, 0x00, 0x83, 0xC4, 0x04
[byte[]]$LoaderCallImport = 0xFF, 0x15, 0x48, 0xF2, 0x9D, 0x00, 0x90, 0x90
# ... then the instructions the splice displaced, and back to where it came from
[byte[]]$LoaderTail = 0x55, 0x8B, 0xEC, 0xE8, 0xA1, 0x10, 0xF2, 0xFF, 0xE9, 0x04, 0x5B, 0xF2, 0xFF

function Stop-Setup([string]$message) {
    Write-Host ''
    Write-Host $message -ForegroundColor Red
    Write-Host 'Wow.exe was not changed.' -ForegroundColor Red
    exit 1
}

function Test-Bytes([byte[]]$data, [int]$offset, [byte[]]$expected) {
    if ($offset -lt 0 -or ($offset + $expected.Length) -gt $data.Length) { return $false }
    for ($i = 0; $i -lt $expected.Length; $i++) {
        if ($data[$offset + $i] -ne $expected[$i]) { return $false }
    }
    return $true
}

# File offset of a virtual address, from the section table. -1 if it is not in the file.
function Get-FileOffset([byte[]]$data, [long]$address) {
    $pe = [BitConverter]::ToInt32($data, 0x3C)
    $sectionCount = [BitConverter]::ToUInt16($data, $pe + 6)
    $optionalSize = [BitConverter]::ToUInt16($data, $pe + 20)
    [long]$imageBase = [BitConverter]::ToUInt32($data, $pe + 24 + 28)
    $table = $pe + 24 + $optionalSize
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $entry = $table + 40 * $i
        if (($entry + 40) -gt $data.Length) { break }
        [long]$start = $imageBase + [BitConverter]::ToUInt32($data, $entry + 12)
        [long]$rawSize = [BitConverter]::ToUInt32($data, $entry + 16)
        [long]$rawStart = [BitConverter]::ToUInt32($data, $entry + 20)
        if ($address -ge $start -and $address -lt ($start + $rawSize)) {
            [long]$offset = $rawStart + ($address - $start)
            if ($offset -lt $data.Length) { return [int]$offset }
        }
    }
    return -1
}

# The zero-terminated name stored in the loader's name field.
function Get-Name([byte[]]$data, [int]$offset) {
    $length = 0
    while ($length -lt $NameSize -and $data[$offset + $length] -ne 0) { $length++ }
    return [System.Text.Encoding]::ASCII.GetString($data, $offset, $length)
}

# Overwrites the name field, and only the name field, inside the file.
function Write-Name([string]$path, [int]$offset, [byte[]]$bytes) {
    $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None)
    try {
        $stream.Position = $offset
        $stream.Write($bytes, 0, $bytes.Length)
    } finally {
        $stream.Dispose()
    }
}

# SHA-256 as text, of bytes in memory and of a file. Done with .NET directly:
# Windows PowerShell loses its Get-FileHash command when it is started from a
# PowerShell 7 session, and this script must work from anywhere.
function Get-BytesHash([byte[]]$bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($sha.ComputeHash($bytes))
    } finally {
        $sha.Dispose()
    }
}

function Get-FileHashText([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($path)
    try {
        return [BitConverter]::ToString($sha.ComputeHash($stream))
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

# --- find and check the executable -----------------------------------------

if (-not $GameDir) { $GameDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) { Stop-Setup "Folder not found: $GameDir" }
$GameDir = (Resolve-Path -LiteralPath $GameDir).ProviderPath
$exePath = Join-Path $GameDir 'Wow.exe'
$backupPath = Join-Path $GameDir 'Wow.exe.before-WotLK-ARPG'
$dllPath = Join-Path $GameDir $OurName

Write-Host "WotLK-ARPG setup - $exePath"
if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    Stop-Setup "Wow.exe was not found in $GameDir. Put this script next to Wow.exe, or pass -GameDir."
}

[byte[]]$data = [System.IO.File]::ReadAllBytes($exePath)
if ($data.Length -lt 0x1000 -or $data[0] -ne 0x4D -or $data[1] -ne 0x5A) {
    Stop-Setup 'That file is not a Windows executable.'
}
$pe = [BitConverter]::ToInt32($data, 0x3C)
if ($pe -lt 0x40 -or ($pe + 0x400) -gt $data.Length -or [BitConverter]::ToUInt32($data, $pe) -ne 0x4550) {
    Stop-Setup 'That file is not a Windows executable.'
}
if ([BitConverter]::ToUInt32($data, $pe + 8) -ne $BuildTimestamp) {
    Stop-Setup 'This is not Wow.exe 3.3.5a (build 12340).'
}

$spliceOffset = Get-FileOffset $data $SpliceAddress
$loaderOffset = Get-FileOffset $data $LoaderAddress
$nameOffset = Get-FileOffset $data $NameAddress
$callOffset = $loaderOffset + $LoaderHead.Length
$tailOffset = $callOffset + $LoaderCallClient.Length
$hasLoader = $loaderOffset -ge 0 -and (Test-Bytes $data $spliceOffset $Splice) -and
    (Test-Bytes $data $loaderOffset $LoaderHead) -and
    ((Test-Bytes $data $callOffset $LoaderCallClient) -or (Test-Bytes $data $callOffset $LoaderCallImport)) -and
    (Test-Bytes $data $tailOffset $LoaderTail) -and
    $nameOffset -ge 0 -and ($nameOffset + $NameSize) -le $data.Length
if (-not $hasLoader) {
    Stop-Setup ("This Wow.exe has no DLL loader in it. Add one first: run St0ny's AIO patcher with the patch`n" +
        "'Load AwesomeWotlkLib.dll at startup', or awesome_wotlk's own patcher. Then run this again.")
}

$current = Get-Name $data $nameOffset
Write-Host "The loader in this Wow.exe currently loads: $current"

# --- decide ------------------------------------------------------------------

if ($Uninstall) {
    if ($current -eq $StockName) {
        Write-Host 'Already uninstalled: nothing to do.' -ForegroundColor Green
        exit 0
    }
    if ($current -ne $OurName) { Stop-Setup "Unexpected DLL name '$current' in the loader." }
    $newName = $StockName
} else {
    if ($current -eq $OurName) {
        Write-Host 'Already installed: nothing to do.' -ForegroundColor Green
        if (-not (Test-Path -LiteralPath $dllPath)) {
            Write-Host "Note: $OurName is not in this folder yet. Copy it next to Wow.exe." -ForegroundColor Yellow
        }
        exit 0
    }
    if ($current -ne $StockName) { Stop-Setup "Unexpected DLL name '$current' in the loader." }
    $newName = $OurName
}

[byte[]]$newBytes = New-Object byte[] $NameSize      # zero filled
[byte[]]$ascii = [System.Text.Encoding]::ASCII.GetBytes($newName)
if ($ascii.Length -ge $NameSize) { Stop-Setup 'Internal error: the DLL name is too long.' }
[Array]::Copy($ascii, 0, $newBytes, 0, $ascii.Length)

[byte[]]$oldBytes = New-Object byte[] $NameSize
[Array]::Copy($data, $nameOffset, $oldBytes, 0, $NameSize)

# --- write -------------------------------------------------------------------

if (-not $Uninstall) {
    # The file as it is now is the state to go back to: keep a copy of it.
    try {
        Copy-Item -LiteralPath $exePath -Destination $backupPath -Force
    } catch {
        Stop-Setup "Could not write the backup ($($_.Exception.Message))."
    }
    Write-Host "Backup written: $backupPath"
}

try {
    Write-Name $exePath $nameOffset $newBytes
} catch {
    Stop-Setup "Could not write to Wow.exe ($($_.Exception.Message)). Close the game and try again."
}

# Read it back: the file must be what it was, with the new name in place.
[Array]::Copy($newBytes, 0, $data, $nameOffset, $NameSize)
$same = $false
try {
    $same = (Get-FileHashText $exePath) -eq (Get-BytesHash $data)
} catch {
    $same = $false
}
if (-not $same) {
    try { Write-Name $exePath $nameOffset $oldBytes } catch { }
    Write-Host ''
    Write-Host 'Wow.exe did not read back as written, so the old name was put back.' -ForegroundColor Red
    if (Test-Path -LiteralPath $backupPath) {
        Write-Host "If the game no longer starts, copy $backupPath over Wow.exe." -ForegroundColor Red
    }
    exit 1
}

Write-Host ''
if ($Uninstall) {
    Write-Host "Done. Wow.exe loads $StockName again." -ForegroundColor Green
    try {
        if ((Test-Path -LiteralPath $backupPath) -and ((Get-FileHashText $exePath) -eq (Get-FileHashText $backupPath))) {
            Write-Host 'It is byte for byte the file it was before the install.'
        }
    } catch {
        # Only a remark: the uninstall itself is done and verified.
    }
    Write-Host "You can delete $OurName, WotLK-ARPG.log and Wow.exe.before-WotLK-ARPG."
} else {
    Write-Host "Done. Wow.exe now loads $OurName at start-up." -ForegroundColor Green
    if (-not (Test-Path -LiteralPath $dllPath)) {
        Write-Host "Note: $OurName is not in this folder yet. Copy it next to Wow.exe." -ForegroundColor Yellow
    }
    Write-Host 'After starting the game, WotLK-ARPG.log in this folder shows what the library did.'
}
exit 0
