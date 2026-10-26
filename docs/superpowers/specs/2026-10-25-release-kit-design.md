# Kit de release para Windows — design

Data: 2026-10-25
Estado: revisão 2, ainda sem código. A revisão 2 responde às perguntas de um builder
independente (marcadas "escolha minha" onde decidi).
Base: `main` em `95e404e62`.
Este documento é a spec própria para "Windows, empacotamento e release" que o design geral
(`2026-10-14-astralchainre-design.md`, linha 253) deixou para mais tarde. A milestone de
afinação fechou: a function-level codegen foi abandonada, ver a memória do projeto.

## 1. Intenção

Hoje só quem tem este repositório e um Linux preparado consegue jogar. Precisa de
`acre.py setup` com o NSP base, o NSP do update e o `prod.keys`, depois de `export`, de
`build` e de `run`, tudo pela linha de comandos. O objetivo é que **qualquer pessoa** com o
seu próprio jogo consiga jogar no PC, como nos outros projetos de recompilação de Switch
(B2recomp, LASRecomp).

A pessoa descarrega um .zip, abre uma app, arrasta a pasta do jogo para a janela, espera
pelo build e carrega em Play.

O trabalho dos projetos parecidos serve só para ideias. Não se copia código.

## 2. Objetivos, não-objetivos e anti-objetivos

**Objetivos**
- **Windows primeiro** (decisão do utilizador). No Linux corre pelo Proton/Wine. Não há
  AppImage nativa nesta spec.
- **Proton: suportado, sem bloquear releases** (decisão do utilizador).
  - Testa-se em Wine a cada mudança, e o README diz que funciona.
  - Um problema que só exista no Wine, como a FM2 mais lenta, não impede uma release.
  - Os valores por defeito, a FM2 incluída, decidem-se pelo Windows real.
- **Entrada: só `exefs/` e `romfs/` extraídos do jogo**, versão **1.0.1** (decisão do
  utilizador).
  - O kit não aceita NSP, XCI nem NCA, e não toca em keys.
  - Quem tem a 1.0.0 atualiza o jogo antes de extrair. O kit não explica como extrair.
- **Interface gráfica** (decisão do utilizador): um `AChainPC.exe` com SDL e Dear ImGui,
  reutilizando a base do overlay da M5.
  - É uma janela única que constrói e depois passa a launcher, como o B2recomp (decisão do
    utilizador).
- **O menor número de passos possível** até jogar (anti-objetivo principal, ver abaixo).
  - O fluxo-alvo é: descompactar, abrir o `AChainPC.exe`, arrastar a pasta, e Play quando o
    build acabar.
  - Nas vezes seguintes é só Play.
- **Report…**: um botão que gera um .zip de diagnóstico com os logs, o minidump do último
  crash, a versão do kit e os dados do PC (CPU, GPU, driver, RAM, versão do Windows).
- **Desenvolvido e empacotado no Linux** por cross-compile com llvm-mingw. O utilizador
  testa o .zip no PC Windows dele, sem ambiente de desenvolvimento lá. No dia a dia os
  testes correm em Wine aqui.

**Não-objetivos**
- AppImage ou Flatpak nativo para Linux. Pode vir depois, com o mesmo código.
- macOS e ARM.
- Suporte à versão 1.0.0, a mods ou a randomizer.
- Atualização automática da app. A pessoa descarrega a nova release.
- Definições do jogo no launcher. Ficam no overlay dentro do jogo (F1, L3+R3), como hoje.
- Assinatura de código. O aviso do SmartScreen é aceite e o README explica-o.

**Anti-objetivos** (falha, mesmo que funcione tecnicamente)
- **Demasiados passos antes de jogar** (resposta do utilizador). Cada pergunta, cada
  ecrã ou cada passo manual a mais tem de ser justificado.
- Qualquer artefacto distribuído que contenha código do jogo, assets, keys, saves ou o
  executável construído (regra do projeto).
