# jogar.ps1
# Abre o LARecomp com as configurações que mediram melhor (ver CLAUDE.md).
# Uso:  powershell -ExecutionPolicy Bypass -File .\jogar.ps1          (normal)
#       powershell -ExecutionPolicy Bypass -File .\jogar.ps1 -Medir   (grava logs/timing_*.log)

param([switch]$Medir)

$BuildDir = Join-Path $PSScriptRoot "out\build\win-amd64-relwithdebinfo"
if (-not (Test-Path (Join-Path $BuildDir "larecomp.exe"))) {
    Write-Error "larecomp.exe não encontrado em $BuildDir"
    exit 1
}

# Log em "warn": o padrão (trace) grava tanto em disco que causa stutter.
$env:REX_LOG_LEVEL = "warn"
if ($Medir) { $env:MCLA_TIMING_LOG = "1" }

$flags = @(
    "--clock_no_scaling=true",              # evita a queda de FPS em sessões longas
    "--clear_memory_page_state=false",      # menos travadas fortes
    "--submit_on_primary_buffer_end=true",  # divide o quadro em vários blocos (necessário para o async)
    "--d3d12_async_submission=true"         # thread nova de submissão (só no nosso ReXGlue)
)

Start-Process -FilePath (Join-Path $BuildDir "larecomp.exe") -WorkingDirectory $BuildDir -ArgumentList $flags
