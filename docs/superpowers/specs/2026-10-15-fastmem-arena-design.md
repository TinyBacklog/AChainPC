# Fastmem por arena (FM2) — design

Data: 2026-10-15
Estado: proposta para revisão (sem código)
Branch: `m4-60fps`, base `a46cfe0a9`

## 0. Resumo

Os helpers de memória (`recomp_load*`/`recomp_store*`) gastam **30.7% dos ciclos**
em combate. O jogo está limitado pelo número de instruções, não pela memória:
IPC 2.8 e 10.6M misses de dTLB em 5s.

Cada acesso do guest custa hoje cerca de 23 instruções:
- passagem de argumentos e `call`;
- comparação com `fm_limit`;
- teste de cruzamento de página;
- load da entrada da tabela de páginas, máscara e teste de null;
- o acesso propriamente dito e o `ret`.

O FM2 troca isto por **3 instruções inline**, sobre a arena virtual que o suyu já
mantém (`VirtualBasePointer`):

```
cmp  a, fm_arena_limit(c)   ; fora da arena -> slow
jae  slow
mov  (arena,a), v           ; o acesso; se fizer fault -> slow
```

Se o `mov` fizer fault (página protegida para a GPU, de debug ou não mapeada),
um handler de SIGSEGV encontra o RIP numa tabela de exceções e muda-o para o
label `slow` do próprio sítio do acesso. O slow path é o helper atual, chamado
em contexto normal e não dentro do sinal. O handler não decodifica instruções e
não chama código do emulador.

## 1. Estado atual (factos verificados)

### 1.1 Arena: `VirtualBasePointer`

- **Criação.** `Core::DeviceMemory` cria
  `buffer{GetIntendedMemorySize(), VirtualReserveSize}` (`core/device_memory.cpp:9-17`).
  - Sem NCE (o caso de x86), a reserva é de `1<<39` (512 GiB).
  - `virtual_base` fica alinhado a hugepage. O offset de alinhamento já está
    incluído em `VirtualBasePointer()` (`common/host_memory.cpp:700-707`).
- **Backing.** O backing é `memfd_create` + `MAP_SHARED`
  (`host_memory.cpp:509-551`). A arena e as entradas da tabela de páginas apontam
  para as mesmas páginas físicas, por isso estão sempre coerentes.
- **Mapeamento.** `guest_va == offset na arena`: `Map(GetInteger(base), …)`
  (`core/memory.cpp:78`). Logo `VirtualBasePointer() + va` aponta para a página do
  guest, sem base extra.
- **Quando a arena é usada.** Só quando `fastmem_arena` está definido:
  `IsApplication() && Settings::IsFastmemEnabled()` (`memory.cpp:53-54`), que é
  verdade em x86 Linux com `cpu_accuracy = Auto` (`common/settings.cpp:187-201`).
  Ou seja, **a arena já é mantida hoje** em todas as nossas execuções; o fallback
  JIT (dynarmic) usa-a como `fastmem_pointer` (`arm_dynarmic_64.cpp:227-234`).
- **Reserva inicial.** **A reserva inicial é RW, não `PROT_NONE`:**
  `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`
  (`host_memory.cpp:444-451`).
  - Só os intervalos que passaram por `Unmap` ficam `PROT_NONE`.
  - Um VA do guest que nunca foi mapeado lê zeros e aceita escritas em
    silêncio. Com fastmem isto é um bug de correção; ver a secção 4.4.

### 1.2 Proteções na arena

| Tipo de página (tabela de páginas) | Permissão na arena | Origem |
|---|---|---|
| Memory | permissões do kernel (RW ou R; sem X em x86) | `MapMemoryRegion` `memory.cpp:77-79`, `ProtectRegion` `94-123` |
| RasterizerCachedMemory, reactive flushing **ligado** (por defeito) | `PROT_NONE` | `RasterizerMarkRegionCached` `memory.cpp:452-461` |
| RasterizerCachedMemory, reactive flushing desligado | `PROT_READ` | idem |
| DebugMemory (watchpoints) | `PROT_NONE` | `MarkRegionDebug` `397-401` |
| Unmapped (depois de `Unmap`) | `PROT_NONE` | `Unmap` `host_memory.cpp:596-609` |
| Nunca mapeada | **RW** (reserva inicial) | 1.1 |

- `use_reactive_flushing` vem **ligado** por defeito (`settings.h:512-519`). O
  `frontend/` não o altera.
