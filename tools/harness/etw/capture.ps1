# Records CPU samples and context switches of the whole system with stacks, in
# named phases, to compare states of a running game in one trace.
#
#   capture.ps1 [-Output <folder>] [-Phases on,off] [-Seconds 20] [-Settle 5]
#
# Beeps once when a phase starts and three times when it ends. After a phase
# there are Settle seconds to change the state of the game for the next one.
# Needs an elevated prompt for the kernel provider, and asks for one itself.
# Uses wpr and not xperf -on: Windows may hold the session xperf would need.
param(
	[string]$Output = (Join-Path $env:USERPROFILE 'xxmi-etw'),
	[string[]]$Phases = @('on', 'off'),
	[int]$Seconds = 20,
	[int]$Settle = 5,
	[int]$StartDelay = 10
)
$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
	$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
		'-Output', "`"$Output`"", '-Phases', ($Phases -join ','),
		'-Seconds', $Seconds, '-Settle', $Settle, '-StartDelay', $StartDelay)
	$shell = (Get-Process -Id $PID).Path
	$process = Start-Process $shell -ArgumentList $arguments -Verb RunAs -PassThru -Wait
	exit $process.ExitCode
}

# The elevated window closes with its messages
New-Item -ItemType Directory -Force $Output | Out-Null
Start-Transcript -Path (Join-Path $Output 'capture-log.txt') -Force | Out-Null

# An elevated shell started from a list of arguments gets "a,b" as one string
$Phases = $Phases | ForEach-Object { $_ -split ',' } | Where-Object { $_ }

$trace = Join-Path $Output ((Get-Date -Format 'yyyyMMdd-HHmmss') + '.etl')

function Invoke-Wpr {
	& wpr.exe @args
	if ($LASTEXITCODE) { throw "wpr $args failed with $LASTEXITCODE" }
}

Write-Host "Recording starts in $StartDelay seconds: bring the game to the front."
Start-Sleep $StartDelay

Invoke-Wpr -start CPU -filemode -recordtempto $Output
try {
	foreach ($phase in $Phases) {
		[Console]::Beep(880, 300)
		Invoke-Wpr -marker "xxmi-phase-start $phase"
		Write-Host "Phase $phase, $Seconds seconds"
		Start-Sleep $Seconds
		Invoke-Wpr -marker "xxmi-phase-end $phase"
		1..3 | ForEach-Object { [Console]::Beep(440, 150) }
		if ($phase -ne $Phases[-1]) { Start-Sleep $Settle }
	}
	Write-Host 'Merging the trace'
	Invoke-Wpr -stop $trace
} catch {
	& wpr.exe -cancel 2>&1 | Out-Null
	throw
}
[Console]::Beep(1320, 500)
Write-Host "Trace: $trace"
