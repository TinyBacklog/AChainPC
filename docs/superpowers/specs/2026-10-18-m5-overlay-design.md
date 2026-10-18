# M5 Overlay ImGui — design

Data: 2026-10-18
Estado: revisão 2, ainda sem código. A revisão 2 responde às seis perguntas da
leitura de um builder independente (marcadas "escolha minha" onde decidi).
Base: `main` em `8411e3c68` (M4 fechado).
Fecha a v1, junto com o M4. A milestone seguinte é de afinação (60 estáveis, depois
1440p/4K) e fica fora deste documento.

## 1. Intenção

Hoje, para ver como o jogo está a correr ou para mudar uma opção, o jogador tem de
sair do jogo, editar o `astralchain.ini` à mão e voltar a lançar. As métricas de
desempenho só existem no bench (`acre.py bench`), e os fallbacks do AOT só aparecem
depois de fechar o jogo, com `roots --from`.

O overlay põe isso dentro do jogo: métricas ao vivo, as opções do ini com aplicação
imediata quando possível, os contadores do AOT e o log. Abre e navega-se com o
comando, no sofá, sem pausar o jogo.

## 2. Objetivos, não-objetivos e anti-objetivos

**Objetivos**
- Abrir e fechar com **Insert** no teclado ou **L3+R3** no comando (decisão do
  utilizador). As duas combinações são configuráveis no ini.
- Navegação completa com o comando (decisão do utilizador), além de rato e teclado.
- **O jogo não pausa** com o overlay aberto (decisão do utilizador). Enquanto está
  aberto, o jogo não recebe input do comando nem do teclado.
- Quatro separadores: Desempenho, Definições, AOT, Log (secção 5).
- Um mini contador de fps num canto, independente do overlay, que se liga e desliga.
- Overlay desenhado depois do FSR, à resolução da janela, nítido com qualquer
  `resolution_scale`.

**Não-objetivos (v1)**
- Windows e macOS. O overlay segue o resto do projeto: Linux x86-64.
- Remapear botões um a um. O separador Definições só troca de preset (keyboard,
  steam_controller, custom).
- Utilização real do GPU (percentagem do hardware). Mostra-se a carga das threads
  de GPU do emulador, não a do GPU físico.
- Overlay nas screenshots do jogo.
- Traduções. A interface é em inglês, como o resto do repositório (escolha minha).

**Anti-objetivos** (falhar mesmo que funcione)
- Qualquer custo mensurável com o overlay fechado: média, 1% low ou paragens piores
  no bench de combate.
- Input a chegar ao jogo com o overlay aberto, ou a ficar preso depois de fechar
  (um botão que o jogo julga premido para sempre).
- Perder os comentários ou a ordem do `astralchain.ini` ao guardar.
- Um crash do jogo causado pelo overlay. Se o overlay não conseguir arrancar
  (Vulkan, fontes), o jogo corre sem ele e o log diz porquê.

## 3. Restrições

- Linux x86-64, SDL3 (sistema 3.4.x, mínimo 3.2.10), Vulkan 1.1+ carregado
  dinamicamente (`VK_NO_PROTOTYPES`), sem dynamic rendering.
- Dear ImGui via CPM (`cpmfile.json` + `AddJsonPackage`), numa versão fixa de
  release, só o núcleo (widgets, layout, navegação). Licença MIT, compatível com
  GPL-2.0-or-later. **Sem os backends oficiais** `imgui_impl_sdl3` e
  `imgui_impl_vulkan` (escolha minha, secções 4.2 e 4.3): o de Vulkan submete à
  fila por conta própria e gere buffers sem saber do timeline do scheduler; o de
  SDL3 lê os gamepads e a janela no thread onde corre o `NewFrame`.
- O `video_core` não passa a depender do ImGui. A fronteira é uma interface pequena
  (secção 4.2).
- Regras do repositório: nada de caminhos pessoais em ficheiros, sem código ou
  assets do jogo, testes nunca tocam no perfil real (`~/.local/share/astralchain`,
  `~/.config/astralchain`).
- Custo com o overlay aberto: alvo < 0.3 ms por frame, medido (secção 8).

## 4. Arquitetura

### 4.1 Threads

Três threads tocam no overlay:

| Thread | O que faz |
|---|---|
| main (SDL) | recebe os eventos SDL; com o overlay aberto, entrega-os ao overlay em vez de ao jogo |
| GPU (`Composite`) | uma vez por frame apresentado: constrói o frame ImGui e uma cópia dos dados de desenho |
| VulkanWorker | grava os comandos de desenho a partir dessa cópia, num lambda do scheduler |

- Os eventos passam do main para o GPU numa fila protegida por mutex. O ImGui só é
  usado no thread GPU (NewFrame, widgets, Render), alimentado à mão a partir da fila
  (`AddKeyEvent`, `AddMouse*`, teclas de gamepad do ImGui). Nada no thread GPU chama
  funções SDL.
- O caminho inverso também existe: ações que têm de correr no main thread
  (`SDL_SetWindowFullscreen`, abrir/fechar, gravar o ini) seguem por uma segunda
  fila, do GPU para o main, consumida no `WaitEvent`.
- O resultado de `ImGui::Render()` é copiado (ImDrawData com as suas listas) antes
  de entrar no lambda do scheduler, porque o lambda corre mais tarde noutro thread.
- Fechado, o overlay não constrói frames nem grava comandos. Só o mini contador de
  fps, se ligado, desenha.

### 4.2 Desenho no Vulkan

- Ponto de inserção: em `RendererVulkan::Composite`, entre
  `blit_swapchain.DrawToFrame(...)` e `scheduler.Flush(*frame->render_ready)`
  (renderer_vulkan.cpp:223-226). Só o `blit_swapchain`; o `blit_capture` e o
  `blit_applet` não levam overlay.
- O alvo é `Frame::image`/`frame->framebuffer`, já à resolução da janela, depois
  do FSR. Um render pass próprio com `loadOp = LOAD` (initial layout GENERAL, mesmo
  formato do frame), criado com `CreateWrappedRenderPass` no modo LOAD.
- Fronteira: `video_core` define uma interface `VideoCore::FrameOverlay` com
  - `Init(const OverlayVulkanContext&)`: instance, physical device, device, família
    e fila gráfica, `vkGetInstanceProcAddr`, formato e número de frames;
  - `Build()`: chamado no thread GPU, devolve se há algo a desenhar;
  - `Record(VkCommandBuffer, VkFramebuffer, VkExtent2D)`: chamado no lambda;
  - `OnFormatChanged(...)`: quando o frame é recriado com outro formato.
  O `RendererBase` ganha `SetFrameOverlay(std::shared_ptr<FrameOverlay>)`; o
  `RendererVulkan` implementa-o. O frontend implementa a interface com ImGui.
- **Renderer próprio, pequeno** (escolha minha): um pipeline (vértices ImGui,
  textura, blending alfa, scissor), um descriptor pool próprio e funções Vulkan
  tiradas do dispatch do emulador. O overlay **não submete nada por si**: o upload
  da textura das fontes e os buffers de vértices e índices gravam-se no command
  buffer do scheduler, e os buffers rodam presos aos ticks do scheduler (só se
  reutiliza um buffer quando o tick em que foi usado já terminou).
- O atlas de fontes é construído uma vez no `Init`; não se usam texturas dinâmicas
  do ImGui 1.92.
- Se `Init` falhar: log de aviso, overlay desativado para a sessão, jogo continua.

### 4.3 Input

- **Combinação de abrir.** O main thread vê os eventos `SDL_EVENT_GAMEPAD_BUTTON_*`
  (hoje caem no `default:` do `WaitEvent`). Um detetor de combinação puro recebe
  pressões e libertações e dispara quando todos os botões da combinação ficam
  premidos ao mesmo tempo; só volta a disparar depois de todos serem largados.
  Insert vem de `SDL_EVENT_KEY_DOWN`.
- **Bloquear o jogo.** Ao abrir, `HIDCore::EnableAllControllerConfiguration()`: os
  getters do comando passam a devolver vazio ao jogo. Ao fechar,
  `DisableAllControllerConfiguration()`. Teclado e rato deixam de ser
  encaminhados pelo `WaitEvent` enquanto o overlay está aberto; ao abrir, o
  frontend liberta para o jogo todas as teclas e botões do rato que tinha
  premidos. O movimento
  (giroscópio) não é bloqueado; o Astral Chain não o usa (escolha minha, a
  confirmar no teste).