- As marcações para a GPU acontecem só nas transições 0↔1 de um refcount de 8
  bits por página (`device_memory_manager.inc:544-620`), não em cada escrita.
- Numa escrita pelo caminho lento, `Memory::Write` → `HandleRasterizerWrite` →
  `GPU::OnCPUWrite` (`memory.cpp:697-734`, `vk_rasterizer.cpp:721-735`).
- Numa leitura pelo caminho lento, `HandleRasterizerDownload` → `OnCPURead`
  (`memory.cpp:679-695`).

### 1.3 Caminho atual (FM-lite)

- `_recomp_fm` (`arm64_to_c.h`, "Fast path") lê a entrada de 32 bytes de
  `fm_table`. Um ponteiro nulo (páginas Unmapped, Debug ou RasterizerCached)
  manda o acesso para `memload`/`memstore` → `h->load`/`h->store` →
  `Core::Memory`, que avisa a GPU.
- Contagens de sítios de acesso no `main`:

  | Helper | Sítios |
  |---|---|
  | `load64` | 367 561 |
  | `store64` | 226 385 |
  | `load32` | 186 412 |
  | `store32` | 112 070 |
  | `load8` | 17 411 |
  | `store8` | 7 328 |
  | `load16` | 6 407 |
  | `store16` | 3 449 |
  | **Total** | **927 023** |

- Os acessos de 128 bits (Q, LDP/STP Q, LD1…) já são compostos por dois
  `load64`/`store64`.
- Os exclusivos (LDXR/STXR) vão por `HostExclusive` e o monitor, não pelos
  helpers.

### 1.4 Sinais

- **Handler do dynarmic** (`exception_handler_posix.cpp:66-186`):
  - é registado de forma lazy, quando o fallback JIT é criado pela primeira vez;
  - usa `SA_SIGINFO|SA_ONSTACK|SA_RESTART`, e tem uma sigaltstack só na thread
    que o registou;
  - verifica se o PC está no seu código; se não estiver, **encadeia no handler
    anterior**.
- O handler do `arm_dynarmic.cpp:14-45` é código morto: ninguém o regista.
- Não há `thread_local` no backend recomp. O desenho abaixo não precisa de
  nenhum.

### 1.5 Medições que motivam isto (`a46cfe0a9`, combate)

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

## 2. Comparação com o FM1 do mk8-recomp

Fonte: `dougchansan/suyu-v0.0.4` na tag `v0.0.11`
(`src/core/recompiler/arm64_to_c.h`, `src/core/arm/recomp/arm_recomp.cpp`,
`docs/releases/v0.0.11.md`), usado só como referência e sem copiar código.

| | FM1 (mk8-recomp) | FM-lite (nosso, `b3114f422`) | FM2 (proposta) |
|---|---|---|---|
| Fonte do endereço | entrada da `Common::PageTable` | igual | arena `VirtualBasePointer` |
| Sítio do acesso | `call` a um helper | `call` a um helper | inline: `cmp`/`jae`/`mov` |
| Verificações por acesso | limite, cruzamento de página, load da entrada, null | igual | só o limite |
| Página GPU/debug/unmapped | entrada nula → slow path | igual | fault → tabela de exceções → slow path |
| Handler de sinais | nenhum | nenhum | sim, só muda o RIP |
| Cruzamento de página | slow path | slow path | hardware; fault só se a 2.ª página estiver protegida |
| Ganho medido | **+4%** (34.6→36.0 fps, notas da v0.0.11) | 0% sozinho (24 fps), mas é a base dos ganhos seguintes | estimativa na secção 8 |
| Inline tentado | sim: imagem 100→222 MB, **−14% FPS** | `RECOMP_FM_INLINE`: +96 MB, 0% | inline de 3 instruções, ~18 B por sítio |

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

Acrescentados ao `GuestContext`, depois de `block_cache` (que termina em
872 + 8192·16 = 131944), e espelhados em `GuestContextView`, com offsets fixados
dos dois lados:

```c
uint8_t* fm_arena;        /* @131944: VirtualBasePointer(), ou NULL */
uint64_t fm_arena_limit;  /* @131952: 1 << as_bits, ou 0 = FM2 desligado */
```

- `RunThread` atualiza os dois em cada entrada, como já faz com
  `fm_table`/`fm_limit`, a partir de `page_table.fastmem_arena`.
- Se a arena for nula (fastmem desligado no suyu) ou o kill switch estiver
  ativo, `fm_arena_limit = 0` e todos os acessos vão para os helpers atuais.

