# AstralChainRe — design

Data: 2026-10-14
Estado: aprovado em conversa, por rever por escrito

## 1. Objetivo

Recompilação estática (AOT) de Astral Chain (Switch, `01007300020FA000`) num executável
nativo para Linux x86_64, reaproveitando o backend AOT já desenvolvido em `suyu-v0.0.4`
(branch local `aot-recomp`, commit `fb50690ed2`).

Uso pessoal primeiro, mas com a estrutura pronta para uma release pública futura
(opção "c"): nunca entra código ou assets do jogo no repositório, os paths são
configuráveis, a licença é definida desde o início e o pipeline é reproduzível.

### Critérios de sucesso

- O jogo arranca num executável próprio (`astralchain`), sem UI do suyu.
- Corre 100% AOT (0 fallbacks JIT) na checklist de segmentos (secção 8).
- Corre em Vulkan, em Linux x86_64.
- 60fps opcional, com cutscenes à velocidade correta.
- Resolução interna configurável.

### Fora de âmbito (por agora)

- Windows, macOS, Steam Deck com perfil próprio, Android.
- Versão 1.0.0 do jogo.
- Ultrawide 21:9.
- Qt, RmlUi, core libretro.
- Rede, multiplayer, web service.

## 2. Ponto de partida

Já existe em `suyu-v0.0.4`:

- `src/core/recompiler/arm64_to_c.h`: lifter AArch64 para C (estilo N64Recomp), com
  descoberta de funções por BL, branches diretos, ADRP+ADD e jump tables.
- `src/core/arm/recomp/`: `ArmRecomp`, uma implementação de `ArmInterface` que executa
  os blocos recompilados sobre o kernel HLE, os services e a GPU reais do suyu, com
  fallback para o dynarmic quando falta um bloco. Inclui também os diagnósticos.
- `tools/aot/`: `build_game.py`, `export_module.cpp` e testes por instrução.
- Validações em `artifacts/aot-validation/`:
  - `aot-zero-vulkan.json` mostra 0 fallbacks e 0 ns de JIT.
  - As sessões jogadas terminam em `window_closed`.
  - As corridas headless strict terminam em `guest_panic`, e o mesmo acontece com o JIT
    (`jit.json`). Isto é um problema do ambiente headless, não do AOT.
  - O último build do export (`build-chain.log`) falhou. Demorou cerca de 4m23 de tempo
    real e 61 min de CPU. O C gerado do `main` ocupa 392M em 63 ficheiros.
- O jogo tem só três módulos: `rtld`, `main` e `sdk`. Não tem subsdk.

Referências externas:

- LASRecomp (`peiikai/LASRecomp`, MIT): modelo de pipeline "o utilizador gera o export
  a partir do seu próprio dump" e de patches por endereço. Não reaproveitamos código.
- HWDER (`sonsegajp/HWDER`): referência para fallbacks e entradas extra.
- `yogesh1239/AstralChain-Dynamic-FPS/docs/TECHNICAL.md`: endereços de present interval
  e timestep para a 1.0.1.
- Eden: fonte de correções para cherry-pick manual. Não é uma dependência.

## 3. Decisões

| Tema | Decisão |
|---|---|
| Relação com o suyu | Vendoring em `runtime/` via `git filter-repo`, com histórico. Nós passamos a manter o runtime. |
| Licença | GPL-2.0-or-later, herdada do suyu. |
| Frontend | SDL3 com `.ini` (fase 1) e overlay ImGui (fase 2). Única UI. |
| Plataforma | Linux x86_64. |
| Versão do jogo | Só a 1.0.1 (base `[v0]` com update `[v65536]`). Qualquer outra é recusada. |
| Melhorias | 60fps e resolução interna. |
| Compilador do C gerado | clang do sistema. GCC é aceite mas não testado. |

## 4. Estrutura do repositório

```
AstralChainRe/
├── CMakeLists.txt              raiz do CMake (vinda do suyu)
├── CMakeModules/ cpmfile.json  infraestrutura de build do suyu
├── externals/                  externals mantidos
├── LICENSE.txt                 GPL-2.0-or-later
├── runtime/                    antigo src/ do suyu (filter-repo, com histórico)
│   ├── common/ core/ video_core/ audio_core/ hid_core/ input_common/
│   └── shader_recompiler/ network/ nintendo_library/ frontend_common/ dynarmic/
├── recompiler/                 arm64_to_c.h + export_module
├── game/                       específico do Astral Chain
│   ├── version.h               hashes dos segmentos da 1.0.1 (rtld/main/sdk)
│   ├── patches/                *.toml, patches por endereço
│   └── roots/                  seeds de funções extra
├── frontend/                   SDL3 + .ini, e mais tarde o overlay ImGui
├── tools/
│   ├── acre.py                 orquestrador do pipeline
│   └── tests/                  testes do lifter (vindos de tools/aot)
├── docs/
└── work/                       no .gitignore: exports, builds, logs
```