- **Botões da combinação.** O jogo pode ver L3 e R3 premidos durante um instante
  antes de a combinação fechar. Aceite pelo utilizador ("se estiver livre"); se o
  teste mostrar efeito no jogo, o builder decide como o suprimir.
- **Ao fechar** (escolha minha): para o jogo, tudo começa largado. Um botão ou
  tecla só conta quando for premido de novo; os sticks retomam no primeiro
  movimento. O estado do controlador emulado é reposto a zero ao fechar, e o
  overlay só fecha na libertação da combinação, para L3/R3 não chegarem ao jogo.
  Nenhum botão pode ficar preso.
- **Navegação.** `ImGuiConfigFlags_NavEnableGamepad` com o backend SDL3: direcional
  ou stick esquerdo para mexer, A confirma, B volta, L/R trocam de separador, stick
  direito faz scroll. O backend SDL3 do ImGui recebe os eventos de gamepad
  reencaminhados pelo main thread pela fila de 4.1.
- O `WaitEvent` processa hoje um evento por chamada, com timeout de 100 ms. Com o
  overlay aberto, esvazia a fila de eventos em cada chamada.

### 4.4 Dados

- **Tempos de frame.** Um anel de timestamps escrito em `OnFrameDisplayed` (thread
  GPU, já existe e é usado pelo TAS). Daí saem fps, tempo de frame, 1% low e a
  percentagem de frames de 33 ms ou mais, com as mesmas definições do bench
  (`tools/acre/bench.py`). Não usa `GetAndResetPerfStats`, que reinicia contadores
  e já tem um consumidor (título da janela / `--show-fps`).
- **CPU por thread.** Uma vez por segundo, ler `/proc/self/task/<tid>/stat` das
  threads `CPUCore_0..3`, `GPU` e `VulkanWorker`, e calcular a percentagem de um
  core. É isto que aparece como "GPU" (carga das threads do emulador).
- **AOT.** Um getter novo em `recomp_diagnostics` devolve uma cópia de: blocos
  executados por core e sítios de fallback (módulo, offset, razão, contagem). O
  interruptor único de hoje divide-se em dois (escolha minha): os sítios de
  fallback passam a ser registados sempre, porque esse caminho já é o lento (JIT);
  blocos e tempos por core continuam só com `--recomp-stats`, para não pôr nada no
  despacho quente. Sem a opção, o separador mostra "n/a (run with
  --recomp-stats)" nesses campos. "Roots novos" é o
  número de sítios `missing_block` da sessão: o export cobre todos os roots
  conhecidos, por isso cada um destes é candidato a root novo.
- **Log.** Um backend de memória em `common/logging.cpp` (lista fixa em
  `ForEachBackend`) guarda as últimas 500 linhas, com mutex próprio.
- **FM2.** Os getters C que já existem (`acre_fm_faults`, `acre_fm_demotions`)
  aparecem no separador AOT.

### 4.5 Definições e o ini

| Chave | Aplicação |
|---|---|
| `vsync` | imediata (o swapchain recria-se sozinho) |
| `scaling_filter` | imediata |
| `volume` | imediata |
| `fullscreen` | imediata (implementar `SDL_SetWindowFullscreen` em runtime) |
| `fastmem_demote_after` | imediata |
| controlos (preset) | imediata (`players[0]` + `HIDCore().ReloadInputDevices()`) |
| `reactive_flushing` | parcial: imagens novas; o overlay mostra "full effect after restart" |
| `fps60`, `resolution_scale`, `fastmem`, `display_index` | "restart required" |
| `[overlay]` (abaixo) | imediata |

Chaves novas, secção `[overlay]`:
- `key = Insert` (nome de tecla SDL);
- `pad = l3+r3` (botões SDL gamepad separados por `+`);
- `fps_counter = false` (mini contador; desligado por defeito, escolha minha).

**Guardar.** Só com o botão "Save to ini" (escolha minha). Não usa o SimpleIni
para escrever, porque ele perde comentários soltos. Uma função pura reescreve o
texto do ficheiro linha a linha: troca o valor das linhas `chave = valor`
existentes na secção certa, mantém tudo o resto byte a byte, e acrescenta no fim da
secção (ou numa secção nova) as chaves que faltam. Grava num ficheiro temporário e
renomeia. Escreve no mesmo ficheiro que `FindAcreIni` encontrou, incluindo um ini
ao lado do executável. Se não havia ini nenhum, cria
`$XDG_CONFIG_HOME/astralchain/astralchain.ini` (ou `~/.config/...`) a partir do
modelo comentado `frontend/astralchain.ini` embutido no binário, e aplica as
alterações por cima (escolha minha).

