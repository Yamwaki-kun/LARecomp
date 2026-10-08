# LARecomp: notas de trabalho

Contexto e progresso do trabalho local nesta cópia do LARecomp (recomp estático de Midnight Club: LA para PC via ReXGlue).

## Git

**Commits só no nome do Arnaldo. Nunca adicionar Claude como co-autor.**

| Repositório | Pasta | `origin` (fork) | `upstream` | Branch de trabalho |
|---|---|---|---|---|
| LARecomp | esta pasta | https://github.com/Yamwaki-kun/LARecomp | https://github.com/mzzvxm/larecomp (`main`) | `trabalho` |
| ReXGlue | `../rexglue-src` | https://github.com/Yamwaki-kun/rexglue-sdk | https://github.com/rexglue/rexglue-sdk (`main`) | `async-submission` |

- A pasta do LARecomp era um ZIP idêntico ao `upstream/main` (commit `23b91be`); o git foi iniciado em cima dela.
- `rexglue-src` é um clone raso (`--depth 1`) da tag `v0.10.0` (`f5337cd`), 2 commits atrás do `upstream/main`. Esses 2 commits não tocam nos arquivos alterados.
- Não commitar no `rexglue-src` as mudanças em `thirdparty/libmspack` e `thirdparty/moltenvk` (correção local dos symlinks no Windows).
- Um commit por assunto. O que é útil para o projeto original fica em commits separados das configurações locais e das notas, para virar PR com cherry-pick.

## Plano (em ordem)

1. **Rodar a 60 FPS estável.** Hoje roda a ~30–42 FPS com instabilidade (ver Diagnóstico).
2. **Resolver os glitches gráficos.** O principal conhecido é o dithered alpha em sombras e vegetação (ver README, "Known issues").
3. **Aplicar filtros gráficos** (pós-processamento: nitidez, AA etc.).
4. **Distribuir melhor o trabalho entre núcleos (multithread).** Ligado ao item 1, ver "Multithread" abaixo.
5. **Se possível, consertar** o que esses filtros e glitches revelarem, mandando as correções upstream (LARecomp e/ou ReXGlue).

## Ambiente montado (máquina do Arnaldo)

- Hardware: Ryzen 5 5600 (6c/12t), RTX 4070 Ti, 16 GB RAM, Windows 11 (build 26100).
- ReXGlue SDK 0.10.0 em `req/win-amd64/` (`rexglue.exe` em `req/win-amd64/bin/`).
- Arquivos do jogo (Xbox 360, NTSC/U, Title ID 545407F8) em `../MCLA_Game_Files/` (`default.xex` + `xarchive_*.rpf`).
  **Não colocar nada do jogo em `generated/`**: o codegen apaga e recria essa pasta.
- Toolchain: CMake (`C:\Program Files\CMake\bin`), LLVM/Clang 23 (`C:\Program Files\LLVM\bin`), Ninja (winget), VS 2026 Community (MSVC + vcvars), Windows SDK 10.0.26100.
  O SDK 10.0.22621 também está instalado, mas **não serve**: faltam `D3D_SHADER_MODEL_6_8` e as enhanced barriers.
  Clang/CMake não estão no PATH por padrão; os comandos abaixo adicionam.

## Comandos

Codegen (depois de mudar hooks ou o `larecomp_config.toml`):
```powershell
.\req\win-amd64\bin\rexglue.exe codegen larecomp_manifest.toml
```

Configurar do zero e compilar (cmd, com o ambiente do VS):
```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" >nul && set PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;%PATH% && cmake --preset win-amd64-relwithdebinfo -DCMAKE_PREFIX_PATH=%CD%/req/win-amd64 . && cmake --build out/build/win-amd64-relwithdebinfo"
```
Só recompilar: mesmo comando, sem a parte do `cmake --preset ...`.

Rodar:
```powershell
cd out\build\win-amd64-relwithdebinfo
$env:REX_LOG_LEVEL="warn"; Start-Process larecomp.exe -ArgumentList "--clock_no_scaling=true"
```
- Log de tempo de quadro: `$env:MCLA_TIMING_LOG="1"`, que gera `logs/timing_*.log`.
- Profiler: `$env:REX_LOG_LEVEL="warn"; powershell -ExecutionPolicy Bypass -File .\profile_gpu_commands.ps1`, que gera `logs/profile_*.log`.
- Configuração efetiva de cada execução: `logs/effective_config.txt`.

