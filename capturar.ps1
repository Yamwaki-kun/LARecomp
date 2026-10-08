# capturar.ps1
# Abre o LARecomp pelo RenderDoc para capturar quadros.
# F11 no jogo: captura exatamente 1 quadro do jogo (precisa do nosso ReXGlue).
# F12: captura do próprio RenderDoc, que corta o quadro do jogo no meio.
# As capturas vão para captures\reflexo_*.rdc (fora do git).
# Uso:  powershell -ExecutionPolicy Bypass -File .\capturar.ps1

$RenderDoc = "C:\Program Files\RenderDoc\renderdoccmd.exe"
$BuildDir = Join-Path $PSScriptRoot "out\build\win-amd64-relwithdebinfo"
$CaptureDir = Join-Path $PSScriptRoot "captures"

if (-not (Test-Path $RenderDoc)) {
    Write-Error "RenderDoc não encontrado em $RenderDoc"
    exit 1
}
New-Item -ItemType Directory -Force $CaptureDir | Out-Null

$env:REX_LOG_LEVEL = "warn"

# As configurações (resolução, async etc.) vêm do larecomp.toml ao lado do exe.
# gpu_debug_markers marca cada etapa do desenho dentro da captura.
& $RenderDoc capture `
    --working-dir $BuildDir `
    --capture-file (Join-Path $CaptureDir "reflexo") `
    (Join-Path $BuildDir "larecomp.exe") `
    --gpu_debug_markers=true `
    --d3d12_renderdoc_capture_key=true