### 3.2 Emissão

- O emissor deixa de chamar diretamente `recomp_load32(c,a)` e passa a emitir
  macros, como `RECOMP_LD32(c,a)` ou `RECOMP_ST64(c,a,v)`.
- O `recomp_runtime.h` resolve as macros:
  - **sem `RECOMP_FASTMEM`** (por defeito): `RECOMP_LD32` → `recomp_load32`.
    O código gerado fica igual ao de hoje, com o mesmo binário e a mesma ABI de
    acessos;
  - **com `-DRECOMP_FASTMEM`** (opção CMake `ACRE_FASTMEM`, desligada por
    defeito): → `recomp_fx_load32`, uma função `static inline` com `asm goto`.
- Os helpers atuais (`recomp_load*` e `recomp_store*`, que usam o FM-lite)
  **ficam como estão** e são o slow path do FM2.

### 3.3 Acesso inline (asm goto + tabela de exceções)

Esboço de um load de 32 bits. É indicativo; os nomes finais saem do plano.

```c
typedef uint32_t __attribute__((may_alias)) acre_u32a;
static inline uint64_t recomp_fx_load32(GuestContext* c, uint64_t a){
  uint32_t v;
  if (RECOMP_LIKELY(a < c->fm_arena_limit)) {
    asm goto("1: movl %1, %0\n"
             ".pushsection acre_fmx,\"a\"\n"
             ".balign 4\n.long 1b - ., %l[slow] - .\n"
             ".popsection"
             : "=r"(v) : "m"(*(const acre_u32a*)(c->fm_arena + a)) : : slow);
    return v;
  }
slow:
  return recomp_load32(c, a);
}
```

- **Tamanhos:**
  - 8 e 16 bits: `movzbl`/`movzwl`;
  - 32 e 64 bits: `movl`/`movq`;
  - stores: `movb`/`movw`/`movl`/`movq`, com o operando `"=m"`.
  - A extensão de sinal continua no C, como hoje.
  - O de 128 bits continua a ser composto por dois acessos de 64 bits. Se o
    segundo fizer fault, o resultado é o mesmo de hoje, porque são dois acessos
    sequenciais.
- **Alinhamento:** x86 aceita acessos desalinhados e o AArch64 também os aceita
  em memória normal. Não há verificação de alinhamento. Os acessos que precisam
  de alinhamento (exclusivos) não passam por aqui.
- **Cruzamento de página:** a arena é contígua por VA, por isso o hardware trata
  um acesso que atravessa duas páginas mapeadas. Se a segunda página estiver
  protegida, o x86 faz fault **antes** de efetivar qualquer byte da store: uma
  instrução não tem efeito parcial em caso de fault. Aí o slow path faz o
  acesso dividido, exatamente como hoje.
- **Atomicidade:** um acesso alinhado de até 64 bits é um único `mov`. É tão
  atómico como hoje, ou mais.
- **Aliasing:** os tipos `may_alias` impedem o compilador de reordenar acessos ao
  guest com base em strict aliasing. As stores levam o operando `"=m"`; não usamos
  um clobber `"memory"` global, que forçaria a recarregar os `c->x[]`. Os
  `asm goto` são implicitamente volatile.
- **Requisitos de compilador:** `asm goto` com outputs exige clang ≥ 11, e o C
  levantado já obriga a usar clang (`9d4f37bfb`). Plano B, se houver problemas:
  uma flag de falha (`xor f,f` + fixup `mov $1,f; jmp 2b` + `test`/`jne`), com
  mais 3 instruções.
- **Tabela de exceções:** fica na secção `acre_fmx`, com entradas relativas de
  2×`int32` (8 B). Não precisa de relocações e funciona com PIE e com binário
  estático.
  - O linker gera `__start_acre_fmx`/`__stop_acre_fmx`.
  - Cada módulo estático contribui com a sua parte; ~1.6M entradas no total
    (main + sdk), ~13 MB.

### 3.4 Handler de SIGSEGV

- Fica num ficheiro próprio, sem dependências do suyu, para os testes do lifter o
  poderem ligar: `core/arm/recomp/recomp_fastmem.cpp`.
