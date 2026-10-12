<#
  Per-user install of Fast Image Viewer. No admin rights; touches only your profile:
    - copies FastImageViewer.exe to %LOCALAPPDATA%\Programs\FastImageViewer
    - adds a Start menu shortcut
    - registers "Open with > Fast Image Viewer" for image types (HKCU only;
      your current default viewer is NOT changed)

  Install:    powershell -ExecutionPolicy Bypass -File .\install.ps1
  Uninstall:  powershell -ExecutionPolicy Bypass -File .\install.ps1 -Uninstall
#>
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$dir     = Join-Path $env:LOCALAPPDATA 'Programs\FastImageViewer'
$exe     = Join-Path $dir 'FastImageViewer.exe'
$classes = 'HKCU:\Software\Classes'
$app     = "$classes\Applications\FastImageViewer.exe"
$progId  = "$classes\FastImageViewer.Image"
$lnk     = Join-Path ([Environment]::GetFolderPath('Programs')) 'Fast Image Viewer.lnk'
$exts    = '.jpg','.jpeg','.jpe','.jfif','.png','.bmp','.dib','.gif','.tif','.tiff','.ico',
           '.jxr','.wdp','.hdp','.dds','.webp','.heic','.heif','.hif','.avif','.jxl'

function Ensure-Key($path) {
    # Never use New-Item -Force on an existing key: it would wipe values other apps own.
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
}

Get-Process FastImageViewer -ErrorAction SilentlyContinue | Stop-Process -Force

if ($Uninstall) {
    Remove-Item $lnk -ErrorAction SilentlyContinue
    Remove-Item $app -Recurse -ErrorAction SilentlyContinue
    Remove-Item $progId -Recurse -ErrorAction SilentlyContinue
    foreach ($e in $exts) {
        Remove-ItemProperty -Path "$classes\$e\OpenWithProgids" -Name 'FastImageViewer.Image' -ErrorAction SilentlyContinue
    }
    Remove-Item $dir -Recurse -ErrorAction SilentlyContinue
    Write-Host "Uninstalled. Your settings are kept in $env:LOCALAPPDATA\FastImageViewer (delete that folder to remove them)."
    return
}

$src = Join-Path $PSScriptRoot 'FastImageViewer.exe'
if (-not (Test-Path $src)) { throw "FastImageViewer.exe not found next to this script." }
New-Item -ItemType Directory -Path $dir -Force | Out-Null
Copy-Item $src $exe -Force
Unblock-File $exe   # drop the "downloaded from the internet" mark

$command = "`"$exe`" `"%1`""

Ensure-Key "$app\shell\open\command"
Set-ItemProperty -Path $app -Name 'FriendlyAppName' -Value 'Fast Image Viewer'
Set-ItemProperty -Path "$app\shell\open\command" -Name '(default)' -Value $command
Ensure-Key "$app\SupportedTypes"

Ensure-Key "$progId\shell\open\command"
Ensure-Key "$progId\DefaultIcon"
Set-ItemProperty -Path $progId -Name '(default)' -Value 'Image'
Set-ItemProperty -Path "$progId\DefaultIcon" -Name '(default)' -Value "$exe,0"
Set-ItemProperty -Path "$progId\shell\open\command" -Name '(default)' -Value $command

foreach ($e in $exts) {
    New-ItemProperty -Path "$app\SupportedTypes" -Name $e -Value '' -PropertyType String -Force | Out-Null
    Ensure-Key "$classes\$e\OpenWithProgids"
    New-ItemProperty -Path "$classes\$e\OpenWithProgids" -Name 'FastImageViewer.Image' -Value '' -PropertyType String -Force | Out-Null
}

$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnk)
$s.TargetPath = $exe
$s.WorkingDirectory = $dir
$s.Save()

Write-Host "Installed to $exe"
Write-Host "Right-click an image > Open with > Fast Image Viewer (or Settings > Apps > Default apps to make it the default)."
