# Fastmem por arena (FM2) — design

Data: 2026-10-15
Estado: **revisão 2**. Inclui os resultados da F0, a prova de conceito e a revisão
dos seis pontos pedidos. Ainda não há código de produção.
Branch: `m4-60fps`, base `a46cfe0a9`

Alterações face à revisão 1:
- A F0 mediu 0.4–2.0M leituras/s a páginas da GPU. Com o desenho da revisão 1,
  cada uma daria um fault. **Isso falha o gate** (secção 5).
- Correção: cada função levantada passa a ter **a sua palavra de limite**, numa
  secção de dados. O handler **despromove** a função quando um acesso dela atinge
  N faults (secção 3.3). O limite global `fm_arena_limit` desaparece.
- A secção 10 trata os seis pontos da revisão: sincronização com a GPU, reserva
  `PROT_NONE`, limites, registos e stores, sinais e threads, e custo da tabela.
- A PoC passou a cobrir a despromoção. Os números vêm de lá: fault ~0.67 µs (não
  2–5 µs), init da tabela ~52 ms.

## 0. Resumo

Os helpers de memória (`recomp_load*`/`recomp_store*`) gastam **30.7% dos ciclos**
em combate. O jogo está limitado pelo número de instruções, não pela memória:
IPC 2.7–2.8.

Cada acesso do guest custa hoje cerca de 23 instruções:
- passagem de argumentos e `call`;
- comparação com `fm_limit`;
- teste de cruzamento de página;
- load da entrada da tabela de páginas, máscara e teste de null;
- o acesso propriamente dito e o `ret`.

O FM2 troca isto por **3 instruções inline**, sobre a arena virtual que o suyu já
mantém (`VirtualBasePointer`):

```
cmp  a, lim_bloco(%rip)     ; fora da arena, ou bloco despromovido -> slow
jae  slow
mov  (arena,a), v           ; o acesso; se fizer fault -> slow
```

- Se o `mov` fizer fault (página protegida para a GPU, de debug ou não mapeada),
  um handler de SIGSEGV encontra o RIP numa tabela de exceções e muda-o para o
  label `slow` do próprio sítio do acesso.
- O slow path é o helper atual, chamado em contexto normal e não dentro do sinal.
- O handler conta os faults de cada sítio. Quando um sítio chega a N faults,
  escreve 0 na palavra de limite da função. A partir daí, essa função vai sempre
  pelo helper, sem faults: o custo é o de hoje.
- O handler não decodifica instruções e não chama código do emulador.

## 1. Estado atual (factos verificados)

### 1.1 Arena: `VirtualBasePointer`

- **Criação.** `Core::DeviceMemory` cria
  `buffer{GetIntendedMemorySize(), VirtualReserveSize}` (`core/device_memory.cpp:9-17`).
  - Sem NCE (o caso de x86), a reserva é de `1<<39` (512 GiB).
  - `virtual_base` fica alinhado a hugepage. O offset de alinhamento já está
    incluído em `VirtualBasePointer()` (`common/host_memory.cpp:700-707`).
- **Backing.** O backing é `memfd_create` + `MAP_SHARED`
  (`host_memory.cpp:509-551`). A arena e o backing apontam para as mesmas páginas
  físicas, por isso estão sempre coerentes.
- **Mapeamento.** `guest_va == offset na arena`: `Map(GetInteger(base), …)`
  (`core/memory.cpp:77-78`). Logo `VirtualBasePointer() + va` aponta para a página
  do guest, sem base extra.
- **O emulador não lê a arena.** A tabela de páginas aponta para o backing
  (`MapPages(…, target, Memory)`, `memory.cpp:74-75`), não para a arena. Por isso
  as proteções da arena **nunca** afetam o slow path (`Core::Memory`). Isto é a
  base da correção do FM2: o slow path funciona sempre, seja qual for a proteção
  da arena.
- **Quando a arena é usada.** Só quando `fastmem_arena` está definido:
  `IsApplication() && Settings::IsFastmemEnabled()` (`memory.cpp:53-54`), que é
  verdade em x86 Linux com `cpu_accuracy = Auto` (`common/settings.cpp:187-201`).
  - **A arena já é mantida hoje** em todas as nossas execuções.
  - O fallback JIT (dynarmic) usa-a como `fastmem_pointer`
    (`arm_dynarmic_64.cpp:227-234`).
- **Reserva inicial.** **A reserva inicial é RW, não `PROT_NONE`:**
  `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`
  (`host_memory.cpp:449-450`).
  - Só os intervalos que passaram por `Unmap` ficam `PROT_NONE`
    (`host_memory.cpp:596-609`).
  - Um VA do guest que nunca foi mapeado lê zeros e aceita escritas em silêncio,
    em páginas anónimas privadas que o emulador não vê. Ver a secção 10.2.

### 1.2 Proteções na arena

| Tipo de página (tabela de páginas) | Permissão na arena | Origem |
|---|---|---|
| Memory | permissões do kernel (RW ou R; sem X em x86) | `MapMemoryRegion` `memory.cpp:77-79`, `ProtectRegion` `94-123` |
| RasterizerCachedMemory, reactive flushing **ligado** (por defeito; é o nosso caso) | `PROT_NONE` | `RasterizerMarkRegionCached` `memory.cpp:452-461` |
| RasterizerCachedMemory, reactive flushing desligado | `PROT_READ` | idem |
| DebugMemory (watchpoints) | `PROT_NONE` | `MarkRegionDebug` `397-401` |
| Unmapped (depois de `Unmap`) | `PROT_NONE` | `Unmap` `host_memory.cpp:596-609` |
| Nunca mapeada | **RW** (reserva inicial) | 1.1 |

- `use_reactive_flushing` está **ligado**: é o valor por defeito
  (`settings.h:512-519`) e está assim em `profile/config/sdl2-config.ini:1107`.
- As marcações para a GPU acontecem só nas transições 0↔1 de um refcount de 8
  bits por página (`device_memory_manager.inc:544-620`), não em cada escrita.
- Numa escrita pelo caminho lento, `Memory::Write` → `HandleRasterizerWrite` →
  `GPU::OnCPUWrite` (`memory.cpp:697-734`, `vk_rasterizer.cpp:721-735`).
