# Scripts de análise do RenderDoc

Rodam dentro do RenderDoc: `& "C:\Program Files\RenderDoc\qrenderdoc.exe" --python <script>.py`.
Os parâmetros vão por variáveis de ambiente (`RD_CAPTURE` = caminho do .rdc, `RD_OUT`/`RD_OUTDIR` = saída).

| Script | Faz | Variáveis extras |
|---|---|---|
| rd_dump.py | Lista texturas e a árvore de ações (com o RT de saída de cada draw) | |
| passes.ps1 | Agrupa os draws do dump em passes por render target | `-Dump <saida do rd_dump>` |
| rd_save.py | Salva texturas como PNG num evento (MSAA = média) | `RD_JOBS=eid:resid:nome;...` |
| rd_hist.py | Histórico de um pixel (quais eventos escreveram) | `RD_HIST=eid:resid:x:y` |
| rd_pick.py | Maior valor float numa região de uma textura | `RD_PICK=eid:resid:x0:y0:x1:y1` |
| rd_srv.py | Texturas lidas pelo VS/PS num evento (e salva em PNG) | `RD_EID` |
| rd_scan.py | Texturas pequenas lidas pelos draws num intervalo | `RD_RANGE=e0:e1` |
| rd_topo.py | Topologia, viewport e GS dos draws num intervalo | `RD_RANGE=e0:e1` |
| rd_blend.py | Estado de blend e formatos dos RTs em eventos | `RD_EIDS=e1,e2` |

Captura de 1 quadro do jogo: `.\capturar.ps1` e **F11** no jogo.