- Pedir à pessoa que instale Python, CMake, Visual Studio ou outra dependência.
- Erros crípticos. Uma pasta errada, uma versão errada ou um CPU sem AVX2 dão uma mensagem
  clara na janela.

## 3. Restrições

- **Licenças:**
  - o runtime é um fork do suyu (GPLv3), e cada release aponta para o código-fonte no
    GitHub do AChainPC;
  - o llvm-mingw (LLVM com a Apache 2.0 e exceção LLVM, mais mingw-w64) é redistribuível;
  - não se usam o MSVC nem as bibliotecas do Windows SDK, que não são redistribuíveis.
- **CPU:** o C gerado é compilado com `-march=x86-64-v3` (AVX2), por isso o launcher
  verifica o CPU e recusa com uma mensagem clara.
- **GPU:** Vulkan 1.3, como hoje.
- **Disco:**
  - o C gerado ocupa cerca de 620 MB (main 424 MB, sdk 194 MB, rtld menos de 1 MB);
  - os objetos e o build vão ocupar vários GB durante o build;
  - o launcher verifica o espaço livre antes de começar.
- **Privacidade:** nenhum caminho pessoal em ficheiros commitados nem nos artefactos.

## 4. Como funciona hoje (resumo factual)

- **`acre.py setup`:**
  - instala o update no perfil;
  - corre o frontend com `--dump-recomp-inputs` sobre o NSP base, que extrai os
    segmentos efetivos dos NSO para `work/inputs/{rtld,main,sdk}`;
  - verifica-os contra os SHA-256 de `game/version.json`;
  - pede o `prod.keys`.
- **`acre.py export`:** o lifter C++ `export_module` corre um processo por módulo. Gera
  `work/export/<m>/src/*.c`, com raízes de `game/roots/` e o patch dos 60 fps em
  `game/patches/`. O hash das entradas evita repetir o export.
- **`acre.py build`:**
  - CMake e Ninja compilam **o runtime inteiro mais o C gerado** num único
    `astralchain-static`;
  - o C gerado é compilado com clang (mais rápido do que GCC) e o C++ com `c++`;
  - não há biblioteca de runtime pré-compilada;
  - a ABI é verificada no configure (`RecompAbiVersion`) e por `static_assert`.
- **`acre.py run`:** lança com `-g <NSP base>`. O romfs é lido do NSP pelo loader normal,
  com o update instalado no NAND do perfil.
- **Caminhos:** perfil, saves e ini em XDG (`~/.local/share/astralchain`,
  `~/.config/astralchain`). Não existe caminho Windows.
- **Código só-Linux nas nossas adições:**
  - a fastmem FM2 (`recomp_fastmem.cpp`: SIGSEGV/SIGBUS, `ucontext`, `REG_RIP`);
  - as estatísticas por thread do overlay (`thread_cpu.cpp`: `/proc`);
  - o CMake liga a FM2 só em Linux x86-64.
- **Já preparado para Windows:** o CMake do suyu mantém os ramos MINGW/WIN32, há ramos
  `_WIN32` no lifter, e `dist/` tem o manifest e o `.rc`.

## 5. Desenho

### 5.1 Conteúdo do .zip

```
AChainPC-v1.0.0-windows-x64.zip
  AChainPC.exe            launcher e runtime (o mesmo executável, ver 5.3)
  *.dll                   runtime do mingw/libc++ e SDL, se não forem estáticos
  kit/
    export_module.exe     lifter
    toolchain/            subconjunto do llvm-mingw: clang, lld, headers e libs só x86_64
    game/                 version.json, roots/, patches/ (o mesmo que o repo tem hoje)
    runtime/              headers e .a/.lib que o C gerado precisa para ligar (ver 5.3)
  README.txt              requisitos, SmartScreen, onde ficam os saves, Report…
  LICENSES/
```

Sem código do jogo. O `.zip` é gerado por um script no repo, a partir de uma árvore limpa,
e há uma verificação automática de que nenhum ficheiro vem de `work/`.

### 5.2 Fluxo do launcher

Uma janela, com estados:

1. **Sem jogo.** Mostra "Drag your Astral Chain folder here", com o botão Browse…
   - Se existir uma pasta `game/` ou `AstralChain/` ao lado do `.exe`, com `exefs/` e
     `romfs/` lá dentro, é escolhida sozinha.
   - **Formatos aceites (escolha minha):**
     - a pasta escolhida ou qualquer subpasta até 3 níveis, com um par `exefs/` e
       `romfs/`;
     - isto cobre os formatos comuns das ferramentas de extração: um invólucro
       `<titleid>/`, ou `atmosphere/contents/<titleid>/`;
     - os nomes são comparados sem distinção de maiúsculas;
     - o `main.npdm` é opcional;
     - mais de um par encontrado obriga a pessoa a escolher; zero pares dá um erro claro.
2. **Verificar** (segundos):
   - a pasta tem `exefs/main`, `exefs/sdk`, `exefs/rtld` e `romfs/`;
   - os segmentos dos NSO batem com `game/version.json`;
   - o CPU tem AVX2, há espaço livre e o Vulkan 1.3 está disponível.

   Qualquer falha dá uma mensagem numa frase, com o que fazer.
3. **Build** (começa sozinho, sem botão): barra de progresso por fase (extrair, export,
   compilar N/M ficheiros, ligar), com uma estimativa de tempo e Cancel.
   - Se a app fechar a meio, retoma na vez seguinte: os objetos já compilados ficam.
   - O build corre com prioridade baixa, para o PC continuar usável.
4. **Pronto.** Mostra um botão grande ▶ Play. Mostra também a pasta do jogo (com
   "change"), a pasta dos saves (com "open") e as ações Rebuild, Report… e Help.
5. **Play** lança o jogo e o launcher esconde-se. Quando o jogo fecha, o launcher volta.
   - Um crash do jogo mostra "The game crashed — Report…".

Se a versão do kit mudar, o estado volta a Build sozinho, sem perder saves nem definições.

### 5.3 Arquitetura do build no PC do utilizador

**Escolha minha:** o runtime vem **pré-compilado** e o PC do utilizador só compila o C
gerado.

- **Forma preferida:**
  - o C gerado é ligado numa **DLL do jogo** (`game.dll`), na pasta de dados da app;
  - o `AChainPC.exe`, que é ao mesmo tempo launcher e runtime, carrega-a em Play;
  - a DLL exporta as tabelas de registo (`recomp_registration`) e o número da ABI;
  - o runtime recusa uma DLL com outra ABI e pede um Rebuild;
  - os helpers do runtime que o C gerado chama (load, store, lookup e outros) chegam por
    imports do `.exe`, com uma import lib gerada no build do kit, ou por uma tabela de
    ponteiros passada na inicialização. A escolha fica para o builder (secção 8).
- **Porquê:**
  - o utilizador não compila o suyu inteiro, o que poupa muito tempo e evita CMake no PC
    dele;
  - o launcher e o jogo são o mesmo executável, já testado;
  - um Rebuild só refaz a DLL.
- **Alternativa, se a DLL se revelar inviável** (por exemplo, por custo das chamadas entre
  módulos na fastmem ou nos lookups): o runtime vai como biblioteca estática (`.a`) e liga
  com os objetos do jogo num `game.exe`.
  - O launcher continua a ser o `AChainPC.exe`, e o critério de escolha é o desempenho no
    bench de combate.
- **Gate DLL contra estático (escolha minha):**
  - mede-se em **Linux nativo**, com a FM2 ligada: o jogo como `.so` carregado pelo runtime
    contra o `astralchain-static` de hoje, 6 runs cada, ordem alternada;
  - mede assim o custo das chamadas entre módulos sem ruído do Wine;
  - a DLL passa se o 1% low não piorar mais de 1 fps e os stalls não subirem mais de 20%;
  - depois do port da FM2 (fase 4), uma confirmação no Windows real.