- Numa leitura pelo caminho lento, `HandleRasterizerDownload` → `OnCPURead`
  (`memory.cpp:679-695`).
  - Há uma cache por core da última área limpa (`rasterizer_read_areas`). Uma
    leitura dentro dessa área não chama a GPU.

### 1.3 Caminho atual (FM-lite)

- `_recomp_fm` (`arm64_to_c.h`, "Fast path") lê a entrada de 32 bytes de
  `fm_table`. Um ponteiro nulo (páginas Unmapped, Debug ou RasterizerCached)
  manda o acesso para `memload`/`memstore` → `h->load`/`h->store` →
  `Core::Memory`, que avisa a GPU.
- Contagens no export atual:

  | Módulo | Sítios de acesso | Funções com acessos |
  |---|---|---|
  | `main` | 927 023 | 342 245 |
  | `sdk` | 521 816 | 164 646 |
  | `rtld` | 737 | 199 |
  | **Total** | **1 449 576** | **507 090** |

  Em `main`, por helper: `load64` 367 561, `store64` 226 385, `load32` 186 412,
  `store32` 112 070, `load8` 17 411, `store8` 7 328, `load16` 6 407,
  `store16` 3 449.
- Os acessos de 128 bits (Q, LDP/STP Q, LD1…) já são compostos por dois
  `load64`/`store64`.
- Os exclusivos (LDXR/STXR) vão por `HostExclusive` e o monitor, não pelos
  helpers.

### 1.4 Sinais

- **Handler do dynarmic** (`exception_handler_posix.cpp:66-186`):
  - é registado de forma lazy, na primeira vez que um JIT com fastmem é criado
    (`RegisterHandler`, linha 118);
  - em Linux instala **só SIGSEGV** (o SIGBUS é só para macOS);
  - usa `SA_SIGINFO|SA_ONSTACK|SA_RESTART`, com uma sigaltstack de 2 MiB só na
    thread que o registou;
  - **dentro do sinal**, toma um `std::shared_lock` sobre os blocos JIT. Se o PC
    não for seu, **escreve `Unhandled SIGSEGV at rip …` no stderr** com
    `fmt::print`, e só depois encadeia no handler anterior.
- O handler do `arm_dynarmic.cpp:14-45` é código morto: ninguém o regista.
- Nenhum código do `core/` ou do `common/` bloqueia SIGSEGV com
  `pthread_sigmask`/`sigprocmask`.
- As fibers usam stacks de `DEFAULT_STACK_SIZE` (`common/fiber.cpp:29`), sem
  sigaltstack.
- Não há `thread_local` no backend recomp.

### 1.5 Medições que motivam isto

Combate, `a46cfe0a9`, **antes** do performance mode:
- 166G instruções e 59G ciclos em 5s: **IPC 2.8**.
- Misses: dTLB 10.6M, L1D 774M.
- Distribuição dos ciclos:
  - `blk_*`: 32.4%;
  - helpers de memória: **30.7%** (`load32` 11.5, `load64` 8.8, `store64` 6.9,
    `store32` 2.4, …);
  - resto: 21.4%.
- Dentro de `recomp_load32`, os ciclos estão espalhados de forma uniforme pelas
  ~20 instruções. Não estão concentrados no load dos dados.
- THP na memória do guest não mudou nada: 3.4 GB em hugepages, mesmos FPS.
  Experiência revertida.

**Baseline congelada** para todas as fases do FM2 (performance mode):

| | |
|---|---|
| Commit | `a46cfe0a9` |
| Binário | `work/baselines/astralchain-static-a46cfe0a9`, sha256 `dc0a1f71e5ace25285222d5ada3f465f0a06613c3a8d56d497ea4db9d9f71410`, 350 237 672 B |
| Build | Release. C levantado com clang 22.1.8, `-march=x86-64-v3 -ffp-contract=off`. C++ com GCC 16.2.1 |
| Configuração | `ab60.ini` (fps60, vsync, `resolution_scale` 1, FSR). suyu com `cpu_accuracy` Auto, reactive flushing ligado, fastmem ligado |
| Cenário | a zona de combate habitual, com vários inimigos, em performance mode |
| FPS | maioria a **40**, quedas a 37, mínimo 32 por pouco tempo, máximo 45 |
| `perf stat`, 10s | task-clock 26.7s (~2.67 cores), cycles 122.4G, instructions 335.9G, **IPC 2.74**, dTLB-load-misses 27.3M |

## 2. Comparação com o FM1 do mk8-recomp

Fonte: `dougchansan/suyu-v0.0.4` na tag `v0.0.11`
(`src/core/recompiler/arm64_to_c.h`, `src/core/arm/recomp/arm_recomp.cpp`,
`docs/releases/v0.0.11.md`), usado só como referência e sem copiar código.

| | FM1 (mk8-recomp) | FM-lite (nosso, `b3114f422`) | FM2 (proposta) |
|---|---|---|---|
| Fonte do endereço | entrada da `Common::PageTable` | igual | arena `VirtualBasePointer` |
| Sítio do acesso | `call` a um helper | `call` a um helper | inline: `cmp`/`jae`/`mov` |
| Verificações por acesso | limite, cruzamento de página, load da entrada, null | igual | só o limite do bloco |
| Página GPU/debug/unmapped | entrada nula → slow path | igual | fault → tabela de exceções → slow path; despromoção do bloco ao fim de N faults |
| Handler de sinais | nenhum | nenhum | sim; muda o RIP e, às vezes, escreve uma palavra de limite |
| Cruzamento de página | slow path | slow path | hardware; fault só se a 2.ª página estiver protegida |
| Ganho medido | **+4%** (34.6→36.0 fps, notas da v0.0.11) | 0% sozinho (24 fps), mas é a base dos ganhos seguintes | estimativa na secção 8 |
| Inline tentado | sim: imagem 100→222 MB, **−14% FPS** | `RECOMP_FM_INLINE`: +96 MB, 0% | inline de 3 instruções, ~20 B por sítio |

**Conclusões:**
- **O FM1 é, na prática, o FM-lite que já temos.** Não há nada para reaproveitar
  no fast path, e o ganho deles (+4%) confirma o teto desta família de soluções.