A raiz do CMake, `CMakeModules/`, `cpmfile.json` e `externals/` ficam na raiz do
repositório, e não dentro de `runtime/`, porque o CMake do suyu depende de
`CMAKE_SOURCE_DIR` (cache do CPM, `externals/`, `cpmfile.json`). O dynarmic ativo é
`runtime/dynarmic`. O submódulo `externals/dynarmic` não era usado e saiu.

Limites entre módulos:

- `runtime/` não conhece o Astral Chain.
- `game/` liga-se ao runtime só por patches e roots, consumidos em tempo de export.
- `frontend/` usa apenas a API de `Core::System` e as definições do runtime.
- Nada derivado do dump (código, rodata, assets, manifest) sai de `work/` ou dos
  diretórios XDG do utilizador.

## 5. Vendoring do runtime

### Entra

- De `src/`: `common`, `core` (incluindo `core/recompiler` e `core/arm/recomp`, que são
  movidos para `recompiler/` quando a separação for limpa), `video_core`,
  `shader_recompiler`, `audio_core`, `hid_core`, `input_common` e `network` (desligado
  por defeito).
- `frontend_common` entra só se for útil para o `.ini`. A decisão é tomada na
  implementação.
- dynarmic: confirmar se o ativo é `src/dynarmic` ou `externals/dynarmic` e manter
  apenas esse, como código vendored. Continua a ser o fallback JIT.
- `tools/aot/` passa a `tools/tests/` (testes) e a `recompiler/` (`export_module.cpp`).
- Externals: SDL, Vulkan-Headers, VulkanMemoryAllocator, Vulkan-Utility-Libraries, sirit,
  SPIRV-Tools, xbyak, mbedtls, opus, cubeb, ffmpeg, nx_tzdb, simpleini, stb, bc_decoder,
  unordered_dense, FidelityFX-FSR, glad e gamemode. São vendored ou obtidos via CPM/vcpkg
  como no suyu.

### Fica de fora

- `suyu`, `yuzu`, `qt_common`, `suyu_cmd`, `yuzu_cmd`, `android`, `libretro_core`,
  `dedicated_room`, `yuzu_room_standalone`, `web_service` e `tests`.
- Externals: breakpad, discord-rpc, cpp-jwt, libusb, libadrenotools, metal-cpp,
  nxdumptool, nx-optimizer, ownfoil, nxapi, sse2neon e oaknut.
- Ficam, ao contrário do que esta lista dizia inicialmente:
  - `renderdoc`: só um header, usado por `core/tools/renderdoc.cpp`.
  - `microprofile`: usado por `common/microprofile.*`.
  - `enet` e `cpp-httplib`: vêm via CPM, porque `network` e `nintendo_library` precisam
    deles.
- `nintendo_library` e `network` ficam no runtime, porque o `core` depende deles.

### Processo

1. Fazer um clone temporário do `suyu-v0.0.4` no branch `aot-recomp`. O original não é
   tocado.
2. Correr `git filter-repo` com os paths que entram, renomeados para `runtime/`.
3. Fazer merge desse histórico no `AstralChainRe`.
4. Remover código morto (por exemplo, chamadas a `web_service`, applets que dependem de
   Qt, hooks do libretro) em commits próprios, com o build verde depois de cada um.

## 6. Pipeline (`tools/acre.py`)

| Comando | Faz |
|---|---|
| `setup --base <v0.nsp> --update <v65536.nsp> --keys <prod.keys>` | Extrai o ExeFS com o update aplicado e valida os hashes contra `game/version.h`. Escreve os paths em `work/game.toml`. |
| `export` | Corre o lifter sobre `rtld`, `main` e `sdk`, com seeds de `game/roots/` e os patches de `game/patches/`. Escreve em `work/export/`. |
| `build` | Corre CMake e Ninja e produz `work/bin/astralchain`. O export é uma biblioteca estática separada do resto. |
| `run [--validate]` | Lança o jogo. `--validate` corre em modo headless strict e escreve um JSON de validação no formato atual. |
| `roots --from <run.json>` | Acrescenta os `fallback_sites` de uma corrida a `game/roots/`. |

Regras:

- O trabalho pesado fica em C++ (`export_module`). O Python só orquestra.
- O export só é regenerado quando o hash de (lifter, roots, patches, módulos) muda. Esse
  hash é guardado no manifest.
- Alterações ao frontend ou ao runtime não recompilam o C gerado.
- Os erros são explícitos, cada um com o seu código de saída: keys em falta, update em
  falta, hash diferente da 1.0.1, compilador ausente ou abaixo da versão mínima.
- A versão mínima do clang é fixada em M2, com base no clang que compilar o export
  atual sem erros.

## 7. Patches e melhorias

### Formato

`game/patches/<nome>.toml`:

- `target`: `main+0xOFFSET`.
- `expect`: bytes originais.
- `kind`: `insn` (substitui instruções AArch64), `c` (substitui um bloco por C) ou
  `hook` (chama C antes da função, ou em vez dela).
- `toggle`: nome do campo em `acre_cfg`, ou nada se o patch for sempre ativo.