- **O mesmo modelo em Linux (escolha minha):** o build de desenvolvimento em Linux passa ao
  mesmo modelo (runtime mais jogo como `.so`). Há um só caminho de registo e de verificação
  da ABI, e esta passa a ser verificada em runtime, ao carregar o jogo. Assim o teste em
  Linux nativo exercita o código que vai na release.
- **Compilador fixo (escolha minha):** uma só versão do llvm-mingw, a mesma no kit e no
  desenvolvimento em Linux, que já usa o clang do llvm-mingw para o C gerado. A versão fica
  registada no repo, e mudá-la obriga a repetir o bench.

**Pipeline dentro do launcher, em C++ (escolha minha):** os passos de `setup`, `export` e
`build` que hoje estão no `acre.py` passam a uma biblioteca C++ partilhada pelo launcher e
por uma CLI de desenvolvimento. Assim o .zip não leva Python.

- **Extração:** o runtime já sabe carregar um jogo desconstruído (`exefs/main` mais
  `romfs/`). O dump dos segmentos (`--dump-recomp-inputs`) passa a aceitar essa pasta em vez
  do NSP.
- **Export:** o `export_module` é chamado como hoje, um processo por módulo, com raízes e
  patches de `kit/game/`.
- **Compilação:**
  - o launcher chama `clang` do toolchain embutido, em paralelo, um processo por ficheiro
    `.c`, com o número de núcleos menos um;
  - o cache por ficheiro é decidido por hash do conteúdo, para retomar e não repetir;
  - no fim chama `lld`;
  - não há CMake nem Ninja no PC do utilizador.
- **O `acre.py` no repo continua** para o desenvolvimento em Linux. Os passos do kit
  partilham a mesma lógica sempre que der. O builder decide quanto do Python fica a chamar
  a CLI nova.

### 5.4 Execução em Windows

- **Jogo desconstruído:** em Play, o runtime lança com `-g <pasta>/exefs/main`. O romfs é
  lido da pasta `romfs/` do utilizador, sem cópia.
  - Não há instalação de update no NAND, porque o dump já é a 1.0.1 completa.