- As duas tentativas de inline (a deles e a nossa) falharam pelo mesmo motivo:
  meteram o **lookup inteiro** (~15 instruções e um load dependente) em cada
  sítio. O FM2 só é diferente se o inline for mínimo. Por isso as verificações
  saem do caminho rápido e passam para o hardware (MMU).
- **Ideias do FM1 que vale a pena adotar**, como desenho e não como código:
  - um slow path que é literalmente o helper atual, para os dois caminhos nunca
    divergirem;
  - um handshake de layout no arranque (já temos o equivalente nos
    `typedef char recomp_layout_*[...]` e nos `static_assert`);
  - um teste diferencial de fast contra slow com log de callbacks.
- **Riscos do FM1 que evitamos:** o FM1 lê a entrada da tabela de páginas sem
  ser atómico e não protege as corridas com `Unmap` ou com a marcação da GPU. No
  FM2, uma corrida dessas acaba num fault e o slow path resolve-a.

## 3. Arquitetura

### 3.1 Novos campos no contexto (ABI 6)

Acrescentado ao `GuestContext`, depois de `block_cache` (que termina em
872 + 8192·16 = 131944), e espelhado em `GuestContextView`, com offset fixado dos
dois lados:

```c
uint8_t* fm_arena;   /* @131944: VirtualBasePointer(), ou NULL = FM2 desligado */
```

- `RunThread` atualiza-o em cada entrada, como já faz com `fm_table`/`fm_limit`,
  a partir de `page_table.fastmem_arena`.
- **Não há limite no contexto.** O limite vive em palavras por função (3.3).
- Desligar o FM2 em run time (arena nula, kill switch, falha de instalação) é
  escrever 0 em todas as palavras de limite, uma vez, antes de o guest correr
  (3.4). Nenhum acesso chega então a ler `fm_arena`.

### 3.2 Emissão

- O emissor deixa de chamar diretamente `recomp_load32(c,a)` e passa a emitir
  macros, como `RECOMP_LD32(c,a)` ou `RECOMP_ST64(c,a,v)`.
- Cada função `blk_*` com pelo menos um acesso declara, no início,
  `RECOMP_BLOCK_LIMIT;`.
- O `recomp_runtime.h` resolve as macros:
  - **sem `RECOMP_FASTMEM`** (por defeito): `RECOMP_LD32` → `recomp_load32` e
    `RECOMP_BLOCK_LIMIT` → nada. O código gerado fica igual ao de hoje, com o
    mesmo binário e a mesma ABI de acessos;
  - **com `-DRECOMP_FASTMEM`** (opção CMake `ACRE_FASTMEM`, desligada por
    defeito): `RECOMP_LD32` → `recomp_fx_load32`, uma função `static inline` com
    `asm goto`; `RECOMP_BLOCK_LIMIT` → a palavra de limite da função.
- Os helpers atuais (`recomp_load*` e `recomp_store*`, que usam o FM-lite)
  **ficam como estão** e são o slow path do FM2.

### 3.3 Acesso inline, palavra de limite e despromoção

Esboço de um load de 32 bits. É o código da PoC (`tools/poc/fm2/fm2.h`); os nomes
finais saem do plano.

```c
#define RECOMP_BLOCK_LIMIT \
  static uint64_t fm_lim __attribute__((section("acre_fml"), aligned(8), used)) = \
    RECOMP_FM_LIMIT_INIT   /* (1<<39) - 7 */

static inline uint64_t recomp_fx_load32(GuestContext* c, uint64_t a, uint64_t* lim){
  uint64_t v;
  asm goto("cmpq %c[lim](%%rip), %[a]\n\tjae %l[slow]\n"
           "1: movl %[m], %k[v]\n\t"
           ".pushsection acre_fmx,\"a\"\n\t.balign 4\n\t"
           ".long 1b - ., %l[slow] - ., %c[lim] - .\n\t.popsection"
           : [v] "=r"(v)
           : [m] "m"(*(const acre_u32a*)(c->fm_arena + a)), [a] "r"(a), [lim] "i"(lim)
           : "cc" : slow);
  return v;
slow:
  return recomp_load32(c, a);
}
```

- **Palavra de limite por função.** Cada função levantada com acessos tem uma
  palavra de 64 bits em `acre_fml` (secção `"aw"`), iniciada com o limite da
  arena. O `cmp` lê-a da memória, dentro do próprio `asm`. Por isso o compilador
  não a pode tirar para fora de um loop, e uma despromoção tem efeito no acesso
  seguinte, mesmo a meio de um loop longo.
  - O valor inicial é uma constante de link (`(1<<39) − 7`). No arranque normal
    não é preciso escrever nenhuma palavra, e as 4 MB de `acre_fml` só entram em
    memória quando são lidas.
  - 0 = função desligada: todos os acessos dela vão para o helper.
- **Tabela de exceções** (`acre_fmx`): por sítio, 3×`int32` auto-relativos
  (12 B): a instrução, o label `slow` e a palavra de limite. Não precisa de
  relocações e funciona com PIE e com binário estático.
- **Despromoção.** O handler conta os faults de cada sítio. Quando um sítio chega
  a `N` faults (por defeito 16; configurável; 0 = nunca), escreve 0 na palavra da
  função e incrementa um contador de despromoções.
  - É permanente durante a sessão. Uma função despromovida custa o mesmo que hoje
    (helper + FM-lite). Não há regressão face à baseline, só menos ganho.
  - Porque é por função e não global: a F0 mostrou que os acessos a páginas da
    GPU vêm de poucos sítios (secção 5), mas uma parte do código toca a GPU e o
    resto não. Um limite global desligava tudo; um por sítio custava mais
    memória e mais linhas de cache no caminho rápido.
  - Granularidade: uma função `blk_*` é um bloco básico levantado (poucas
    instruções). Despromover um bloco não afeta os outros blocos da mesma função
    do guest.
- **Tamanhos:**
  - 8 e 16 bits: `movzbl`/`movzwl`;
  - 32 e 64 bits: `movl`/`movq`;
  - stores: `movb`/`movw`/`movl`/`movq`, com o operando `"=m"`.
  - A extensão de sinal continua no C, como hoje.
  - O de 128 bits continua a ser composto por dois acessos de 64 bits.
