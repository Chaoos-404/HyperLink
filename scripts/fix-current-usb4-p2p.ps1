param(
  [int]$Mtu = 1500,
  [string]$LogPath = "$PSScriptRoot\..\usb4-current-p2p-fix.log"
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

function Get-LiveUsb4P2pAdapter {
  $adapters = Get-NetAdapter -IncludeHidden |
    Where-Object {
      $_.Status -eq 'Up' -and
      $_.InterfaceDescription -like 'USB4(TM) P2P Network Adapter*'
    } |
    Sort-Object ifIndex -Descending

  if (-not $adapters) {
    throw 'No live USB4(TM) P2P Network Adapter was found.'
  }

  return $adapters[0]
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this script from an elevated Administrator PowerShell.'
}

Remove-Item -LiteralPath $LogPath -Force -ErrorAction SilentlyContinue
$adapter = Get-LiveUsb4P2pAdapter
$adapterName = $adapter.Name

Write-Log "Applying current USB4 P2P fix to adapter: $adapterName [$($adapter.InterfaceDescription)] ifIndex=$($adapter.ifIndex)"

Save-Section 'Before adapter' { Get-NetAdapter -Name $adapterName | Format-List Name,Status,LinkSpeed,InterfaceDescription,DriverInformation,MacAddress,ifIndex,InterfaceGuid }
Save-Section 'Before IP interface' { Get-NetIPInterface -InterfaceAlias $adapterName | Format-List InterfaceAlias,AddressFamily,ConnectionState,NlMtu,InterfaceMetric,Dhcp,Forwarding,WeakHostReceive,WeakHostSend }
Save-Section 'Before RSC' { Get-NetAdapterRsc -Name $adapterName -ErrorAction SilentlyContinue | Format-List * }
Save-Section 'Before bindings' { Get-NetAdapterBinding -Name $adapterName | Sort-Object ComponentID | Select-Object DisplayName,ComponentID,Enabled }

Write-Log "Setting IPv4 and IPv6 MTU to $Mtu on $adapterName."
Set-NetIPInterface -InterfaceAlias $adapterName -AddressFamily IPv4 -NlMtuBytes $Mtu
Set-NetIPInterface -InterfaceAlias $adapterName -AddressFamily IPv6 -NlMtuBytes $Mtu

Write-Log "Disabling IPv4 forwarding on $adapterName."
Set-NetIPInterface -InterfaceAlias $adapterName -AddressFamily IPv4 -Forwarding Disabled

$bindingsToDisable = @(
  'ft_fortifilter',
  'INSECURE_NPCAP',
  'vmware_bridge',
  'ms_l1vhlwf',
  'ms_l2bridge'
)

foreach ($componentId in $bindingsToDisable) {
  $binding = Get-NetAdapterBinding -Name $adapterName -ComponentID $componentId -ErrorAction SilentlyContinue
  if ($null -eq $binding) {
    Write-Log "Binding not present: $componentId"
    continue
  }

  if (-not $binding.Enabled) {
    Write-Log "Binding already disabled: $($binding.DisplayName) [$componentId]"
    continue
  }

  Write-Log "Disabling binding on ${adapterName}: $($binding.DisplayName) [$componentId]"
  Disable-NetAdapterBinding -Name $adapterName -ComponentID $componentId
}

Write-Log "Disabling receive segment coalescing on $adapterName."
Disable-NetAdapterRsc -Name $adapterName -ErrorAction SilentlyContinue

$interfaceGuid = [string]$adapter.InterfaceGuid
$interfaceGuid = $interfaceGuid.Trim('{', '}')
$tcpipPath = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{$interfaceGuid}"
Write-Log "Applying per-interface ACK overrides at $tcpipPath."
New-Item -Path $tcpipPath -Force | Out-Null
New-ItemProperty -Path $tcpipPath -Name TcpAckFrequency -PropertyType DWord -Value 1 -Force | Out-Null
New-ItemProperty -Path $tcpipPath -Name TCPNoDelay -PropertyType DWord -Value 1 -Force | Out-Null
New-ItemProperty -Path $tcpipPath -Name TcpDelAckTicks -PropertyType DWord -Value 0 -Force | Out-Null
Save-Section 'ACK registry values before restart' { Get-ItemProperty -LiteralPath $tcpipPath | Select-Object TcpAckFrequency,TCPNoDelay,TcpDelAckTicks,IPAddress,SubnetMask,DhcpIPAddress }

Write-Log "Restarting $adapterName so adapter settings apply."
Restart-NetAdapter -Name $adapterName -Confirm:$false
Start-Sleep -Seconds 8

$adapter = Get-LiveUsb4P2pAdapter
$adapterName = $adapter.Name
Write-Log "Live adapter after restart: $adapterName [$($adapter.InterfaceDescription)] ifIndex=$($adapter.ifIndex)"

Save-Section 'After adapter' { Get-NetAdapter -Name $adapterName | Format-List Name,Status,LinkSpeed,InterfaceDescription,DriverInformation,MacAddress,ifIndex,InterfaceGuid }
Save-Section 'After IP interface' { Get-NetIPInterface -InterfaceAlias $adapterName | Format-List InterfaceAlias,AddressFamily,ConnectionState,NlMtu,InterfaceMetric,Dhcp,Forwarding,WeakHostReceive,WeakHostSend }
Save-Section 'After RSC' { Get-NetAdapterRsc -Name $adapterName -ErrorAction SilentlyContinue | Format-List * }
Save-Section 'After bindings' { Get-NetAdapterBinding -Name $adapterName | Sort-Object ComponentID | Select-Object DisplayName,ComponentID,Enabled }

$interfaceGuid = [string]$adapter.InterfaceGuid
$interfaceGuid = $interfaceGuid.Trim('{', '}')
$tcpipPath = "HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{$interfaceGuid}"
Save-Section 'ACK registry values after restart' { Get-ItemProperty -LiteralPath $tcpipPath | Select-Object TcpAckFrequency,TCPNoDelay,TcpDelAckTicks,IPAddress,SubnetMask,DhcpIPAddress }

Write-Log 'Current USB4 P2P fix complete.'