- **Caminhos:**
  - por defeito, `%APPDATA%\AChainPC\` (ini, perfil, saves, logs) e
    `%LOCALAPPDATA%\AChainPC\` (build, DLL do jogo, cache de shaders);
  - um `portable.txt` ao lado do `.exe` põe tudo ao lado do `.exe`, como no Zelda64Recomp.
- **Keys:** o runtime não precisa de keys com um jogo desconstruído. Se algum serviço do
  suyu as exigir no arranque, isso é um bug a corrigir, não algo a pedir ao utilizador.

### 5.5 Port para Windows (código nosso)

- **FM2 (fastmem):** o handler SIGSEGV/SIGBUS passa a um Vectored Exception Handler. Lê e
  reescreve `CONTEXT.Rip` com a mesma lógica de recuperação, e encadeia com o handler do
  dynarmic, como em Linux.
  - Sem a FM2 o jogo corre, mas mais lento. Ligada por defeito só depois de passar os testes
    da FM2 em Wine e no Windows real.
- **`thread_cpu`:** `GetThreadTimes` e enumeração de threads do processo, em vez de `/proc`.
- **ini e perfil:** os caminhos da secção 5.4 em vez de XDG, em `acre_ini_apply.cpp` e
  `ini_rewrite.cpp`.
- **Frontend:** `/proc/self/exe` já tem alternativas `_WIN32`. Rever `suyu.cpp` para o modo
  launcher.
- **Toolchain do kit:**
  - todo o runtime compila com llvm-mingw (clang e libc++, alvo `x86_64-w64-mingw32`), a
    partir do Linux;
  - as dependências do suyu passam a versões MinGW;
  - as que não compilarem ficam desligadas se não forem precisas para jogar, como
    `gamemode`, Qt e web services.
- **Crash:** um handler de exceções não tratadas grava um minidump
  (`MiniDumpWriteDump`) em `logs/`, para o Report… o incluir.

### 5.6 Testes e release

- **CI local:**
  - script `tools/release/build-kit.sh`, que faz o cross-compile, gera o `.zip` e verifica
    que nenhum artefacto vem de `work/` nem contém caminhos pessoais;
  - testes unitários do pipeline C++ e do lifter, como hoje.
- **Pasta de teste exefs/romfs (escolha minha):** uma ferramenta só de desenvolvimento,
  que nunca vai no kit. É uma opção do frontend que reutiliza o código FS do suyu com o
  `prod.keys` do utilizador e escreve a 1.0.1 completa (base mais update) em
  `work/dump-1.0.1/{exefs,romfs}`. O resultado fica em `work/`, nunca no repo.
- **Wine (aqui, a cada mudança):**
  - abrir o launcher;
  - verificar uma pasta exefs/romfs, criada a partir do nosso dump, que fica em `work/`;
  - build completo, e medir o tempo;
  - correr o jogo headless em modo `--validate`;
  - correr o bench de combate pelo Wine.
- **PC Windows do utilizador (antes de cada release):**
  - descompactar, arrastar a pasta, build, jogar a checklist (`docs/checklist.md`) e
    Report…;
  - **opcional:** com o OpenSSH Server ligado, os passos sem janela (copiar, build, ler
    logs) correm à distância.
- **Medições a recolher no primeiro build real em Windows:**
  - o tempo de build, e o efeito do Windows Defender: se for grave, o launcher mostra um
    conselho com o caminho a excluir;
  - o pico de RAM e o espaço em disco;
  - fps e 1% low comparados com Linux nativo.
- **Release:**
  - um tag `vX.Y.Z` e o `.zip` nas Releases do GitHub do AChainPC;
  - o push e a publicação são sempre do utilizador.

## 6. Fases

0. **Pasta de teste:** a ferramenta de extração só de desenvolvimento (secção 5.6).
1. **Toolchain Windows:** runtime e frontend compilam com llvm-mingw a partir do Linux. O
   `astralchain-static` atual, com o export de hoje, arranca em Wine.
2. **Jogo desconstruído:** extrair o input de exefs/romfs, e lançar com `-g exefs/main` e
   o romfs da pasta. Fica testado em Linux nativo primeiro.
3. **Runtime pré-compilado mais build do jogo à parte:** primeiro em Linux (`.so`), com o
   gate da secção 5.3. Depois o mesmo modelo em MinGW (DLL ou `.a`).
4. **Port das partes nossas:** FM2 com VEH, `thread_cpu`, caminhos, minidump.
5. **Pipeline C++ e launcher ImGui** com os estados da secção 5.2.
6. **Script do kit e teste no Windows real.** Primeira release candidata.

## 7. Riscos

- **O tempo de build num PC normal.** Aqui são cerca de 5 min com 16 núcleos. Com 6 a 8
  núcleos e o Defender ligado pode passar de 20 min.
  - Mitigação: o build retoma, corre em prioridade baixa e mostra uma estimativa honesta.
  - Se for inaceitável, revemos as flags de otimização por ficheiro.
- **A FM2 no Windows e no Wine.** O SEH no Wine é emulado sobre sinais, por isso cada
  falha custa mais. A despromoção da FM2 já limita as falhas a poucas por segundo.
- **Dependências do suyu em MinGW.** Algumas podem não compilar. O plano é desligar as que
  não são precisas para jogar.
- **Diferenças do driver AMD/NVIDIA no Windows face ao RADV.** Só se veem no PC real.
- **Tamanho do .zip.** O llvm-mingw completo tem 608 MB. O subconjunto x86_64
  comprimido deve ficar abaixo de 150 MB. A medir.

## 8. O que fica para o builder

- DLL com imports do `.exe` contra uma tabela de ponteiros (secção 5.3), decidido por
  medição.
- Qual versão do llvm-mingw fixar.
- Estrutura exata da biblioteca C++ do pipeline e a forma de o `acre.py` a reutilizar.
- O formato do cache de objetos e da retoma.
- O aspeto visual do launcher. Arte própria, sem assets do jogo.
- Que dependências do suyu ficam desligadas no build MinGW.