- **Alinhamento, limites e stores:** secção 10.3 e 10.4.
- **Aliasing:** os tipos `may_alias` impedem o compilador de reordenar acessos ao
  guest com base em strict aliasing. As stores levam o operando `"=m"`; não usamos
  um clobber `"memory"` global, que forçaria a recarregar os `c->x[]`. Os
  `asm goto` são implicitamente volatile.
- **Requisitos de compilador:** `asm goto` com outputs exige clang ≥ 11, e o C
  levantado já obriga a usar clang (`9d4f37bfb`). A PoC compila e passa com o
  clang 22.1.8. Plano B: uma flag de falha (`xor f,f` + fixup `mov $1,f; jmp 2b`
  + `test`/`jne`), com mais 3 instruções.

### 3.4 Handler de SIGSEGV e instalação

- Fica num ficheiro próprio, sem dependências do suyu, para os testes do lifter o
  poderem ligar: `core/arm/recomp/recomp_fastmem.cpp`. A base é
  `tools/poc/fm2/fm2_runtime.c`.
- **Instalação** (`std::call_once`, quando o primeiro `ArmRecomp` liga o FM2,
  antes de o guest correr):
  1. **Forçar primeiro o registo do handler do dynarmic.** O dynarmic regista o
     seu de forma lazy. Se o fizesse depois de nós, ficava por cima. Cada fault do
     FM2 passaria então por um `shared_lock` e por um `fmt::print` no stderr
     antes de chegar a nós (1.4). Acrescenta-se ao dynarmic uma função pública
     que chama o `RegisterHandler()` existente; a ordem fica FM2 → dynarmic →
     ação anterior.
  2. Converter `[__start_acre_fmx, __stop_acre_fmx)` num vetor ordenado de
     `(insn, label, lim)` (offsets de 32 bits a partir da base de `.text`; `lim`
     como índice em `acre_fml`). O linker não garante a ordem.
  3. Validar cada entrada: o `lim` tem de cair dentro de `acre_fml` e estar
     alinhado. Se alguma falhar, o FM2 fica desligado (todas as palavras a 0).
  4. Instalar `sigaction(SIGSEGV)` e `sigaction(SIGBUS)` com
     `SA_SIGINFO|SA_ONSTACK`, guardando as ações anteriores.
  5. Se a arena for nula, se o kill switch estiver ativo, ou se algum dos passos
     falhar: escrever 0 em todas as palavras de `acre_fml` e registar uma linha no
     log. O jogo corre então exatamente como hoje.
- **No sinal:**
  1. Ler `rip = uc_mcontext.gregs[REG_RIP]`.
  2. Fazer uma pesquisa binária na tabela.
  3. Se encontrar a entrada: incrementar o contador do sítio e o contador global
     (atómicos relaxed). Se o contador do sítio passar a `N`, escrever 0 na
     palavra da função (store atómico relaxed de 8 bytes alinhados). Depois pôr
     `gregs[REG_RIP] = label` e retornar.
  4. **Mais nada:** nenhuma chamada ao emulador, nenhum lock, nenhuma alocação.
     Isto é async-signal-safe.
  5. Se não encontrar, encadear no handler anterior com a mesma semântica do
     dynarmic: `sa_sigaction`, `SIG_DFL` (repor e retornar, para o fault se
     repetir e gerar o core dump certo) ou `SIG_IGN`.
- **Telemetria** (barata, fora do sinal): faults/s e despromoções no
  `--show-fps`/log, uma vez por segundo, como o `acre-memstats`.
- **Depuração:** com o FM2 ligado, o gdb vai parar em SIGSEGVs legítimos. O
  README vai dizer `handle SIGSEGV nostop noprint pass`.

### 3.5 Compatibilidade com a GPU

Ver a secção 10.1.

## 4. Fallback seguro e flags

1. **Build:** `ACRE_FASTMEM=OFF` (por defeito) gera um código igual ao atual. Os
   helpers, o FM-lite, a cache de 8192 entradas e o encadeamento ficam intactos.
2. **Run time:**
   - `ACRE_NO_FASTMEM=1`, ou `fastmem = false` no `astralchain.ini`: todas as
     palavras de limite a 0 no arranque. Todos os acessos vão para os helpers.
   - O `cmp` inline continua a ser executado, mas o comportamento é exatamente o
     dos helpers.
   - `fastmem_demote_after = N` no ini (por defeito 16; 0 = nunca despromover,
     para diagnóstico).
3. **Automático:** o FM2 fica desligado, com uma linha no log, se:
   - o suyu tiver o fastmem desligado (`fastmem_arena` nulo);
   - o `sigaction` falhar, ou a tabela for inválida;
   - `VirtualBasePointer` não estiver alinhado a 4 KiB;
   - uma thread de CPU tiver SIGSEGV bloqueado (secção 10.5). Isto é verificado
     na primeira entrada de cada `ArmRecomp` em `RunThread`.
4. **Reserva da arena:** passa a `PROT_NONE` (secção 10.2), só no x86 Linux.
   É a Fase 1, sozinha e validada à parte.
5. **Arquivados e não tocados:**
   - o TLB por software (`experiment/software-tlb`, `ca790ba67`);
   - o FM-inline (histórico até `62ba677ad`);
   - a cache de alvos de 8192 entradas.

## 5. Implementação incremental

Cada fase tem os seus testes e o seu commit. O benchmark de cada fase mede só
essa fase, contra a baseline da secção 1.5.

### F0 — medir, sem mudar comportamento (**feita**)

- Build com `ACRE_MEMSTATS` (opção CMake, desligada por defeito;
  `runtime/core/arm/recomp/recomp_memstats.h`). Conta, em
  `HostLoad`/`HostStore`, os acessos que saem do caminho rápido, pelo tipo de
  página. Também amostra 1 em 64 dos acessos que fariam fault e guarda a stack.
- PoC isolada em `tools/poc/fm2/` (`run.sh OUT_DIR [--bench]`).

**Resultado 1 — acessos a páginas da GPU (gameplay, 120s e 136s):**

| Tipo | Loads/s | Stores/s |
|---|---|---|
| memory (o caminho rápido recusou por outro motivo) | ~180 | ~180 |
| memory-cross (cruzamento de página) | ~6.8k | ~4.3k |
| unmapped, debug, out-of-range | 0 | 0 |
| **rasterizer** | **média 580k; 400k–2.0M em gameplay** | ~5k |