- **Inicialização** (`std::call_once`, quando o primeiro `ArmRecomp` liga o FM2):
  1. Converter `[__start_acre_fmx, __stop_acre_fmx)` num vetor ordenado de pares
     `(insn, label)` em offsets de 32 bits a partir de uma base de `.text`. O
     linker não garante a ordem.
  2. Instalar `sigaction(SIGSEGV, SA_SIGINFO|SA_ONSTACK)` e guardar a ação
     anterior.
  3. Também SIGBUS: um acesso não canónico dá `#GP` e chega como SIGSEGV, mas
     cobre-se o caso por segurança.
- **No sinal:**
  1. Ler `rip = uc_mcontext.gregs[REG_RIP]`.
  2. Fazer uma pesquisa binária na tabela.
  3. Se encontrar a entrada, pôr `gregs[REG_RIP] = label`, incrementar um
     contador atómico relaxed de faults e retornar. **Mais nada:** nenhuma
     chamada ao emulador, nenhum lock, nenhuma alocação. Isto é
     async-signal-safe.
  4. Se não encontrar, encadear no handler anterior com a mesma semântica do
     dynarmic: `sa_sigaction`, `SIG_DFL` (repor e retornar para o fault se
     repetir) ou `SIG_IGN`.
- **Coexistência com o dynarmic:**
  - Se o dynarmic se registar depois de nós, guarda o nosso handler como
    anterior e encadeia nele quando o PC não é seu.
  - Se se registar antes, somos nós a encadear nele.
  - As faixas de PC não se sobrepõem: código JIT contra `.text` estático.
- **Estado do CPU no label `slow`:** a instrução que fez fault não alterou nada,
  e no caminho do goto o compilador já assume que o output é indefinido. É o
  mesmo contrato do `unsafe_get_user` do kernel.
- **Fibers:** sem sigaltstack, o handler corre na stack da fiber. Só faz uma
  pesquisa binária, por isso precisa de muito pouca stack.
- **Depuração:** com o FM2 ligado, o gdb vai parar em SIGSEGVs legítimos. O
  README vai dizer `handle SIGSEGV nostop noprint pass`.

### 3.5 Compatibilidade com a GPU e com `mprotect`

- **Escrita numa página em cache da GPU** (`PROT_NONE` ou `PROT_READ`): fault →
  slow → `memstore` → `Memory::Write` → `OnCPUWrite`. É o mesmo caminho de hoje.
  - Quando a GPU deixar de seguir a página, `RasterizerMarkRegionCached(false)`
    repõe RW e as escritas seguintes já não fazem fault.
- **Leitura de uma página em cache, com reactive flushing:** fault → slow →
  `Memory::Read` → `OnCPURead`. É o mesmo comportamento de hoje.
- **Corridas:** a GPU pode mudar a proteção enquanto um core acede à página. O
  pior caso é um fault a mais e o slow path faz o acesso corretamente. Não
  existe a janela de leitura não atómica que o FM1 tem.
- **Custo:** cada fault custa ~2–5 µs (a medir), contra ~50 ns do slow path de
  hoje. Isto só compensa se os acessos a páginas da GPU forem raros. **É o maior
  risco do desenho**, e a Fase 0 mede-o antes de se escrever qualquer código.

## 4. Fallback seguro e flags

1. **Build:** `ACRE_FASTMEM=OFF` (por defeito) gera um código igual ao atual. Os
   helpers, o FM-lite, a cache de 8192 entradas e o encadeamento ficam
   intactos.
2. **Run time:**
   - `ACRE_NO_FASTMEM=1`, ou `fastmem = false` no `astralchain.ini`, põe
     `fm_arena_limit = 0`: todos os acessos vão para os helpers.
   - O `cmp` inline continua a ser executado, mas o comportamento é exatamente o
     dos helpers.
3. **Automático:**
   - Se o suyu tiver o fastmem desligado (`fastmem_arena` nulo), ou se o
     `sigaction` falhar, o FM2 fica desligado e há uma linha no log.
   - Se `VirtualBasePointer` não estiver alinhado a 4 KiB, fica também
     desligado.
4. **Reserva da arena:** passa a `PROT_NONE` (secção 1.1), só no x86 Linux.
   - Hoje um VA que nunca foi mapeado é RW, o que já é um risco para o fastmem
     do fallback JIT.
   - Com `PROT_NONE`, esse acesso faz fault e vai para o slow path, que o
     regista como unmapped, como acontece hoje com os helpers.
   - Esta mudança vai primeiro, sozinha e validada à parte (Fase 1).
5. **Arquivados e não tocados:**
   - o TLB por software (`experiment/software-tlb`, `ca790ba67`);
   - o FM-inline (histórico até `62ba677ad`);
   - a cache de alvos de 8192 entradas.

