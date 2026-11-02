# Instala (ou remove) o agente do painel CYD para iniciar com o Windows, sem janela.
#
#   powershell -ExecutionPolicy Bypass -File tools\pc_agent\install_startup.ps1            instala
#   powershell -ExecutionPolicy Bypass -File tools\pc_agent\install_startup.ps1 -Remove    remove
#
# Cria uma tarefa agendada do USUARIO ATUAL ("Painel CYD - agente"), disparada no logon,
# que roda o agente com pythonw.exe (sem console) e o reinicia se ele cair. Nao exige
# administrador e nao mexe em mais nada. Para ver o que ele faz: python agent.py status.
param([switch]$Remove)

$taskName = "Painel CYD - agente"
if ($Remove) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Get-CimInstance Win32_Process -Filter "Name='pythonw.exe'" |
        Where-Object { $_.CommandLine -match 'pc_agent|agent\.py' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
    Write-Host "Agente removido da inicializacao."
    return
}

$agentDir = $PSScriptRoot
$projectRoot = Split-Path (Split-Path $agentDir)
$pythonw = Join-Path $projectRoot ".venv\Scripts\pythonw.exe"
if (-not (Test-Path $pythonw)) { throw "pythonw.exe nao encontrado em $pythonw" }

$action = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$agentDir\agent.py`"" -WorkingDirectory $agentDir
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings `
    -Description "Agente do painel CYD (metricas e atalhos do PC)" -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Write-Host "Agente instalado e iniciado. Ele sobe sozinho a cada logon."
