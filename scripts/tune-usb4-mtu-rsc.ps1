param(
  [string]$AdapterName = 'Ethernet 5',
  [int]$Mtu = 1500,
  [string]$LogPath = "$PSScriptRoot\..\usb4-mtu-rsc-tune.log"
)

$ErrorActionPreference = 'Stop'

function Write-Log {
  param([string]$Message)
  $line = "$(Get-Date -Format o) $Message"
  Add-Content -LiteralPath $LogPath -Value $line
  Write-Host $line
}

function Save-Section {
  param(
    [string]$Title,
    [scriptblock]$Command
  )

  Write-Log "=== $Title ==="
  try {
    & $Command | Out-String | Add-Content -LiteralPath $LogPath
  } catch {
    Write-Log "FAILED: $($_.Exception.Message)"
  }
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script from an elevated Administrator PowerShell.'
}

Remove-Item -LiteralPath $LogPath -Force -ErrorAction SilentlyContinue
Write-Log "Applying USB4 MTU/RSC tuning to adapter: $AdapterName"

Save-Section 'Before IP interface' { Get-NetIPInterface -InterfaceAlias $AdapterName | Format-List InterfaceAlias,AddressFamily,ConnectionState,NlMtu,Forwarding }
Save-Section 'Before netsh IPv4 MTU' { netsh interface ipv4 show subinterfaces }
Save-Section 'Before netsh IPv6 MTU' { netsh interface ipv6 show subinterfaces }
Save-Section 'Before RSC' { Get-NetAdapterRsc -Name $AdapterName -ErrorAction SilentlyContinue | Format-List * }

Write-Log "Setting IPv4 and IPv6 MTU to $Mtu on $AdapterName."
Set-NetIPInterface -InterfaceAlias $AdapterName -AddressFamily IPv4 -NlMtuBytes $Mtu
Set-NetIPInterface -InterfaceAlias $AdapterName -AddressFamily IPv6 -NlMtuBytes $Mtu

Write-Log "Disabling receive segment coalescing on $AdapterName."
Disable-NetAdapterRsc -Name $AdapterName

Write-Log 'Restarting adapter so MTU and RSC changes are applied.'
Restart-NetAdapter -Name $AdapterName -Confirm:$false
Start-Sleep -Seconds 5

Save-Section 'After IP interface' { Get-NetIPInterface -InterfaceAlias $AdapterName | Format-List InterfaceAlias,AddressFamily,ConnectionState,NlMtu,Forwarding }
Save-Section 'After netsh IPv4 MTU' { netsh interface ipv4 show subinterfaces }
Save-Section 'After netsh IPv6 MTU' { netsh interface ipv6 show subinterfaces }
Save-Section 'After RSC' { Get-NetAdapterRsc -Name $AdapterName -ErrorAction SilentlyContinue | Format-List * }

Write-Log 'USB4 MTU/RSC tuning complete.'