## 5. Implementação incremental

Cada fase tem os seus testes e o seu commit. O benchmark de cada fase mede só
essa fase, contra `a46cfe0a9` com o FM2 desligado.

- **F0 — medir, sem mudar comportamento.**
  - Build `RECOMP_MEMSTATS` com o contador de slow path partido por tipo de
    página: Unmapped, Debug, RasterizerCached leitura e escrita, cruzamento de
    página, fora do limite. Medição de 10s em combate.
  - Microbenchmark do custo de um SIGSEGV com o handler mínimo nesta máquina.
  - **Gate:** a estimativa de faults/s × custo do fault tem de ficar abaixo de
    1% do tempo do `CPUCore` principal. Se não ficar, paramos e revemos. Uma
    alternativa seria testar `use_reactive_flushing=false`, mas isso muda a
    semântica e só avança com decisão tua.
- **F1 — arena `PROT_NONE`.** Mudança só no `host_memory.cpp` (x86 Linux).
  Teste: o validate de 90s com 0 fallbacks, a checklist e o fallback JIT forçado
  (`SUYU_RECOMP_FORCE_MISS_AFTER`) continuam bons.
- **F2 — runtime.** `recomp_fastmem.cpp` (tabela + handler + encadeamento),
  `fm_arena` e `fm_arena_limit` no contexto (ABI 6), o kill switch e a chave do
  ini. Nesta fase o jogo ainda não usa o FM2.
- **F3 — emissor.** As macros `RECOMP_LD*`/`RECOMP_ST*`, os `recomp_fx_*` em
  `asm goto` e a opção `ACRE_FASTMEM`. Reexport e build com o FM2 ligado.
- **F4 — benchmarks A/B e decisão.**
- **F5 — `ACRE_FASTMEM=ON` por defeito**, só se a F4 o justificar. Os helpers
  ficam como fallback permanente.

## 6. Testes

### 6.1 Unidade e lifter (ctest, label `lifter`)

**`test_fastmem_access`** é um programa em C gerado como os outros testes do
lifter. Usa as macros reais do header.
- **Fixture:** uma arena de 4 páginas:
  - p0 RW;
  - p1 `PROT_READ`;
  - p2 `PROT_NONE`;
  - p3 RW;
  - `fm_arena_limit = 4 páginas`.
- Os helpers lentos são substituídos por versões que registam as chamadas.
- **Casos:**
  - cada tamanho de load e store em p0 vai pelo caminho rápido, com 0 chamadas
    ao slow;
  - uma store em p1 faz fault → slow com o endereço e o valor certos, e p1 fica
    inalterada;
  - um load em p2 → slow;
  - um load e uma store que cruzam de p0 para p2 → slow, sem bytes parciais em
    p0;
  - um acesso que cruza de p0 para p1 (load) → caminho rápido;
  - `a >= limit` → slow, sem tocar na arena: a fixture põe uma página RW logo a
    seguir à arena que tem de ficar inalterada;
  - extensão de sinal (LDRSB/LDRSH/LDRSW) igual à dos helpers;
  - um 128-bit em que a segunda metade faz fault;
  - `fm_arena_limit = 0` → tudo pelo slow path.
- **Encadeamento:** o teste instala primeiro um handler "anterior" que regista a
  chamada e faz `siglongjmp`, e depois provoca um fault fora da tabela. O
  anterior tem de ser chamado.
- **Concorrência:** 4 threads fazem faults em simultâneo durante 1s. Não pode
  haver deadlocks, e as contagens têm de bater certo.
- **Mutantes que o teste tem de matar:**
  - o label do handler desviado em +1;
  - o handler não muda o RIP;
  - o `cmp` do limite removido;
  - a tabela sem ordenação.

**`test_fastmem_diff`** é um diferencial inspirado na ideia do FM1, com
implementação nossa.
- Corre os mesmos programas dos testes `test_chain` e `test_fmlite`, mais um
  fuzz de acessos aleatórios (tamanhos, offsets junto aos limites de página,
  páginas protegidas).
- Compara o FM2 com os helpers: registos, memória e o log de chamadas ao slow
  path têm de coincidir.

**ABI:** os `typedef char recomp_layout_*` e os `static_assert` dos offsets 131944
e 131952.

### 6.2 Integração

- Validate headless de 90s com o FM2 ligado: PASS e 0 fallbacks. O relatório
  inclui os faults por segundo, a partir do contador do handler.
