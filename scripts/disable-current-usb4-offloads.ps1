param(
  [string]$LogPath = "$PSScriptRoot\..\usb4-offload-disable.log"
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

Write-Log "Disabling offloads on adapter: $adapterName [$($adapter.InterfaceDescription)] ifIndex=$($adapter.ifIndex)"
Save-Section 'Before advanced properties' { Get-NetAdapterAdvancedProperty -Name $adapterName | Sort-Object DisplayName | Select-Object DisplayName,DisplayValue,RegistryKeyword,RegistryValue }
Save-Section 'Before checksum offload' { Get-NetAdapterChecksumOffload -Name $adapterName -ErrorAction SilentlyContinue | Format-List * }
Save-Section 'Before LSO' { Get-NetAdapterLso -Name $adapterName -ErrorAction SilentlyContinue | Format-List * }

$advancedKeywords = @(
  '*IPChecksumOffloadIPv4',
  '*TCPChecksumOffloadIPv4',
  '*TCPChecksumOffloadIPv6',
  '*UDPChecksumOffloadIPv4',
  '*UDPChecksumOffloadIPv6',
  '*LsoV2IPv4',
  '*LsoV2IPv6'
)

foreach ($keyword in $advancedKeywords) {
  $property = Get-NetAdapterAdvancedProperty -Name $adapterName -RegistryKeyword $keyword -ErrorAction SilentlyContinue
  if ($null -eq $property) {
    Write-Log "Advanced property not present: $keyword"
    continue
  }

  Write-Log "Disabling $($property.DisplayName) [$keyword]"
  Set-NetAdapterAdvancedProperty -Name $adapterName -RegistryKeyword $keyword -DisplayValue 'Disabled'
}

Write-Log "Restarting $adapterName so offload changes apply."
Restart-NetAdapter -Name $adapterName -Confirm:$false
Start-Sleep -Seconds 8

$adapter = Get-LiveUsb4P2pAdapter
$adapterName = $adapter.Name
Write-Log "Live adapter after restart: $adapterName [$($adapter.InterfaceDescription)] ifIndex=$($adapter.ifIndex)"
Save-Section 'After advanced properties' { Get-NetAdapterAdvancedProperty -Name $adapterName | Sort-Object DisplayName | Select-Object DisplayName,DisplayValue,RegistryKeyword,RegistryValue }
Save-Section 'After checksum offload' { Get-NetAdapterChecksumOffload -Name $adapterName -ErrorAction SilentlyContinue | Format-List * }
Save-Section 'After LSO' { Get-NetAdapterLso -Name $adapterName -ErrorAction SilentlyContinue | Format-List * }
Save-Section 'After IP interface' { Get-NetIPInterface -InterfaceAlias $adapterName | Format-List InterfaceAlias,AddressFamily,ConnectionState,NlMtu,Forwarding }

Write-Log 'USB4 offload disable complete.'
