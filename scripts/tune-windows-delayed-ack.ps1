param(
  [string]$LogPath = "$PSScriptRoot\..\windows-delayed-ack-tune.log"
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
Write-Log 'Applying Windows delayed-ACK experiment.'

Save-Section 'Before netsh global' { netsh int tcp show global }
Save-Section 'Before netsh supplemental' { netsh int tcp show supplemental }
Save-Section 'Before TCP settings' {
  Get-NetTCPSetting |
    Select-Object SettingName,AutoTuningLevelLocal,ScalingHeuristics,CongestionProvider,DelayedAckTimeoutMs,DelayedAckFrequency,MinRtoMs,InitialCongestionWindowMss,EcnCapability,Timestamps,ForceWS |
    Format-Table -AutoSize
}

Write-Log 'Setting Internet delayed ACK timeout to 10 ms and frequency to 1.'
try {
  Set-NetTCPSetting -SettingName Internet -DelayedAckTimeoutMs 10 -DelayedAckFrequency 1 -ErrorAction Stop
  Write-Log 'Internet template updated.'
} catch {
  Write-Log "Internet template update failed: $($_.Exception.Message)"
}

Write-Log 'Setting InternetCustom delayed ACK timeout to 10 ms and frequency to 1.'
try {
  Set-NetTCPSetting -SettingName InternetCustom -DelayedAckTimeoutMs 10 -DelayedAckFrequency 1 -ErrorAction Stop
  Write-Log 'InternetCustom template updated.'
} catch {
  Write-Log "InternetCustom template update failed: $($_.Exception.Message)"
}

Save-Section 'After netsh global' { netsh int tcp show global }
Save-Section 'After netsh supplemental' { netsh int tcp show supplemental }
Save-Section 'After TCP settings' {
  Get-NetTCPSetting |
    Select-Object SettingName,AutoTuningLevelLocal,ScalingHeuristics,CongestionProvider,DelayedAckTimeoutMs,DelayedAckFrequency,MinRtoMs,InitialCongestionWindowMss,EcnCapability,Timestamps,ForceWS |
    Format-Table -AutoSize
}

Write-Log 'Windows delayed-ACK experiment complete. New TCP connections will use the changed template.'
