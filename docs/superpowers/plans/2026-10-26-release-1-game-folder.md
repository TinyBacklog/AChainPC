# Kit de release, plano 1: jogo a partir de uma pasta exefs/romfs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fases 0 e 2 da spec. Uma ferramenta de desenvolvimento gera a 1.0.1 completa em
`work/dump-1.0.1/{exefs,romfs}`. O pipeline (`setup`, `run`, `bench`) passa a aceitar essa
pasta em vez do NSP, sem keys e sem update instalado, em Linux nativo.

**Architecture:**
- **Extração (dev only):** uma opção nova do frontend, `--extract-game=DIR`, abre o NSP
  base de `-g`, aplica o update instalado no perfil com o `PatchManager` do suyu e copia o
  exefs e o romfs efetivos para disco. O `acre.py extract` corre-a num perfil descartável.
- **Carregar a pasta:** o loader de diretório do suyu (`AppLoader_DeconstructedRomDirectory`)
  já carrega `exefs/main` com o `main.npdm`, mas só procura o romfs como ficheiro dentro do
  exefs. Passa a procurar também a pasta irmã `romfs/` e a montá-la com `CreateRomFS`.
- **Pipeline:** `acre.py setup --game-dir DIR` faz o dump dos segmentos a partir de
  `-g DIR/exefs/main`. O registo `work/game.json` guarda `game_dir`, e `run`/`bench` lançam
  com `-g DIR/exefs/main`, sem `--keys-dir`.

**Tech Stack:** C++20 (runtime e frontend, VFS do suyu), Python 3.11 stdlib (`tools/acre`),
ctest.

**Spec:** `docs/superpowers/specs/2026-10-25-release-kit-design.md` (secções 5.4, 5.6 e
fases 0 e 2).

**Planos seguintes** (um por fase, cada um escrito quando o anterior fechar, porque
dependem de medições): 2 toolchain llvm-mingw e arranque em Wine (fase 1); 3 runtime
pré-compilado e jogo à parte, com o gate DLL contra estático (fase 3); 4 port das partes
nossas (fase 4); 5 pipeline C++ e launcher ImGui (fase 5); 6 script do kit e teste no
Windows real (fase 6). A ordem 0, 2, 1 é escolha minha: a fase 2 testa-se em Linux nativo e
dá a pasta que a fase 1 precisa para arrancar em Wine.

## Global Constraints

- Linux x86-64 neste plano. Código novo sem APIs só-Linux (`/proc`, `sigaction`), porque o
  mesmo código vai para MinGW no plano 2. Usar `std::filesystem`.
- Versão suportada: **1.0.1**, title id `01007300020FA000`, conferida por
  `game/version.json` (sem mudanças a esse ficheiro).
- A saída da extração vai só para `work/dump-1.0.1/`. Nada de `work/` entra no repo.
- Testes com valores inventados: sem bytes do jogo, sem offsets reais, sem keys. Testes nunca
  tocam em `~/.local/share/astralchain` nem em `~/.config/astralchain`.
- O caminho NSP de hoje (`setup --base/--update`) continua a funcionar sem mudanças de
  comportamento.
- Commits em inglês, autor kadinho, sem `Co-Authored-By`, sem caminhos pessoais. Sem push.
- Branch `release/windows-kit` (já criada, com a spec em `17f36917c`).

## Review Focus

1. **Pasta sem `main.npdm`, sem `romfs/` ou com `romfs` vazio.** Esperado: o `setup`
   recusa com uma frase que diz o que falta, antes de lançar o frontend. Teste: Task 4
   (`test_game_dir_missing_parts`).
2. **`romfs` como ficheiro dentro do exefs (`romfs.bin`) e como pasta irmã ao mesmo
   tempo.** Esperado: o ficheiro ganha, como hoje; a pasta só é usada se não houver
   ficheiro. Teste: Task 2 (`file_beats_sibling_dir`).
3. **Extração para uma pasta que já existe, ou interrompida a meio.** Esperado: escreve em
   `DIR.tmp` e só renomeia no fim; uma pasta antiga não é misturada com a nova. Teste:
   Task 1 (`refuses_existing_destination`) e Task 3 (`test_extract_replaces_old_dump`).
4. **Perfil de jogo com o update 1.0.1 instalado no NAND, lançado com a pasta.**
   Esperado: o jogo usa o exefs da pasta e os saves do perfil, sem aplicar o update outra
   vez. Teste: Task 5, passo de validação com o perfil de jogo real.
5. **`game.json` antigo (só `base`, com `keys_dir`) depois desta mudança.** Esperado:
   `run` e `bench` continuam a lançar com o NSP e as keys. Teste: Task 4
   (`test_launch_args_legacy_record`).

---

### Task 1: Copiar uma pasta VFS para disco

