# perfil.ps1
# Abre o jogo com as flags do jogar.ps1, o profiler e o log de tempo ligados.
# Gera logs/profile_*.log e logs/timing_*.log em out\build\win-amd64-relwithdebinfo.
# Uso:  powershell -ExecutionPolicy Bypass -File .\perfil.ps1 [-SemLimite] [-Thread "GPU Commands"] [-Foco "Func1,Func2"]

param(
    [switch]$SemLimite,
    [string]$Thread = "GPU Commands",
    [string]$Foco = "",
    [string]$SlowMs = "7.0"
)

$env:MCLA_PROFILE = "1"
$env:MCLA_PROFILE_THREAD = $Thread
$env:MCLA_PROFILE_SLOW_MS = $SlowMs
$env:MCLA_PROFILE_FOCUS = $Foco

$extra = @()
if ($SemLimite) { $extra += "--fps_limit=0" }

& (Join-Path $PSScriptRoot "jogar.ps1") -Medir -Extra $extra