O patch é aplicado em tempo de lift. Se os bytes não baterem com `expect`, o export
falha. Um patch com toggle é emitido como `if (acre_cfg.<toggle>) { novo } else
{ original }`, o que permite ligá-lo e desligá-lo sem rebuild.

### 60fps (`fps60`)

1. O present interval (`nvnWindowSetPresentInterval`) passa de 2 para 1.
2. O timestep variável deixa de estar preso ao clamp `[1,2]` e passa a meio frame.
3. Cutscenes: hoje usam um passo fixo, e é isso que as põe a 2× com os patches
   existentes. É preciso encontrar, por reverse engineering, o passo fixo ou a flag de
   cutscene e fazê-lo seguir o dt real.
   - Esta é a tarefa com mais incerteza. Fica concluída quando a duração de uma cutscene
     medida com timer for igual a 30fps e a 60fps, dentro de ±2%.
4. Validação: comparar o mesmo segmento a 30fps e a 60fps (duração das cutscenes,
   distância de saltos e dash, timers de combate).

Os endereços do TECHNICAL.md são confirmados contra o nosso `main`, cujo `.text` termina
em `0xA0E7D0`, como diz o documento.

### Resolução interna

- Usa o resolution scaler do `video_core`, sem patches ao jogo.
- `.ini`: `resolution_scale` (1–4) e `scaling_filter` (por exemplo FSR ou bilinear).
- Risco: render targets de pós-processamento (bloom, DOF) podem não ser escalados ou ser
  escalados mal. Se acontecer, corrige-se no `video_core`.

## 8. Frontend

### Fase 1 (SDL3 + `.ini`)

- Um executável com janela Vulkan. Abre no monitor primário por defeito, com
  `display_index` no `.ini`.
- O `astralchain.ini` é procurado ao lado do binário e depois em
  `$XDG_CONFIG_HOME/astralchain/`. Se não existir, é criado com os defaults comentados.
- Chaves do `.ini`: paths, `fps60`, `resolution_scale`, `scaling_filter`, `vsync`,
  `fullscreen`, `display_index`, mapeamento de controlos (gamepad SDL com defaults para
  Xbox e PS, mais teclado) e volume e dispositivo de áudio.
- NAND virtual e saves em `$XDG_DATA_HOME/astralchain/`.
- Erros fatais aparecem numa caixa de mensagem SDL e ficam em `astralchain.log`.

### Fase 2 (overlay ImGui)

- F1 mostra e esconde o overlay.
- Toggles do `.ini`, aplicados em runtime quando possível. Os outros mostram "requer
  reinício".
- FPS e frametime, contadores AOT (blocos e fallbacks por sítio) e log.

## 9. Testes

- **Lifter:** os testes de `tools/tests/` correm em `ctest` e comparam o resultado de
  cada instrução com o dynarmic.
- **Patches:** verificação de `expect` em tempo de export.
- **Smoke:** `acre.py run --validate` em headless strict durante 3 minutos. Passa se não
  houver fallbacks nem crash.
  - Pré-requisito: perceber o `guest_panic` headless, que também acontece com o JIT. É a
    primeira tarefa de M2.
- **Manual:** checklist de segmentos (menu inicial, prólogo, primeiro combate, primeira
  cutscene, save e load, Legion summon). Corre a 30fps e, a partir de M4, a 60fps.

## 10. Milestones

1. **M1 Fundação:** repositório git, vendoring do runtime com histórico, build limpo do
   runtime e do frontend mínimo, testes do lifter verdes.
2. **M2 Pipeline:** `acre.py setup/export/build/run` funciona de ponta a ponta.
   - Corrigir a falha de build do export.
   - Resolver o `guest_panic` headless, para que `--validate` seja fiável.
3. **M3 Jogável a 30fps:** 0 fallbacks na checklist, saves funcionam, controlos
   configuráveis pelo `.ini`.
4. **M4 Melhorias:** `fps60`, com as cutscenes a passar no critério de ±2%, e resolução
   interna.
5. **M5 Overlay ImGui.**
6. **Depois:** Windows, empacotamento e release, cada um com uma spec própria.

## 11. Riscos

| Risco | Mitigação |
|---|---|
| Timestep das cutscenes difícil de encontrar | Tarefa de investigação com critério mensurável. Até lá, o `fps60` pode baixar para 30 durante cutscenes, se a flag for encontrada antes do passo fixo. |
| Bugs gráficos conhecidos do jogo em yuzu/suyu (ASTC, nevoeiro, "grey uniform") | Herdamos o `video_core` e as correções conhecidas. Cherry-pick do Eden quando for preciso. |
| Tempo de compilação do export (cerca de 1 h de CPU) | Export como biblioteca separada, regenerado só quando o hash muda. |
| Manter um runtime que já não tem upstream | Vendoring com histórico. Eden como fonte de correções. O âmbito fica limitado a um jogo. |
| Licença | GPL-2.0+ em todo o projeto. Nenhum código de fontes incompatíveis. |