- Os primeiros ~30s (menus e carregamento) têm ~3k/s. O gameplay sobe para
  0.4–2.0M/s.
- Com o desenho da revisão 1, cada um destes loads daria um fault:
  580k × 0.67 µs ≈ **0.39 cores em média, ~1.3 cores no pico**. O gate era menos
  de 1% de um core. **Falhou.** Não é um bloqueio de correção, é de performance.

**Resultado 2 — de onde vêm (1.37M amostras, 1 em 64):**

| Sítios (por ordem) | Fração acumulada |
|---|---|
| 1 | 40.7% |
| 2 | 71.0% |
| 5 | 79.4% |
| 10 | 84.2% |
| 20 | 90.5% |
| 40 | 95.3% |

- Total: 472 stacks distintas.
- Os dois primeiros são os dois loads de 64 bits de um loop de comparação do SDK,
  do tipo `memcmp`, sobre memória que a GPU segue.
- **Consequência:** com despromoção por função, cada sítio paga no máximo N
  faults e passa ao helper. Com ~500 sítios e N = 16: ~8k faults **uma vez**,
  ~5 ms no total. O gate passa.
- Custo da despromoção: o `memcmp` despromovido perde o FM2 também quando compara
  memória normal. Fica ao custo de hoje. A F4 mede quantos acessos ficam em
  funções despromovidas.

**Resultado 3 — PoC** (clang 22.1.8, `-O2 -march=x86-64-v3`, esta máquina):

| Caminho | Custo |
|---|---|
| Helper ao estilo FM-lite | 0.81 ns/acesso |
| FM2 inline, limite por função | **0.24 ns/acesso** (0.20 com o limite global da revisão 1) |
| Fault recuperado | **~0.67 µs** |

- Testes que passam: larguras de 8/16/32/64 bits em load e store; páginas RW, RO
  e NONE; cruzamentos de página; store sem efeito parcial; limites (`−7`, perto
  de 2^64, página sentinela intacta); limite 0 = desligado; 12 valores inteiros e
  6 doubles vivos através de um fault; encadeamento num handler anterior; 4
  threads, uma com sigaltstack; despromoção (N faults, depois sem faults, as
  outras funções intactas, reset); 4 threads a disputar a despromoção do mesmo
  sítio (1 despromoção, mesmos valores).
- Mutantes mortos: sem o `cmp`; label +1; sem mudar o RIP; sem ordenação; sem o
  store do 0; palavra de limite errada; `>=` em vez de `==` no limiar; reset que
  não limpa as contagens.
- **Escala real:** 1 450 000 sítios gerados (o número do export): tabela de
  17.4 MB, init de **51–54 ms**, todos os sítios recuperados.

### F1 — arena `PROT_NONE`

- Mudança só em `ChooseVirtualBase` (`host_memory.cpp`, x86 Linux): reserva com
  `PROT_NONE`.
- Testes:
  - o validate de 90s com 0 fallbacks;
  - fallback JIT forçado (`SUYU_RECOMP_FORCE_MISS_AFTER`), para exercitar o
    fastmem do dynarmic com a nova reserva;
  - a checklist no jogo (6.2).
- De passagem, corrigir o fecho do memfd em `host_memory.cpp:545-547`
  (`fd = -1` antes de `close(fd)` deixa o memfd aberto). Commit à parte.

### F2 — runtime

- `recomp_fastmem.cpp` (tabela, handler, despromoção, encadeamento, registo
  prévio do dynarmic), `fm_arena` no contexto (ABI 6), o kill switch, as chaves
  do ini, a verificação da máscara de sinais e a telemetria.
- Nesta fase o jogo ainda não usa o FM2.

### F3 — emissor

- As macros `RECOMP_LD*`/`RECOMP_ST*`/`RECOMP_BLOCK_LIMIT`, os `recomp_fx_*` em
  `asm goto` e a opção `ACRE_FASTMEM`.
- Reexport e build com o FM2 ligado.

### F4 — benchmarks A/B e decisão

### F5 — `ACRE_FASTMEM=ON` por defeito

Só se a F4 o justificar. Os helpers ficam como fallback permanente.

**Resultado (2026-10-18):** justificado. Numa cena parada e determinista da
cidade (o script TAS acaba sempre no mesmo frame), 4 runs de cada a 1080p:
sem FM2 48.0 fps (47.0–48.7), com FM2 57.8 fps (55.5–59.2), ou seja, +20%. O
`acre.py build` passa `-DACRE_FASTMEM=ON` em Linux x86-64 (`--no-fastmem`
desliga-o), e o default do CMake é ON só em Linux x86-64 com clang.

## 6. Testes

### 6.1 Unidade e lifter (ctest, label `lifter`)

**`test_fastmem_access`** é um programa em C gerado como os outros testes do
lifter. Usa as macros reais do header. É a PoC passada para o ctest.
- **Fixture:** um memfd mapeado duas vezes, como o `HostMemory`: um backing
  sempre RW (o que o slow path usa) e uma arena de 4 páginas, seguida de uma
  página sentinela RW:
  - p0 RW;
  - p1 `PROT_READ`;
  - p2 `PROT_NONE`;
  - p3 RW.
- Os helpers lentos são substituídos por versões que registam as chamadas.
- **Casos:**
  - cada tamanho de load e store em p0 vai pelo caminho rápido, com 0 chamadas
    ao slow;
  - uma store em p1 faz fault → slow com o endereço e o valor certos;
  - um load em p2 → slow;
  - um load que cruza de p0 para p1 → caminho rápido; uma store → slow, sem bytes
    parciais em p0;
  - um load que cruza de p1 para p2 → slow;
  - `a >= limite` → slow, sem tocar na arena; a página sentinela fica intacta;
    endereços perto de 2^64;
  - extensão de sinal (LDRSB/LDRSH/LDRSW) igual à dos helpers;
  - um 128-bit em que a segunda metade faz fault;
  - palavra de limite a 0 → tudo pelo slow path, sem faults;
  - valores vivos em registos inteiros e vetoriais através de um fault;
  - **despromoção:** N−1 faults sem despromover; o N-ésimo despromove; depois
    disso, 0 faults e o slow path dá os mesmos valores; outra função mantém o
    caminho rápido; o reset volta a promover.
