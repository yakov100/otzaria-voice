# Runs the audio generation until morning: starts generate.py (which resumes
# where it stopped) and drops a STOP file at the given hour so that the run
# ends after the current shard. Register in Task Scheduler for 22:30, or run
# by hand before going to sleep:
#     powershell -File run_nights.ps1 [-StopAt 07:00] [-LimitLines 0]
param([string]$StopAt = "07:00", [int]$LimitLines = 0)
$work = Join-Path $env:LOCALAPPDATA "OtzariaVoice\distill"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Remove-Item (Join-Path $work "STOP") -ErrorAction SilentlyContinue
$stopTime = [datetime]::ParseExact($StopAt, "HH:mm", $null)
if ($stopTime -lt (Get-Date)) { $stopTime = $stopTime.AddDays(1) }
$watcher = Start-Job -ArgumentList $work, $stopTime -ScriptBlock {
  param($work, $stopTime)
  while ((Get-Date) -lt $stopTime) { Start-Sleep -Seconds 60 }
  New-Item -ItemType File -Path (Join-Path $work "STOP") -Force | Out-Null
}
$args = @("-X", "utf8", (Join-Path $here "generate.py"), "--work", $work)
if ($LimitLines -gt 0) { $args += @("--limit-lines", "$LimitLines") }
& python @args 2>&1 | Tee-Object -FilePath (Join-Path $work ("generate-" + (Get-Date -Format "yyyyMMdd-HHmm") + ".log"))
Stop-Job $watcher -ErrorAction SilentlyContinue
Remove-Job $watcher -ErrorAction SilentlyContinue