**Files:**
- Create: `frontend/game_folder.h`, `frontend/game_folder.cpp`
- Modify: `frontend/CMakeLists.txt` (adicionar os dois ficheiros ao executável)
- Test: `tools/tests/test_game_folder.cpp`, `tools/tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  `bool WriteDirectoryToHost(const FileSys::VirtualDir& source, const std::filesystem::path& destination, std::string& error);`
  Escreve em `destination` + `.tmp` e renomeia no fim. Falha (devolve `false` e preenche
  `error`) se `destination` já existir. Apaga um `.tmp` antigo antes de começar.

- [ ] **Step 1: Write the failing test**

`tools/tests/test_game_folder.cpp`, no estilo `CHECK` de `test_thread_cpu.cpp`. Uma árvore
`FileSys::VectorVfsDirectory` com `exefs/main` (bytes `{1,2,3}`), `exefs/main.npdm`
(`{4}`) e `romfs/data/a.bin` (`{5,6}`), escrita para uma pasta temporária
(`std::filesystem::temp_directory_path()` + nome único):
- `copies_tree`: devolve `true`; os três ficheiros existem com os mesmos bytes; não fica
  nenhum `*.tmp`.
- `refuses_existing_destination`: segunda chamada para o mesmo destino devolve `false`,
  `error` não vazio, ficheiros da primeira intactos.
- `clears_stale_tmp`: com um `destination.tmp/lixo` pré-existente, a cópia funciona e
  `lixo` não aparece no destino.

Registar em `tools/tests/CMakeLists.txt` como `frontend_game_folder` (label `frontend`),
ligado a `core` como `test_recomp_snapshot`, com `frontend/game_folder.cpp` nas fontes.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_game_folder`
Expected: FAIL a compilar, `game_folder.h` não existe.

- [ ] **Step 3: Implement `WriteDirectoryToHost` in `frontend/game_folder.cpp`**

Abre `destination.tmp` com `FileSys::RealVfsFilesystem::CreateDirectory(..., OpenMode::ReadWrite)`,
copia com `FileSys::VfsRawCopyD`, e faz `std::filesystem::rename` no fim.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_game_folder && ctest --test-dir build -R frontend_game_folder --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add frontend/game_folder.h frontend/game_folder.cpp frontend/CMakeLists.txt tools/tests/test_game_folder.cpp tools/tests/CMakeLists.txt
git commit -m "feat(frontend): copy a VFS directory to disk through a temporary folder"
```

---

### Task 2: O loader de diretório lê a pasta `romfs/` irmã

**Files:**
- Modify: `runtime/core/loader/deconstructed_rom_directory.h`,
  `runtime/core/loader/deconstructed_rom_directory.cpp:271-274`
- Test: `tools/tests/test_game_folder.cpp` (mesmo executável)

**Interfaces:**
- Produces: `FileSys::VirtualFile Loader::FindDirectoryRomFS(const FileSys::VirtualDir& exefs);`
  Ordem: ficheiro `romfs.bin` no exefs, ficheiro `romfs` no exefs, pasta `romfs/` no pai do
  exefs montada com `FileSys::CreateRomFS`. `nullptr` se nada existir ou se a pasta estiver
  vazia.

- [ ] **Step 1: Write the failing tests**

- `sibling_dir_romfs`: árvore `root/{exefs/main, romfs/data/a.bin}`;
  `FindDirectoryRomFS(root/exefs)` não é `nullptr`, e `FileSys::ExtractRomFS(result)` tem
  `data/a.bin` com os mesmos bytes.
- `file_beats_sibling_dir`: com `exefs/romfs.bin` e `romfs/` presentes, o resultado é o
  próprio `romfs.bin` (mesmo ponteiro).
- `no_romfs`: só `exefs/main` dá `nullptr`; `romfs/` vazia também dá `nullptr`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build --target test_game_folder`
Expected: FAIL a compilar, `FindDirectoryRomFS` não declarada.

- [ ] **Step 3: Implement `FindDirectoryRomFS` and use it in `Load`**

