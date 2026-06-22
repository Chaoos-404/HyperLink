param(
  [ValidateSet('off', 'on')]
  [string]$State,
  [string]$LogPath = "$PSScriptRoot\..\defender-firewall-toggle.log"
)

$ErrorActionPreference = 'Stop'

function Write-Log {
  param([string]$Message)
  $line = "$(Get-Date -Format o) $Message"
  Add-Content -LiteralPath $LogPath -Value $line
  Write-Host $line
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script from an elevated Administrator PowerShell.'
}

Write-Log "Requested Windows Defender Firewall state: $State"
Get-NetFirewallProfile |
  Select-Object Name,Enabled,DefaultInboundAction,DefaultOutboundAction |
  Out-String |
  Add-Content -LiteralPath $LogPath

if ($State -eq 'off') {
  Write-Log 'Turning Defender Firewall off for all profiles for a short controlled speed test.'
  Set-NetFirewallProfile -Profile Domain,Private,Public -Enabled False
} else {
  Write-Log 'Turning Defender Firewall on for all profiles.'
  Set-NetFirewallProfile -Profile Domain,Private,Public -Enabled True
}

Get-NetFirewallProfile |
  Select-Object Name,Enabled,DefaultInboundAction,DefaultOutboundAction |
  Out-String |
  Add-Content -LiteralPath $LogPath

Write-Log "Windows Defender Firewall is now set to: $State"