## O que já foi feito

- Montado o ambiente acima. Codegen OK (247 arquivos) e build OK (`out/build/win-amd64-relwithdebinfo/larecomp.exe`).
- `larecomp_manifest.toml`: `file_path` mudado de `out/build/win-amd64-release/assets/default.xex` para `../MCLA_Game_Files/default.xex` (é o que o README descreve). **Única mudança no código do projeto.**
- Tentativa de contornar o SDK antigo no `device_manager.cpp` foi **desfeita**; a solução foi instalar o SDK 26100.
- O jogo roda: chega ao menu e ao free roam.

## Diagnóstico de desempenho (2026-10-08)

Timing log, dirigindo: 30–36 FPS, com picos acima de 33 ms quase todo segundo. Profiler: média de 42 FPS, p50 22,9 ms, p99 60 ms.

Descartado: compilação de shader (`pipeline miss=0`), cache de texturas (`tex miss=0`), espera pela GPU, interrupt storm do vblank (~1.000/s, normal) e a GPU (4070 Ti sobra).

**Gargalo: a thread `GPU Commands` do ReXGlue, a ~95% de um núcleo.** A thread de render do jogo (XThread0728) passa ~60% do tempo esperando por ela em `D3DDevice_BlockOnFence` / `Patch_FenceSpinThrottle`. Os outros núcleos ficam ociosos.

Dentro da GPU Commands:
- ~40%: código do `rexgpu-xenosrd.dll` **sem símbolos**. Não dá para ver as funções com o SDK público.
- ~6%: `NtProtectVirtualMemory` / `rex::memory::Protect` (proteção de páginas; suspeita: `clear_memory_page_state=true`).
- ~10%: driver da NVIDIA.
- ~8%: `Mtx_lock` (contenção de mutex).

Flags relevantes no `effective_config.txt`:
- `d3d12_submit_on_primary_buffer_end = false`, mas o README diz que deveria ser `true`.
- `FAIL gpu_vsync_fast_poll`: a flag não existe no SDK público.
- `clear_memory_page_state = true`

Hipótese principal: o README diz que o autor usa um ReXGlue **customizado e não publicado**, que tira cerca de 480 mil chamadas por segundo de `QueryPerformanceCounter` (`PROFILE_SCOPE_US`) por pacote PM4 na GPU Commands (cerca de 25% dessa thread). O SDK 0.10.0 público provavelmente ainda tem esse overhead.

## Testes de flags (2026-10-08, timing log, sem os 5 s iniciais, trajeto de ~2 min)

| Execução | Flags | FPS médio | mín | p10 |
|---|---|---|---|---|
| 00:37 | nenhuma | 45,4 | 22 | 34 |
| 00:44 | `--clear_memory_page_state=false` | 45,7 | 33 | 35 |
| 00:45 | + `--submit_on_primary_buffer_end=true` | 47,1 | 34 | 37 |