- **Encadeamento:** um handler "anterior" que regista a chamada e faz
  `siglongjmp`; um fault fora da tabela tem de lhe chegar. Mais um caso com o
  handler real do dynarmic registado antes de nós.
- **Concorrência:** 4 threads com faults simultâneos, uma com sigaltstack, sem e
  com despromoção (uma despromoção, valores iguais).
- **Escala:** 1.45M sítios gerados, num layout não ordenado.
- **Mutantes que o teste tem de matar:** os oito da secção 5 (resultado 3).

**`test_fastmem_diff`** é um diferencial inspirado na ideia do FM1, com
implementação nossa.
- Corre os mesmos programas dos testes `test_chain` e `test_fmlite`, mais um
  fuzz de acessos aleatórios (tamanhos, offsets junto aos limites de página,
  páginas protegidas, mudanças de proteção entre acessos).
- Compara o FM2 com os helpers: registos, memória e o log de chamadas ao slow
  path têm de coincidir.

**ABI:** os `typedef char recomp_layout_*` e o `static_assert` do offset 131944.

### 6.2 Integração

- Validate headless de 90s com o FM2 ligado: PASS e 0 fallbacks. O relatório
  inclui os faults/s e as despromoções.
- Fallback JIT forçado (`SUYU_RECOMP_FORCE_MISS_AFTER`) com o FM2 ligado, para
  confirmar que os dois handlers coexistem e que não aparece
  `Unhandled SIGSEGV` no stderr.
- Checklist no jogo, feita por ti:
  - a zona de combate de sempre;
  - uma cutscene;
  - menus e mapa;
  - uma zona com muitas texturas e partículas, onde a GPU mexe mais na memória;
  - um save e um load.

### 6.3 Benchmarks

- A baseline da secção 1.5: o mesmo commit, a mesma zona e o mesmo modo de
  energia (performance mode).
- **A/B:**
  - **A:** `a46cfe0a9` (a baseline congelada);
  - **B:** FM2 on;
  - **B′:** FM2 on com `ACRE_NO_FASTMEM=1`, que isola o custo do `cmp` e do
    código maior.
- **Métricas:**
  - o teu FPS em combate, com o mínimo, o típico e o máximo;
  - `perf stat` de 10s em combate: `cycles`, `instructions`, IPC e
    `L1-icache-load-misses`, este por causa do código maior;
  - os faults/s, o total de despromoções e, num build de medição, a fração dos
    acessos que passam por funções despromovidas;
  - o tamanho do binário e o tempo de arranque (init da tabela).
- **Referência JIT:** o mesmo save e a mesma zona no `suyu-v0.0.4` (dynarmic
  com o fastmem dele), para ver a que distância ficamos do emulador.
  - Só se tiveres disponibilidade para correr essa build.
  - Nenhum commit no `suyu-v0.0.4`.

## 7. Riscos de regressão

| Risco | Probabilidade | Mitigação |
|---|---|---|
| Faults frequentes em páginas da GPU | **medido: alta** sem despromoção | despromoção por função (F0: ~500 sítios, ~5 ms no total); telemetria; kill switch |
| Uma função quente que só às vezes toca na GPU fica despromovida (perde o ganho) | média | sem regressão face a hoje; F4 mede a fração; limiar configurável |
| Código maior provoca misses de i-cache (o que matou os dois inlines anteriores) | média | inline mínimo (~20 B por sítio, contra ~11 B de um call hoje); slow path em `.text.unlikely`; `L1-icache-load-misses` no A/B |
| Palavras de limite: mais linhas de cache de dados no caminho rápido | baixa | uma palavra de 8 B por bloco; só os blocos quentes ocupam cache; PoC: +0.04 ns/acesso |
| Handler do dynarmic por cima do nosso | **certa** se nada for feito | registo forçado do dynarmic antes do nosso; teste de integração |
| Bug no handler causa crashes difíceis de depurar | baixa | handler mínimo, sem chamadas; testes com mutantes; o kill switch repõe o comportamento atual |
| Uma thread com SIGSEGV bloqueado morre no primeiro fault | baixa | verificação da máscara por thread; FM2 desligado se falhar |
| A arena `PROT_NONE` revela acessos do guest que hoje "funcionam" em zeros | baixa | F1 isolada; o slow path regista-os |
| O compilador reordena acessos (aliasing) | baixa | `may_alias`, operandos `"=m"`, o teste diferencial |
| `asm goto` com outputs falha em alguma versão do clang | baixa | plano B com flag de falha (+3 instruções) |

## 8. Estimativa de ganho

| Caminho | Instruções por acesso, aprox. | PoC |
|---|---|---|
| Helper atual (`call` + corpo + `ret` + passagem de argumentos) | ~23 | 0.81 ns |
| FM2 (`cmp` com memória, `jae`, `mov`) | 3 | 0.24 ns |

- **Poupança:** cerca de 70–80% do custo dos helpers, o que dá ~20–25% dos
  ciclos dos threads de CPU, se os ciclos acompanharem as instruções.
- **Teto:** o thread principal (`CPUCore_1`) está a ~96%, por isso o teto é cerca
  de **+30% de FPS** nas zonas limitadas pelo CPU.
- **Correções realistas:**
  - **i-cache:** os dois inlines anteriores perderam tudo por causa disto. Aqui o
    código cresce cerca de 9 B por sítio no hot path, mais o slow call frio, em
    vez de ~60–120 B. Estimativa do binário: +40–60 MB de código, +17 MB de
    tabela e +4 MB de palavras de limite.
  - **faults:** com a despromoção, ~5 ms uma vez por sessão (F0).
  - **funções despromovidas:** ficam ao custo de hoje. Pela F0, são poucas
    (~500 de 507 090), mas uma delas é um `memcmp` do SDK.
- **Estimativa:**

  | Cenário | FPS (baseline: maioria 40, máximo 45) |
  |---|---|
  | Esperado | **+10% a +20%** em combate (~44–48 típico) |
  | Pessimista | ~0%, se a i-cache dominar |
  | Otimista | ~+25% |

  Os 60fps estáveis não estão garantidos só com isto. O resto do tempo está nos
  blocos do jogo (32%) e no "outro" (21%: GPU, exclusivos, despacho).

