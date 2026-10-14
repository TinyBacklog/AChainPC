"""acre.py export: dumped modules + roots -> C projects in work/export (incremental)."""
import hashlib
import json
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from . import config, roots

ORDER = ["rtld", "main", *(f"subsdk{i}" for i in range(10)), "sdk"]
TITLE = "ASTRAL CHAIN"
GAME_ROOTS = config.REPO / "game" / "roots"
GENERATOR = config.REPO / "build" / "bin" / "export_module"
LIFTER_FILES = [
    config.REPO / "runtime/core/recompiler/arm64_to_c.h",
    config.REPO / "runtime/core/arm/recomp/recomp_identity.h",
    config.REPO / "recompiler/export_module.cpp",
    config.REPO / "tools/acre/roots.py",
    config.REPO / "tools/acre/export.py",
]


def require_inputs(inputs: Path) -> list[str]:
    names = [n for n in ORDER if (inputs / n / "module.json").is_file()]
    if not names:
        config.fail(config.EXIT_NO_SETUP, f"no dumped modules in {inputs}; run `acre.py setup` first")
    return names


def input_hash(inputs: Path, roots_dir: Path, lifter_files: list[Path]) -> str:
    """Everything the generated C depends on; equal hash means the export is current."""
    h = hashlib.sha256()
    def feed(label: str, path: Path):
        h.update(label.encode() + b"\0")
        h.update(hashlib.sha256(path.read_bytes()).digest())
    for path in sorted(inputs.glob("*/data/*.bin")):
        feed(str(path.relative_to(inputs)), path)
    for path in sorted(inputs.glob("*/module.json")):
        feed(str(path.relative_to(inputs)), path)
    for path in sorted(roots_dir.glob("*.roots")):
        feed("roots/" + path.name, path)
    for path in lifter_files:
        feed("lifter/" + path.name, path)
    return h.hexdigest()


def extra_roots(roots_dir: Path, module: str) -> set[int]:
    path = roots_dir / f"{module}.roots"
    if not path.is_file():
        return set()
    values = set()
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            values.add(int(line, 16))
    return values


def registration(modules: list[str]) -> str:
    out = ['#include <stdint.h>', 'typedef void (*BlockFn)(void*);',
           'typedef struct { const char* name; BlockFn (*lookup)(uint64_t); void (*set_base)(uint64_t); } Module;']
    for name in modules:
        out += [f'extern BlockFn recomp_image_lookup_{name}(uint64_t);',
                f'extern void recomp_image_set_base_{name}(uint64_t);']
    out += ['static const Module modules[] = {']
    out += [f'{{"{name}", recomp_image_lookup_{name}, recomp_image_set_base_{name}}},' for name in modules]
    out += ['};', 'const Module* suyu_recomp_static_modules(unsigned* count);',
            'const Module* suyu_recomp_static_modules(unsigned* count) {',
            '*count = sizeof(modules) / sizeof(modules[0]); return modules; }']
    return '\n'.join(out) + '\n'


def _build_generator() -> None:
    result = subprocess.run(["cmake", "--build", str(config.REPO / "build"), "--target", "export_module"],
                            capture_output=True, text=True)
    if result.returncode != 0 or not GENERATOR.is_file():
        config.fail(config.EXIT_EXPORT_FAILED,
                    "cannot build export_module (configure `build/` first):\n" + result.stdout[-2000:])


def _export_module(name: str, inputs: Path, out: Path, logs: Path) -> dict:
    module = json.loads((inputs / name / "module.json").read_text())
    found, entry = roots.discover(inputs / name, module)
    found |= extra_roots(GAME_ROOTS, name)
    roots_file = out / f"{name}.roots"
    roots_file.write_text("".join(f"{value:x}\n" for value in sorted(found)))
    log = logs / f"export-{name}.log"
    with log.open("w") as stream:
        rc = subprocess.run([str(GENERATOR), name, str(inputs / name / "data"), str(out / name),
                             hex(module["text_vaddr"]), hex(entry), str(roots_file), TITLE],
                            stdout=stream, stderr=subprocess.STDOUT).returncode
    if rc != 0:
        config.fail(config.EXIT_EXPORT_FAILED, f"export_module failed for {name}; see {log}")
    ident = json.loads((out / name / "recomp_identity.json").read_text())
    module["segment_sha256"] = ident["sha256"]
    module["abi_version"] = ident["abi_version"]
    module["roots"] = len(found)
    return module


def run(work: Path = config.WORK) -> None:
    record = config.load(work)
    inputs, export = Path(record["inputs"]), work / "export"
    names = require_inputs(inputs)
    digest = input_hash(inputs, GAME_ROOTS, LIFTER_FILES)
    manifest_path = export / "aot_manifest.json"
    if manifest_path.is_file() and json.loads(manifest_path.read_text()).get("input_hash") == digest:
        print("export up to date")
        return

    _build_generator()
    tmp, logs = work / "export.tmp", work / "logs"
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    logs.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=len(names)) as pool:
        modules = list(pool.map(lambda n: _export_module(n, inputs, tmp, logs), names))
    (tmp / "recomp_registration.c").write_text(registration(names))
    manifest = {"version": 3, "input_hash": digest, "runtime_abi_version": modules[0]["abi_version"],
                "modules": modules}
    (tmp / "aot_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if export.exists():
        shutil.rmtree(export)
    tmp.rename(export)
    print("exported: " + ", ".join(f"{m['name']} ({m['roots']} roots)" for m in modules))