## 5. Os separadores

```
┌─ AstralChainRe ──────────────── Insert / L3+R3 ┐
│ [Performance] [Settings] [AOT] [Log]            │
│                                                 │
│  57.2 fps   frame 17.5 ms   1% low 28           │
│  ▁▂▁▁█▁▁▂▁▁▁█▁▁▁▁▂▁▁  (last 10 s)              │
│  frames ≥ 33 ms: 12%                            │
│  CPUCore_0 78%  CPUCore_1 86%  CPUCore_2 50%    │
│  GPU 36%  VulkanWorker 9%                       │
└─────────────────────────────────────────────────┘
```

- Painel semitransparente no lado esquerdo, por cima do jogo.
- **Performance:** a figura acima. Gráfico do tempo de frame dos últimos 10 s com
  linhas em 16.7 e 33.3 ms.
- **Settings:** cada chave do ini com o seu controlo, a etiqueta "restart required"
  ou "full effect after restart" quando aplicável, e "Save to ini".
- **AOT:** blocos por core, fallbacks por sítio (tabela), roots novos, faults e
  despromoções do FM2.
- **Log:** últimas 500 linhas, filtro por nível, scroll com o stick direito.
- **Mini contador:** "57 fps" num canto, fonte pequena, sem fundo opaco. Usa o
  mesmo renderer com um frame ImGui mínimo. Como está desligado por defeito, o
  anti-objetivo "custo zero fechado" mede-se com ele desligado; ligado, conta para
  o mesmo orçamento de < 0.3 ms do overlay aberto (escolha minha).

## 6. Erros

- `Init` do Vulkan falha: aviso no log, overlay desligado, jogo continua.
- O ficheiro ini não pode ser escrito: mensagem no separador Settings, o ficheiro
  antigo fica intacto (o temporário é descartado).
- `/proc` ilegível: a linha de CPU mostra "n/a".
- Formato do frame muda (swapchain recriado): o overlay recria o seu pipeline.

## 7. O que fica com o builder

- Versão exata do Dear ImGui (release recente estável).
- Organização dos ficheiros do frontend e nome da biblioteca do overlay.
- Fonte (a do ImGui por defeito serve) e escala em ecrãs HiDPI.
- Como copiar o ImDrawData para o lambda (cópia profunda própria ou um helper).
- Como suprimir L3/R3 no jogo, só se o teste mostrar que é preciso.

## 8. Testes e critério de fecho

**Automáticos (ctest)**
- Reescrita do ini: troca de valor, chave em falta, secção em falta, comentários e
  linhas em branco intactos, chaves com o mesmo nome noutra secção, ficheiro sem
  newline final, criação a partir do modelo quando não há ficheiro.
- Estatísticas de frame: fps, 1% low e percentagem ≥ 33 ms contra os mesmos casos
  do teste do bench.
- Detetor de combinação: abre só com os dois premidos, não repete enquanto
  premidos, fecha na libertação, ordem de pressão indiferente, combinações de 1 e 3
  botões.
- Parser das chaves `[overlay]` no `test_acre_ini`.
- Getter do `recomp_diagnostics`: cópia coerente com os registos.

**Bench**
- Combate, overlay fechado contra o build anterior: sem perda na média, 1% low e
  paragens (anti-objetivo).
- Combate com o overlay aberto (opção de linha de comando para o abrir no
  arranque, só para medir): custo < 0.3 ms por frame. O mesmo com só o mini
  contador ligado.

**Manual (linhas novas na checklist)**
- Insert e L3+R3 abrem e fecham; o jogo não reage ao comando com o overlay aberto;
  nenhum botão fica preso depois de fechar.
- Navegação completa com o comando.
- Cada opção "imediata" muda de facto ao vivo; "Save to ini" grava e mantém os
  comentários.
- O jogo continua a correr por trás.

Fecha o M5 (e a v1) quando os testes passam, o bench cumpre os dois critérios e a
checklist manual passa com o utilizador.
