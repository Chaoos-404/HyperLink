param(
  [string]$AdapterName = 'Ethernet 5',
  [string]$LogPath = "$PSScriptRoot\..\usb4-receive-fix.log"
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
Write-Log "Applying USB4 receive-path fix to adapter: $AdapterName"

Save-Section 'Before adapter' { Get-NetAdapter -Name $AdapterName | Format-List Name,Status,LinkSpeed,InterfaceDescription,DriverInformation,MacAddress,ifIndex }
Save-Section 'Before IP interface' { Get-NetIPInterface -InterfaceAlias $AdapterName | Format-List InterfaceAlias,AddressFamily,ConnectionState,NlMtu,InterfaceMetric,Dhcp,Forwarding,WeakHostReceive,WeakHostSend }
Save-Section 'Before RSC' { Get-NetAdapterRsc -Name $AdapterName -ErrorAction SilentlyContinue | Format-List * }
Save-Section 'Before bindings' { Get-NetAdapterBinding -Name $AdapterName | Sort-Object ComponentID | Select-Object DisplayName,ComponentID,Enabled }

Write-Log 'Disabling IPv4 forwarding on the direct USB4 link.'
Set-NetIPInterface -InterfaceAlias $AdapterName -AddressFamily IPv4 -Forwarding Disabled

$bindingsToDisable = @(
  'ft_fortifilter',
  'INSECURE_NPCAP',
  'vmware_bridge',
  'ms_l1vhlwf',
  'ms_l2bridge'
)

foreach ($componentId in $bindingsToDisable) {
  $binding = Get-NetAdapterBinding -Name $AdapterName -ComponentID $componentId -ErrorAction SilentlyContinue
  if ($null -eq $binding) {
    Write-Log "Binding not present: $componentId"
    continue
  }

  if (-not $binding.Enabled) {
    Write-Log "Binding already disabled: $($binding.DisplayName) [$componentId]"
    continue
  }

  Write-Log "Disabling binding on ${AdapterName}: $($binding.DisplayName) [$componentId]"
  Disable-NetAdapterBinding -Name $AdapterName -ComponentID $componentId
}

Write-Log 'Restarting adapter so binding and forwarding changes are applied.'
Restart-NetAdapter -Name $AdapterName -Confirm:$false
Start-Sleep -Seconds 5

Save-Section 'After adapter' { Get-NetAdapter -Name $AdapterName | Format-List Name,Status,LinkSpeed,InterfaceDescription,DriverInformation,MacAddress,ifIndex }
Save-Section 'After IP interface' { Get-NetIPInterface -InterfaceAlias $AdapterName | Format-List InterfaceAlias,AddressFamily,ConnectionState,NlMtu,InterfaceMetric,Dhcp,Forwarding,WeakHostReceive,WeakHostSend }
Save-Section 'After RSC' { Get-NetAdapterRsc -Name $AdapterName -ErrorAction SilentlyContinue | Format-List * }
Save-Section 'After bindings' { Get-NetAdapterBinding -Name $AdapterName | Sort-Object ComponentID | Select-Object DisplayName,ComponentID,Enabled }

Write-Log 'USB4 receive-path fix complete.'
