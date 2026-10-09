# jogar.ps1
# Abre o LARecomp com as configurações que mediram melhor (ver CLAUDE.md).
# Uso:  powershell -ExecutionPolicy Bypass -File .\jogar.ps1          (normal)
#       powershell -ExecutionPolicy Bypass -File .\jogar.ps1 -Medir   (grava logs/timing_*.log)
#       powershell -ExecutionPolicy Bypass -File .\jogar.ps1 -Extra "--flag=valor","--outra=valor"

param([switch]$Medir, [string[]]$Extra = @())

$BuildDir = Join-Path $PSScriptRoot "out\build\win-amd64-relwithdebinfo"
if (-not (Test-Path (Join-Path $BuildDir "larecomp.exe"))) {
    Write-Error "larecomp.exe não encontrado em $BuildDir"
    exit 1
}

# Log em "warn": o padrão (trace) grava tanto em disco que causa stutter.
$env:REX_LOG_LEVEL = "warn"
if ($Medir) { $env:MCLA_TIMING_LOG = "1" }
# Com -Medir, cada quadro de 50 ms ou mais vira um aviso no larecomp_*.log,
# com o trabalho de textura daquele quadro (só no nosso ReXGlue).
if ($Medir) { $Extra += "--gpu_slow_frame_log_ms=50" }

$flags = @(
    "--clock_no_scaling=true",              # evita a queda de FPS em sessões longas
    "--clear_memory_page_state=false",      # menos travadas fortes
    "--submit_on_primary_buffer_end=true",  # divide o quadro em vários blocos (necessário para o async)
    "--d3d12_async_submission=true",        # thread nova de submissão (só no nosso ReXGlue)
    # Renderer nativo: sem o mcla_shaders.pack ele não desenha nada. Força o
    # caminho do ReXGlue mesmo que o menu tenha gravado outra coisa no larecomp.toml.
    "--mcla_native_gfx=false",
    "--mcla_native_gfx_nocp=false",
    "--mcla_native_gfx_continuous=false",
    # Ultrawide 3440x1440: o jogo acha que a TV é 1720x720 (2,389:1) e a câmera
    # usa essa proporção. Com resolution_scale = 2, renderiza em 3440x1440 nativo.
    # Para 16:9, trocar por --video_mode_width=1280 e --aspect_ratio=16:9.
    "--video_mode_width=1720",
    "--aspect_ratio=auto"
)
# Com "powershell -File", uma lista "a","b" chega como um texto só ("a,b"),
# então separa nas vírgulas.
$flags += $Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ }

Start-Process -FilePath (Join-Path $BuildDir "larecomp.exe") -WorkingDirectory $BuildDir -ArgumentList $flags