## 9. Fora de âmbito

- Remover o FM-lite ou os helpers.
- Mudar a cache de 8192 entradas, o encadeamento ou o lock dos exclusivos.
- Desligar `use_reactive_flushing` sem decisão explícita. A F0 mostra que, com o
  reactive flushing desligado, as páginas da GPU ficavam `PROT_READ`, e os 580k
  loads/s deixavam de fazer fault. Mas isso muda a semântica e não é necessário
  com a despromoção.
- Proteger na arena só as regiões que a GPU modificou (em vez de todas as que
  segue). Resolvia o caso do `memcmp` na origem, mas mexe no rasterizer.
  Fica como ideia para depois da F4.
- Re-promoção periódica de funções despromovidas.
- Portar para Windows ou macOS. O desenho é específico de Linux x86-64; nas
  outras plataformas o FM2 fica desligado.

## 10. Revisão: os seis pontos

### 10.1 Sincronização entre o CPU e as mudanças de proteção da GPU

- **Quem muda as proteções.** `RasterizerMarkRegionCached` e `ProtectRegion`
  chamam `HostMemory::Protect` → `mprotect`. Podem correr na thread da GPU ou
  numa thread de CPU, em simultâneo com acessos do FM2 noutros cores.
- **Garantia do kernel.** Quando o `mprotect` retorna, o kernel já fez o
  shootdown dos TLBs em todos os CPUs. Nenhum acesso que comece depois disso usa
  a permissão antiga.
- **Acesso concorrente com a marcação** (a página passa a ser seguida pela GPU):
  - um acesso que termina antes do `mprotect` vê a página ainda normal. É
    equivalente a ter acontecido antes da marcação, tal como hoje. O FM-lite e o
    fastmem do dynarmic têm a mesma janela, porque leem a tabela de páginas ou a
    arena sem lock;
  - um acesso depois do `mprotect` faz fault → slow path → `Core::Memory` →
    `OnCPURead`/`OnCPUWrite`. É o comportamento de hoje.
- **Acesso concorrente com o fim da marcação** (a página volta a RW): no pior
  caso há um fault a mais, e o slow path faz o acesso certo, porque lê pelo
  backing (1.1).
- **Não há nenhum estado do FM2 que a GPU tenha de atualizar.** A arena é a única
  fonte de verdade. Não existe a leitura não atómica da tabela que o FM1 tem.
- **Despromoção por corrida:** corridas repetidas no mesmo sítio podem
  despromovê-lo. Isso só tira performance, nunca correção.
- **Ordem das stores:** x86 é TSO. Uma store do FM2 e a mesma store pelo helper
  ficam na mesma posição da ordem do programa. O slow path corre logo a seguir
  ao fault, sem outros acessos ao guest pelo meio.

### 10.2 Impacto global da reserva inicial `PROT_NONE`

- **Hoje** a reserva de 512 GiB é `PROT_READ|PROT_WRITE` com `MAP_NORESERVE`.
  Uma leitura num VA que nunca foi mapeado devolve zeros. Uma escrita cria uma
  página anónima privada que o emulador nunca vê.
- **Com `PROT_NONE`:**
  - **FM2:** o acesso faz fault → slow path → `Core::Memory` → "Unmapped
    Read/Write" no log e 0 na leitura. É o que os helpers fazem hoje.
  - **dynarmic (fallback JIT):** o acesso faz fault no código JIT. O handler do
    dynarmic trata-o como trata hoje uma página `PROT_NONE` depois de `Unmap`:
    `FakeCall` para o caminho lento dele, que também vai a `Core::Memory`. O
    resultado muda num só caso: uma escrita num VA nunca mapeado deixa de criar
    uma página "fantasma" que outros acessos do fastmem liam depois. Hoje, esse
    caso já dá resultados diferentes conforme o caminho (fastmem contra
    callback), por isso a mudança torna-os coerentes.
  - **`Map`** usa `mmap(MAP_FIXED)` e substitui a reserva com as permissões
    certas. Não depende da permissão da reserva.
  - **`Protect` num intervalo que inclui reserva nunca mapeada** (por exemplo,
    `MarkRegionDebug(false)` ou `RasterizerMarkRegionCached(false)`) volta a pôr
    essas páginas RW. É o comportamento de hoje, não piora.
  - **Overcommit:** uma reserva `PROT_NONE` não conta para o commit em nenhum
    modo de `vm.overcommit_memory`. A reserva RW com `MAP_NORESERVE` é ignorada no
    modo 2 (estrito), onde 512 GiB falham. A mudança só melhora.
  - **`madvise(MADV_HUGEPAGE)`** sobre a reserva (`host_memory.cpp:565`) é válido
    com `PROT_NONE`. As páginas mapeadas depois herdam o hint.
- **Validação (F1):** o validate, o fallback JIT forçado e a checklist, sem o
  FM2. Se aparecerem logs "Unmapped" novos, são acessos do guest que hoje leem
  zeros em silêncio. Investigam-se antes da F2.

### 10.3 Limites da arena, overflow e acessos desalinhados

- **Limite:** cada palavra de limite vale `tamanho_da_arena − 7` = `(1<<39) − 7`.
  `a < limite` garante `a + 8 ≤ tamanho`, por isso nenhum acesso de qualquer
  largura passa do fim da arena.
  - Os últimos 7 bytes do espaço de endereços vão pelo helper. Nunca são
    mapeados pelo jogo.
  - Testado: página sentinela RW logo a seguir à arena fica intacta.
- **Overflow:** a comparação é sem sinal sobre `a` de 64 bits, **antes** da soma.
  `a` perto de 2^64 falha o `cmp` e vai para o helper; a soma `arena + a` nunca
  acontece. Testado com `UINT64_MAX − 3`. Endereços com tag nos bits altos
  (TBI) também falham o `cmp`, e o helper aplica a máscara como hoje.
- **Espaço de endereços do guest menor que a arena:** os VAs entre o fim do
  espaço do guest e `1<<39` são reserva. Depois da F1 estão `PROT_NONE`: fault
  → slow → unmapped. **Por isso a F1 é pré-requisito da F3.**
