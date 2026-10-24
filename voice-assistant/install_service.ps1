# Run once as Administrator on the server:
#   powershell -ExecutionPolicy Bypass -File install_service.ps1
# Starts the voice assistant at every boot (even before anyone logs in) and
# opens UDP port 42042 for the intercom boxes.
$folder = Split-Path -Parent $MyInvocation.MyCommand.Path
$bat = Join-Path $folder "start_gateway.bat"

New-NetFirewallRule -DisplayName "Intercom voice assistant (UDP 42042)" -Direction Inbound `
  -Protocol UDP -LocalPort 42042 -Profile Private,Domain -Action Allow -ErrorAction SilentlyContinue | Out-Null

$action = New-ScheduledTaskAction -Execute "cmd.exe" -Argument "/c `"$bat`"" -WorkingDirectory $folder
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
  -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType S4U -RunLevel Highest
Register-ScheduledTask -TaskName "IntercomVoiceAssistant" -Action $action -Trigger $trigger `
  -Settings $settings -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName "IntercomVoiceAssistant"
"Installed and started. Log: $folder\gateway.log"