Substitui as linhas 271-274. O pai é o do diretório original (antes do `PatchExeFS`), por
isso guardar `dir->GetParentDirectory()` antes do patch. Um `LOG_INFO` diz qual das três
fontes foi usada.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build --target test_game_folder && ctest --test-dir build -R frontend_game_folder --output-on-failure`
Expected: PASS (testes da Task 1 também).

- [ ] **Step 5: Commit**

```bash
git add runtime/core/loader/deconstructed_rom_directory.h runtime/core/loader/deconstructed_rom_directory.cpp tools/tests/test_game_folder.cpp
git commit -m "feat(loader): mount a romfs folder next to the exefs folder"
```

---

### Task 3: Extração de desenvolvimento (`--extract-game`, `acre.py extract`)

**Files:**
- Modify: `frontend/game_folder.h`, `frontend/game_folder.cpp`, `frontend/suyu.cpp`
  (opção, ajuda, condição do auto-detect na linha 645, bloco ao lado de `--install` na
  linha 860)
- Create: `tools/acre/extract.py`
- Modify: `tools/acre.py`, `tools/acre/config.py`
- Test: `tools/acre/tests/test_extract.py`

**Interfaces:**
- Consumes: `WriteDirectoryToHost` (Task 1).
- Produces:
  - C++: `int ExtractGame(Core::System& system, const std::string& nsp_path, const std::filesystem::path& out);`
    escreve `out/exefs/` e `out/romfs/`; devolve 0 ou 1.
  - Frontend: `--extract-game=DIR` com `-g BASE.nsp`; sai logo a seguir.
  - Python: `config.DUMP_DIR = WORK / "dump-1.0.1"`, `config.EXIT_EXTRACT_FAILED = 27`,
    `extract.run(base: Path, update: Path, keys_dir: Path, work: Path = config.WORK, frontend: Path = config.FRONTEND) -> Path`.
  - CLI: `acre.py extract --base BASE.nsp --update UPDATE.nsp [--keys DIR]`.

- [ ] **Step 1: Write the failing Python tests**

`tools/acre/tests/test_extract.py`, com `_frontend` substituído por um falso que regista os
argumentos e cria `exefs/main`, `exefs/main.npdm` e `romfs/x` no destino:
- `test_extract_uses_throwaway_profile`: o perfil passado ao frontend está dentro de
  `work/`, é recriado vazio, e nunca é `config.play_profile()`.
- `test_extract_installs_update_then_extracts`: duas chamadas, `--install=<update>` e depois
  `--extract-game=<work>/dump-1.0.1.new` com `-g <base>`.
- `test_extract_replaces_old_dump`: um `dump-1.0.1/velho` existente desaparece e o novo
  fica no lugar.
- `test_extract_failure_keeps_old_dump`: frontend com rc 1 sai com 27 e o dump antigo fica.
- `test_missing_keys_exits_10`, `test_missing_update_exits_11`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd tools && python3 -m unittest acre.tests.test_extract -v`
Expected: FAIL, `acre.extract` não existe.

- [ ] **Step 3: Implement `extract.py` and the CLI entry**

Reutiliza `setup.check_keys`, `setup.check_file`, `setup.fresh_profile` e
`setup._frontend`. Perfil descartável `work/extract-profile`. O frontend escreve em
`dump-1.0.1.new`; o Python troca pela pasta final só no fim.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd tools && python3 -m unittest acre.tests.test_extract -v`
Expected: PASS.

- [ ] **Step 5: Implement `ExtractGame` and `--extract-game` in the frontend**

`FileSys::NSP` sobre o ficheiro de `-g`; o NCA `Program` do `GetProgramTitleID()`;
`FileSys::PatchManager` com `system.GetFileSystemController()` e
`system.GetContentProvider()` depois de `SetContentProvider`, `SetFilesystem` e
`CreateFactories` (a mesma ordem do bloco de `--install`); `PatchExeFS(nca->GetExeFS())`;
`ExtractRomFS(PatchRomFS(nca, nca->GetRomFS(), ContentRecordType::Program))`. Junta as duas
pastas numa `VectorVfsDirectory` com subpastas `exefs` e `romfs` e escreve com
`WriteDirectoryToHost`. Sem NSP, sem NCA `Program` ou sem keys: `LOG_CRITICAL` com a causa e
rc 1. O auto-detect da linha 645 não corre com `--extract-game`.

- [ ] **Step 6: Run the real extraction**

Run: `cmake --build build --target astralchain && python3 tools/acre.py extract --base <NSP base> --update <NSP update>`
Expected: rc 0; `work/dump-1.0.1/exefs/{main,main.npdm,rtld,sdk}` e `work/dump-1.0.1/romfs/`
existem. A verificação de versão é feita na Task 4.

- [ ] **Step 7: Commit**

```bash
git add frontend/game_folder.h frontend/game_folder.cpp frontend/suyu.cpp tools/acre.py tools/acre/config.py tools/acre/extract.py tools/acre/tests/test_extract.py
git commit -m "feat(acre): extract the effective 1.0.1 exefs and romfs for testing"
```

---

### Task 4: `setup --game-dir`, e `run`/`bench` com a pasta

**Files:**
- Modify: `tools/acre/setup.py`, `tools/acre/config.py`, `tools/acre/run.py:84-95`,
  `tools/acre/bench.py:160-161`, `tools/acre.py`
- Test: `tools/acre/tests/test_setup.py`, `tools/acre/tests/test_run.py`

**Interfaces:**
- Produces:
  - `setup.check_game_dir(game_dir: Path) -> None` (sai com `EXIT_NO_GAME_FILE` = 11 e
    nomeia a primeira peça em falta: `exefs/main`, `exefs/main.npdm`, `exefs/rtld`,
    `exefs/sdk` ou `romfs/` não vazia).
  - `setup.run_game_dir(game_dir: Path, work: Path = config.WORK, frontend: Path = config.FRONTEND) -> None`.
    Grava `{"game_dir", "keys_dir": None, "profile", "inputs"}`.
  - `config.launch_args(record: dict) -> tuple[list[str], list[str]]`: (opções antes dos
    extras, `["-g", alvo]`). Com `game_dir`, sem `--keys-dir` e alvo
    `<game_dir>/exefs/main`. Sem `game_dir`, como hoje (`--keys-dir` e `base`).
  - CLI: `acre.py setup --game-dir DIR`, mutuamente exclusivo com `--base`.

- [ ] **Step 1: Write the failing tests**

- `test_game_dir_missing_parts`: para cada peça em falta, rc 11 e a mensagem contém o
  nome da peça.
- `test_game_dir_dump_has_no_keys_and_no_install`: com o `_frontend` falso, uma só chamada,
  com `--dump-recomp-inputs` e `-g <dir>/exefs/main`, sem `--keys-dir` nem `--install`.
- `test_game_dir_record`: `game.json` tem `game_dir` absoluto e `keys_dir` nulo.
- `test_launch_args_game_dir` e `test_launch_args_legacy_record` (registo antigo só com
  `base` e `keys_dir`).

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd tools && python3 -m unittest acre.tests.test_setup acre.tests.test_run -v`
Expected: FAIL nos testes novos.