- **Desalinhados:** x86 aceita loads e stores desalinhados. O AArch64 também os
  aceita em memória normal, que é a única onde o guest os faz. Os acessos que
  exigem alinhamento (exclusivos, atómicos) não passam pelo FM2.
- **Cruzamento de página:** a arena é contígua por VA. Um acesso que atravessa
  duas páginas acessíveis é feito pelo hardware. Se a segunda página estiver
  protegida, a instrução inteira faz fault, sem efeito parcial (10.4), e o helper
  faz o acesso dividido como hoje.
- **Atomicidade:** um acesso alinhado de até 64 bits é um único `mov`, tão
  atómico como hoje ou mais. Um acesso desalinhado que cruza linhas de cache não
  é atómico, nem hoje nem no AArch64 sem LSE2.

### 10.4 Preservação de registos e semântica das stores depois de um fault

- **Registos:** o kernel guarda o contexto completo no frame do sinal (registos
  gerais, flags e o estado XSAVE: SSE, AVX e AVX-512) e repõe-no no `sigreturn`.
  O handler só altera `REG_RIP`. Todos os outros registos chegam ao label `slow`
  com o valor que tinham na instrução que fez fault.
- **Contrato com o compilador:** no caminho do `asm goto` para `slow`, o
  compilador considera os outputs indefinidos. Não lê `v` nesse caminho. É o mesmo
  contrato do `unsafe_get_user` do kernel. As flags estão declaradas como
  clobber (`"cc"`), por causa do `cmp`.
- **Testado:** 12 valores inteiros e 6 doubles vivos através de um load e de uma
  store que fazem fault, com o resultado igual ao do caminho lento puro. Na F2 o
  teste junta valores vivos em registos AVX (`__m256`).
- **Stores:** no x86 as exceções de página são precisas. Uma instrução que faz
  fault não efetiva nenhum byte, mesmo quando atravessa duas páginas e só a
  segunda está protegida. Testado: a store de 8 bytes em p0→p1 não altera p0.
  O helper faz depois a store completa.
- **Exatamente uma vez:** depois do fault, o RIP salta para `slow`. A instrução
  não é repetida, e o helper faz a store uma vez.
- **128 bits:** dois acessos de 64 bits. Se o segundo fizer fault, o primeiro já
  foi feito pelo caminho rápido. É igual a hoje (duas chamadas ao helper).

### 10.5 Handlers de SIGSEGV/SIGBUS, threads e stacks alternativas

- **Âmbito:** o `sigaction` é do processo. O handler serve todas as threads,
  incluindo as que forem criadas depois.
- **Stack:** com `SA_ONSTACK`, o handler corre na sigaltstack se a thread tiver
  uma. Hoje só a thread que registou o dynarmic tem uma. Nas outras corre na stack
  atual, que nas threads de CPU é a stack da fiber. O handler usa algumas
  dezenas de bytes; o frame do kernel com XSAVE ocupa até ~3–4 KB nesta CPU
  (AVX-512). Testado com e sem sigaltstack. A F2 confirma que
  `DEFAULT_STACK_SIZE` deixa folga.
- **Máscara de sinais:** um SIGSEGV síncrono com o sinal bloqueado não chama o
  handler. O kernel repõe a ação por defeito e o processo morre. Nenhum código
  do `core/` bloqueia SIGSEGV hoje. Mesmo assim, na primeira entrada de cada
  `ArmRecomp` em `RunThread`, o runtime lê a máscara
  (`pthread_sigmask(SIG_BLOCK, NULL, &m)`). Se SIGSEGV ou SIGBUS estiverem
  bloqueados, desliga o FM2 (todas as palavras a 0) e regista no log.
- **Reentrância:** sem `SA_NODEFER`, o SIGSEGV fica bloqueado durante o handler.
  Um fault dentro do handler (um bug) mata o processo com core dump, que é o
  resultado certo. O handler não toma locks, por isso não há deadlocks entre
  threads.
- **Ordem com o dynarmic:** secção 3.4, passo 1. Com o FM2 por cima, um fault no
  código JIT não está na nossa tabela e encadeia no dynarmic, que o trata. Um
  fault nosso nunca chega ao dynarmic. Teste de integração: o fallback JIT forçado
  com o FM2 ligado, sem `Unhandled SIGSEGV` no stderr.
- **SIGBUS:** o dynarmic não o instala em Linux; nós instalamos. Um SIGBUS num
  sítio da tabela (por exemplo, um memfd truncado) segue o mesmo caminho.
- **Outros handlers que se instalem depois de nós** (SDL, drivers): ficam por
  cima e, se encadearem, chegam a nós. Se não encadearem, os faults do FM2 matam
  o processo. A F2 verifica no arranque e, uma vez por segundo com a telemetria,
  que a ação de SIGSEGV continua a ser a nossa. Se não for, desliga o FM2 (todas
  as palavras a 0).

### 10.6 Custo de inicialização e memória da tabela de exceções

Medido na PoC com o número real de sítios (1 450 000) e ~507k palavras de
limite estimadas:

| | Tamanho | Notas |
|---|---|---|
| `acre_fmx` (secção) | 17.4 MB (12 B/sítio) | só leitura, do ficheiro; o kernel pode libertar as páginas depois do init |
| Tabela ordenada (heap) | 17.4 MB | `(insn, label, lim)` em 3×`uint32` |
| Contadores por sítio | 5.8 MB (4 B/sítio) | `calloc`; só as páginas com faults chegam a ocupar memória |
| `acre_fml` (palavras de limite) | ~4 MB (8 B/função) | dados; só ficam em RAM as páginas lidas; as escritas são raras (despromoções) |
| **Init** | **51–54 ms** | uma vez, antes de o guest correr; dominado pelo `qsort` |

- Comparado com a memória do processo (vários GB), o custo é pequeno.
- **Otimização possível** (não necessária para avançar): ordenar a tabela em
  build time, num passo depois do link, ou marcar `acre_fmx` como `"aw"` e
  ordenar no sítio. Poupa os 17.4 MB do heap e a maior parte dos 52 ms.
- O kill switch escreve 0 nas ~507k palavras: ~4 MB de páginas sujas, uma vez.
