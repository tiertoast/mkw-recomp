"""Exercise the player compiler and real loader with a synthetic chunk. Run with the runtime and foundations paths."""

import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tools/prepare"))
from gamecode import compile_game, module_id
from machine import default_cc

runtime, fixture = (Path(p).resolve() for p in sys.argv[1:3])
target = "macos-x86_64"
with tempfile.TemporaryDirectory(prefix="mkw setup ") as tmp:
    game = Path(tmp)
    gen = game / "work/generated"
    chunk = gen / "fixture"
    chunk.mkdir(parents=True)
    exefs = game / "exefs"
    exefs.mkdir()
    # Format/version metadata only. No instructions or assets from the game.
    (exefs / "main").write_bytes(b"NSO0".ljust(64, b"\0") + bytes.fromhex("9e244438c3d8ef1c"))
    (gen / "layout.json").write_text('[{"name":"fixture"}]')
    (gen / "layout.h").write_text("#define MOD_FIXTURE_BASE 0x6000000000ull\n#define MOD_FIXTURE_SIZE 4096\n")
    (chunk / "chunk.c").write_text("""
#include "cpu.h"
static void body(Cpu *c) { c->pc = c->x[1]; rt_dispatch(c); c->x[0] += 3; }
static void enter(Cpu *c) { rt_sj_owner(c, body, 0x6000000000ull, 0x6000000004ull); }
const uint64_t fixture_fn_addrs[] = {0x6000000000ull}, fixture_fn_count = 1;
const GuestFn fixture_fn_ptrs[] = {enter};
""")
    cc = default_cc(game, target)
    compile_game(game, exefs, game / "work", cc, target, 1, runtime=runtime)
    lib = game / "mkwgame.dylib"
    stamp = game / "mkwgame.id"
    expected = module_id(target)
    assert stamp.read_text() == expected == (runtime / "mkwgame.id").read_text()
    exports = subprocess.check_output(["nm", "-gUj", lib], text=True).splitlines()
    assert exports == ["_mkwgame_tables"]
    assert "@rpath/libmkw-host.1.dylib" in subprocess.check_output(["otool", "-L", lib], text=True)
    imports = subprocess.check_output(["nm", "-um", lib], text=True)
    for name in ("rt_dispatch", "rt_sj_owner"):
        assert any("_" + name + " (from libmkw-host)" in line for line in imports.splitlines())
    subprocess.run([fixture, game], check=True, timeout=15)
    env = dict(os.environ, MKW_GAME_DIR=str(game), MKW_PROFILE_DIR=str(game / "profile"))
    def probe():
        return subprocess.run([runtime / "mkw", "--probe-game-module"], cwd=game, env=env, capture_output=True, timeout=20)

    good = probe()
    if good.returncode:
        raise RuntimeError(good.stderr.decode(errors="replace"))
    assert expected.encode() in good.stdout
    if len(sys.argv) == 4:
        output = Path(sys.argv[3])
        output.mkdir(parents=True, exist_ok=True)
        shutil.copy2(lib, output)
        shutil.copy2(stamp, output)
    stamp.write_text("wrong")
    assert subprocess.run([fixture, game, "--present"]).returncode == 1
    stamp.write_text(expected)
    # Re-link the same module with an incompatible embedded ID but a matching sidecar.
    glue = game / "work/obj/modules.c"
    original = glue.read_text()
    includes = [f"-I{p}" for p in {h.parent for h in (root / "runtime").rglob("*.h")}]
    for embedded_id, abi in (("incompatible", "RT_GAME_ABI"), (expected, "0")):
        glue.write_text(original.replace("RT_GAME_ID", f'"{embedded_id}"').replace("RT_GAME_ABI", abi))
        subprocess.run([*cc, "-dynamiclib", "-fvisibility=hidden", *includes, f"-I{gen}", glue,
                        game / "work/obj/fixture_chunk.o", runtime / "libmkw-host.1.dylib",
                        "-Wl,-adhoc_codesign", "-o", lib], check=True)
        bad = probe()
        assert bad.returncode and b"built for another version of the runtime" in bad.stderr
    print("PASS setup module, Mach-O binding, runtime load and incompatible ID rejection")