- Fallback JIT forçado (`SUYU_RECOMP_FORCE_MISS_AFTER`) com o FM2 ligado, para
  confirmar que os dois handlers coexistem.
- Checklist no jogo, feita por ti:
  - a zona de combate de sempre;
  - uma cutscene;
  - menus e mapa;
  - uma zona com muitas texturas e partículas, onde a GPU mexe mais na memória;
  - um save e um load.

### 6.3 Benchmarks

- Os mesmos commits, a mesma zona e o mesmo modo de energia (performance mode).
- **A/B:**
  - **A:** `a46cfe0a9` (FM2 off);
  - **B:** FM2 on;
  - **B′:** FM2 on com `ACRE_NO_FASTMEM=1`, que isola o custo do `cmp` e do
    código maior.
- **Métricas:**
  - o teu FPS em combate, com o mínimo, o típico e o máximo;
  - `perf stat` de 10s em combate: `cycles`, `instructions`, IPC e
    `L1-icache-load-misses`, este por causa do código maior;
  - os faults por segundo;
  - o tamanho do binário.
- **Referência JIT:** o mesmo save e a mesma zona no `suyu-v0.0.4` (dynarmic
  com o fastmem dele), para ver a que distância ficamos do emulador.
  - Só se tiveres disponibilidade para correr essa build.
  - Nenhum commit no `suyu-v0.0.4`.

## 7. Riscos de regressão

| Risco | Probabilidade | Mitigação |
|---|---|---|
| Faults frequentes em páginas da GPU (reactive flushing) tornam o FM2 mais lento | média | F0 com gate; métrica de faults/s; kill switch |
| Código maior provoca misses de i-cache (o que matou os dois inlines anteriores) | média | inline mínimo (~18 B por sítio, contra ~11 B de um call hoje); slow path em `.text.unlikely`; `L1-icache-load-misses` no A/B |
| Bug no handler causa crashes difíceis de depurar | baixa | handler mínimo, sem chamadas; testes com mutantes; o kill switch repõe o comportamento atual |
| A arena `PROT_NONE` revela acessos do guest que hoje "funcionam" em zeros | baixa | F1 isolada; o slow path regista-os |
| O compilador reordena acessos (aliasing) | baixa | `may_alias`, operandos `"=m"`, o teste diferencial |
| Conflito com o handler do dynarmic | baixa | encadeamento nos dois sentidos, testado |
| `asm goto` com outputs falha em alguma versão do clang | baixa | plano B com flag de falha (+3 instruções) |

## 8. Estimativa de ganho

| Caminho | Instruções por acesso, aprox. |
|---|---|
| Helper atual (`call` + corpo + `ret` + passagem de argumentos) | ~23 |
| FM2 (`cmp`, `jae`, `mov`; o endereço já está em registo; `fm_arena` e o limite costumam ficar num registo ou em L1) | ~3–4 |

- **Poupança:** cerca de 80% do custo dos helpers, o que dá ~25% dos ciclos dos
  threads de CPU, se os ciclos acompanharem as instruções (IPC alto e uniforme).
- **Teto:** o thread principal (`CPUCore_1`) está a ~96%, por isso o teto é cerca
  de **+30% de FPS** nas zonas limitadas pelo CPU.
- **Correções realistas:**
  - **i-cache:** os dois inlines anteriores perderam tudo por causa disto. Aqui o
    código cresce cerca de 7 B por sítio no hot path, mais o slow call frio, em
    vez de ~60–120 B. Estimativa total do binário: +40–60 MB, dos quais só
    ~10 MB são hot.
  - **faults:** dependem da F0.
- **Estimativa:**

  | Cenário | FPS |
  |---|---|
  | Esperado | **+10% a +20%** em combate (de ~40–44 para ~45–52) |
  | Pessimista | ~0%, se a i-cache dominar |
  | Otimista | ~+25% |

  Os 60fps estáveis não estão garantidos só com isto. O resto do tempo está nos
  blocos do jogo (32%) e no "outro" (21%: GPU, exclusivos, despacho).

## 9. Fora de âmbito

- Remover o FM-lite ou os helpers.
- Mudar a cache de 8192 entradas, o encadeamento ou o lock dos exclusivos.
- Desligar `use_reactive_flushing` sem decisão explícita.
- Portar para Windows ou macOS. O desenho é específico de Linux x86-64; nas
  outras plataformas o FM2 fica desligado.