- `clear_memory_page_state=false`: a média ficou igual, mas o FPS mínimo subiu (menos travadas fortes). Vale manter.
- `submit_on_primary_buffer_end=true`: diferença dentro do ruído, e o Arnaldo achou um pouco pior jogando. Fica desligado (padrão).
  Atenção: o nome certo é `submit_on_primary_buffer_end` (cvar do LARecomp, [graphics.cpp:61](src/mc_engine/hooks/graphics.cpp#L61)). Passar `--d3d12_...` direto não funciona, porque o `larecomp_app.h` sobrescreve.
- **Conclusão: as flags não chegam a 60 FPS.** O teto é a thread GPU Commands do ReXGlue.

## Filtros e resolução (passo 3)

Já existe no menu de pausa ([pause_menu.cpp:853](src/mc_engine/pause_menu.cpp#L853) e linhas ~930–1031):
- `resolution_scale` (RES SCALE)
- upscaler/efeito: bilinear, CAS, FSR 1, FSR 2, FSR 3, com qualidade e nitidez
- FXAA próprio (`mcla_native_gfx_fxaa`, em `src/native_gfx/d3d12/fxaa_pass.cpp`)

Ainda não foi testado nesta máquina. Resolução e filtros pesam na GPU (que está ociosa), mas resoluções maiores podem aumentar o trabalho de resolve/EDRAM na thread GPU Commands; é preciso medir.
DLSS não existe: exigiria NVIDIA Streamline/NGX + motion vectors e depth, que um jogo de Xbox 360 não fornece. FSR 2/3 têm a mesma limitação; falta verificar como estão implementados aqui.

### Motion vectors / HUD (ideia futura)
- Motion vectors exatos para a câmera: depth (EDRAM) + matrizes view/proj do quadro atual e do anterior (hook). Para objetos em movimento: optical flow (a RTX 4070 Ti tem OFA em hardware).
- Separar HUD de cena: o jogo desenha o HUD/UI como geometria inline (BeginVertices/EndVertices) no fim do quadro, depois do composite/tonemap ([native_gfx.cpp:1966](src/native_gfx/native_gfx.cpp#L1966)), em duas superfícies 1280x720 RGBA8 ([native_gfx.cpp:763](src/native_gfx/native_gfx.cpp#L763)). Dá para aplicar upscaler/FXAA antes do HUD e desenhar o HUD por cima.

## Renderer nativo experimental (`src/native_gfx/`)
O LARecomp tem um renderer D3D12 próprio (`mcla_native_gfx`) e um modo "nocp" (sem o command processor do ReXGlue, [larecomp_app.h:299](src/larecomp_app.h#L299)). Se ele estiver maduro, **pula o gargalo da thread GPU Commands**. Investigar o estado dele antes de otimizar o ReXGlue.

## Profile com símbolos (2026-10-08, `logs/profile_20261008_010037.log`, ~45 FPS)

Tempo inclusivo da thread GPU Commands (% das amostras em quadros lentos):

| % | Função (rexglue-src) | O que é |
|---|---|---|
| ~59 | `D3D12CommandProcessor::IssueDraw` | Total por draw (inclui os itens abaixo) |
| **~30** | `SharedMemory::RequestRanges` ([shared_memory.cpp:340](../rexglue-src/src/graphics/shared_memory.cpp)) | Sincronizar a memória do Xbox com o buffer da GPU a cada draw |
| ↳ 14 | `D3D12SharedMemory::UploadRanges` | memcpy + CopyBufferRegion das páginas sujas |
| ↳ 10 | `SharedMemory::MakeRangeValid` → `EnablePhysicalMemoryAccessCallbacks` → `memory::Protect` (~7–9) | VirtualProtect (syscall) a cada upload para detectar novas escritas |
| **~17** | `IssueSwap` → `EndSubmission` → `DeferredCommandList::Execute` | Replay da command list diferida na D3D12 real, **na mesma thread**, no fim do quadro |
| ~13 | `PrimitiveProcessor::Process` | Conversão de índices/primitivos |
| ~10 | `Mtx_lock` | Contenção de mutex |
| ~8 | `UpdateBindings` | Descritores/bindings |
| ~6 | `TextureCache::RequestTextures` | Texturas |
| ~14 | driver NVIDIA (`OpenAdapter10/12`) | Chamadas D3D12 |

Alvos, em ordem de ganho e risco:
1. **Mover `DeferredCommandList::Execute` para uma thread worker** (~17%; multithread de verdade; a lista já é gravada de forma abstrata).
2. **Reduzir o custo do RequestRanges** (~30%): páginas reescritas todo quadro (geometria dinâmica) fazem o ciclo proteger → falta de página → re-upload a cada quadro. Ideias: agrupar as chamadas de VirtualProtect; tratar páginas "quentes" como sempre sujas (sem proteção, com re-upload uma vez por submissão); tirar o `std::vector` alocado a cada chamada (`merged_ranges`).
3. `PrimitiveProcessor::Process` (~13%): ver se há cache de índices convertidos.

## Mudança 1 no ReXGlue: submissão assíncrona (em teste)

Cvar nova `d3d12_async_submission` (padrão `false`, experimental) em `rexglue-src`:
- `include/rex/graphics/d3d12/command_processor.h`: thread `D3D12 Async Submit`, `AwaitAsyncSubmission()` e membros novos.
- `include/rex/graphics/d3d12/deferred_command_list.h`: `SwapStream()`.
- `src/graphics/d3d12/command_processor.cpp`: o `EndSubmission` troca o stream gravado com `async_command_list_` e a thread faz Reset → Execute → Close → ExecuteCommandLists → Signal. Fica no máximo 1 submissão em voo. Swaps esperam (o presenter usa a mesma fila). Início em `SetupContext`, parada em `ShutdownContext`.
- `shared_memory.cpp` / `texture_cache.cpp`: `AwaitAsyncSubmission()` antes de `UpdateTileMappings`. Também é chamado em `CheckSubmissionFence` antes do signal de operações na fila.
- Só ganha com várias submissões por quadro: rodar com `--d3d12_async_submission=true --submit_on_primary_buffer_end=true`.
- **Resultado (2026-10-08, `timing_20261008_010822`):** média de 55,0 FPS, mín. 36, p10 46. Antes, com as mesmas flags e sem async (00:45): média de 47,1, p10 37. **~+17% na média e ~+24% nos 10% piores.** O Arnaldo achou "muito mais suave". O log confirma `D3D12 async submission enabled`.
  Teste A (nossas DLLs, sem async, `timing_20261008_011050`): média de 51,4, mín. 34, p10 41. **Efeito isolado da thread async: +7% na média (51,4 → 55,0), +12% no p10 (41 → 46).** O resto do ganho em relação às 00:45 veio do nosso build do ReXGlue (outro compilador/flags; pode ter ruído).
  **Sem glitches novos** com a flag (verificado pelo Arnaldo). Candidata a ser ligada por padrão e enviada upstream.
- Risco conhecido: comandos gravados que referenciam descritores **CPU** transitórios, reescritos antes da thread executar. Se aparecer glitch só com a flag ligada, a suspeita começa por aí.

## Mudança 2 no ReXGlue: RequestRanges sem trava no caminho comum (em teste)

Profile com foco (`profile_20261008_012442`, 54,3 FPS, com async): `RequestRanges` = 23% das amostras da GPU Commands. Dentro dele: `UploadRanges` 40%, **`Mtx_lock` 34%** (`global_critical_region`, disputada com as threads do jogo), **alocação do `std::vector` 12%**. Chamadas vêm de `RequestRange` (95%): índices via `PrimitiveProcessor::Process` (~11% da thread) e vértices no `IssueDraw`.

Mudança em `shared_memory.h/.cpp`:
- `AreRangesValidLockFree()`: confere os bits de `system_page_flags_valid_` com `atomic_ref` (acquire), sem pegar a trava. Se todas as páginas já são válidas, retorna na hora.
- `request_ranges_merged_`: vetor persistente no lugar do `std::vector` local.
- **Resultado (`timing_20261008_012837`, com async):** média de 57,6 FPS, mín. 41, p10 51. Antes (teste B, `010822`): 55,0 / 36 / 46. **+5% na média, +11% no p10.** Sem glitches de geometria (verificado pelo Arnaldo).
- Acumulado desde o início (DLLs oficiais, 00:45): 47,1 → 57,6 de média (+22%); p10 37 → 51 (+38%).
- Raciocínio de segurança: os bits só são escritos sob a trava, e a página é invalidada (no handler da falta de página) **antes** da escrita do guest, então uma escrita ordenada antes do draw é sempre vista.

## Profile depois das mudanças 1 e 2 (`profile_20261008_013119`)

- 2ª janela de 30 s: **59,0 FPS (limite de 60)**, p99 23,6 ms, máx. 59 ms, mesmo com o profiler ligado.
- CPU: GPU Commands **77%** (antes ~95%, não está mais saturada), `D3D12 Async Submit` 21% (trabalho que saiu da GPU Commands), thread de render do jogo 71%.
- Maior custo isolado restante: `MakeRangeValid` → `EnableAccessCallbacks` → `VirtualProtect`, ~10% da thread. As chamadas já são agrupadas e só protegem páginas ainda não protegidas; o custo vem do ciclo proteger → falta de página → re-upload em geometria dinâmica. O `Protect` roda **segurando a trava global** (`xmemory.cpp:2103`), o que também trava as threads do jogo.
- Outros: `UpdateBindings` 11%, `RequestTextures` 9% (`FindOrCreateTexture` 28% dele; `CreateTexture` esperando o driver), `PrimitiveProcessor` 8%, `WriteRegister` 9%.
- Ideias para depois: fazer o `Protect` fora da trava global; tratar páginas "quentes" sem proteção (cuidado: buffers dinâmicos tipo ring dentro da mesma submissão).

## Multithread

- O jogo já é multithread (o Xbox 360 tem 3 núcleos e 6 threads), mas no PC só ~2,8 de 12 núcleos são usados.
- O teto é a thread `GPU Commands`: ela lê o stream PM4 **em ordem**, porque cada comando depende do estado deixado pelo anterior. Paralelizar a leitura em si é inviável.
- Caminhos realistas, a decidir depois do profile com símbolos:
  - **Tirar trabalho pesado da GPU Commands:** conversão e upload de texturas, `shared_memory` e proteção de páginas, processamento de primitivos/vértices (`primitive_processor`), hash de estado e shaders.
  - **Pipeline produtor/consumidor:** uma thread interpreta o PM4 e monta os draws; outra grava as command lists D3D12 e faz o submit.
  - **Reduzir contenção:** o `Mtx_lock` aparece com ~8% no profile.
- Código no ReXGlue: `rexglue-src/src/graphics/command_processor.cpp`, `.../d3d12/command_processor.cpp`, `shared_memory.cpp`, `primitive_processor.cpp`.

## Build do ReXGlue a partir do código-fonte

- Clone completo (tag v0.10.0 com submódulos) em `../rexglue-src`. A pasta `../rexglue-sdk` é um ZIP sem submódulos e não compila.
- Usa Ninja Multi-Config: o preset de configuração é `win-amd64`; o de build é `win-amd64-relwithdebinfo`.
- Corrigido: o git gravou os symlinks de `thirdparty/libmspack/cabextract/mspack/*` como texto; foram trocados pelos arquivos reais.
- Build OK (2026-10-08). Saída em `rexglue-src/out/win-amd64/RelWithDebInfo/` (`rexgpu-xenosrd.dll/.pdb`, `rexruntimerd.dll/.pdb`).
- Essas DLLs foram copiadas, com os `.pdb`, para `out/build/win-amd64-relwithdebinfo/`. As originais do SDK estão em `out/build/win-amd64-relwithdebinfo/_dll_original/`; para voltar, basta copiá-las de volta.
- **Atenção: todo build do LARecomp copia de novo as DLLs de `req/`, sobrescrevendo as nossas.** Depois de cada build, copiar outra vez `rexgpu-xenosrd.dll/.pdb` e `rexruntimerd.dll/.pdb` de `rexglue-src/out/win-amd64/RelWithDebInfo/`. (A melhorar: apontar o `CMAKE_PREFIX_PATH` para um install do rexglue-src.)
- `guest_profiler.cpp` ganhou a seção "inclusive": cada função na pilha é contada uma vez por amostra, para atribuir o tempo do driver a quem chamou.
- O modo automático do Claude bloqueia rodar o build de código externo; o Arnaldo roda o build manualmente:
  `cmd /c "<vcvars64.bat> && set PATH=...LLVM;...CMake;%PATH% && cmake --preset win-amd64 && cmake --build --preset win-amd64-relwithdebinfo > build_log.txt 2>&1"`

## Próximos passos (passo 1: 60 FPS)

1. ~~Testes de flags~~ (feito, ver acima).
2. Testar `lod_city_scale` e `disable_msaa` pelo menu de pausa, para medir o quanto o número de draws pesa.
3. Se não bastar: compilar o ReXGlue a partir do código-fonte (https://github.com/rexglue/rexglue), com símbolos, para o profiler nomear as funções; depois remover o `PROFILE_SCOPE_US` do caminho do PM4 e atacar os hotspots. Boa contribuição upstream (serve também para o `../skate3recomp-main`).