- [ ] **Step 3: Implement**

`run_game_dir` partilha com `run` o dump, a verificação por `identity.verify` e a troca de
`inputs.tmp` por `inputs` (extrair uma função comum). `run.py` e `bench.py` passam a usar
`config.launch_args`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd tools && python3 -m unittest discover -s acre/tests -t . -v`
Expected: PASS, todos.

- [ ] **Step 5: Real setup from the extracted folder**

Run: `python3 tools/acre.py setup --game-dir work/dump-1.0.1`
Expected: `inputs verified: 1.0.1 (rtld, main, sdk)`. Uma mensagem "game version 1.0.0
detected" quer dizer que a Task 3 não aplicou o update: voltar à Task 3.

- [ ] **Step 6: Commit**

```bash
git add tools/acre.py tools/acre/setup.py tools/acre/config.py tools/acre/run.py tools/acre/bench.py tools/acre/tests/test_setup.py tools/acre/tests/test_run.py
git commit -m "feat(acre): set up and run the game from an exefs/romfs folder"
```

---

### Task 5: Validação no jogo e documentação

**Files:**
- Modify: `README.md` (secção de setup), `tools/README.md`,
  `docs/superpowers/specs/2026-10-25-release-kit-design.md` (correção do `main.npdm`)

- [ ] **Step 1: Export unchanged**

Run: `python3 tools/acre.py export`
Expected: nada a refazer (os inputs têm os mesmos hashes que antes).

- [ ] **Step 2: Headless validation from the folder**

Run: `python3 tools/acre.py run --validate --seconds 180`
Expected: `PASS: ran until the timeout without fallbacks`. O log mostra o romfs vindo da
pasta irmã (o `LOG_INFO` da Task 2) e nenhuma leitura de keys.

- [ ] **Step 3: Bench de combate, pasta contra NSP**

Correr o bench de combate com o registo da pasta e com o registo NSP (guardar uma cópia do
`game.json` de cada), 3 runs cada. Expected: média e 1% low dentro do ruído (±1.5 fps). O
romfs montado da pasta não pode trazer stalls novos.

- [ ] **Step 4: Play check with the user**

Pedir ao utilizador uma sessão curta com o perfil de jogo: carregar o save existente, uma
luta, um loading de área. Expected: saves carregam, sem crash nem assets em falta.

- [ ] **Step 5: Docs**

- README e `tools/README.md`: `acre.py setup --game-dir DIR` como caminho principal (pasta
  exefs/romfs da 1.0.1), o NSP como alternativa; `acre.py extract` só para quem tem NSP e
  keys.
- Spec, secção 5.2: o `main.npdm` é **obrigatório**, não opcional. O loader precisa dele
  para o title id e o layout do processo, e toda a extração de exefs o inclui.

- [ ] **Step 6: Commit**

```bash
git add README.md tools/README.md docs/superpowers/specs/2026-10-25-release-kit-design.md
git commit -m "docs: game folder setup; main.npdm is required"
```
