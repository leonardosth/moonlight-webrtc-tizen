# Script to update Apollo / Vibepollo configuration with recommended gaming stream settings
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$confPath = 'C:\Program Files\Apollo\config\sunshine.conf'

if (-not (Test-Path $confPath)) {
    throw "Arquivo $confPath não encontrado."
}

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Elevando permissões de administrador..."
    Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

Copy-Item $confPath "$confPath.bak" -Force
Write-Host "Backup criado em: $confPath.bak"

$newConfig = @'
capture = wgcc
frame_limiter_provider = rtss
nvenc_vbv_increase = 40
video_max_batch_size_kb = 32
nvenc_preset = 6
headless_mode = enabled
rtss_frame_limit_type = nvidia reflex
virtual_display_mode = per_client
server_cmd = [{"cmd":"bubbles.scr","elevated":false,"name":"Bubbles"}]
dd_use_sunshine_virtual_display_driver = enabled
address_family = both
nvenc_spatial_aq = enabled
nvenc_temporal_aq = enabled
dd_configuration_option = ensure_primary
fec_percentage = 0
'@

Set-Content -Path $confPath -Value $newConfig -Encoding utf8
Write-Host "sunshine.conf atualizado com sucesso:"
Get-Content $confPath
