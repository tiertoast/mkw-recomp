"""Inspect the Mach-O ABI and run the loader after relocation and against invalid synthetic modules.

Run through CTest after tools/build/build.py --target macos-x86_64.
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def output(*args):
    return subprocess.check_output(args, text=True)


def check(executable, game_id, exports):
    directory = executable.parent
    host = directory / "libmkw-host.1.dylib"
    game = directory / "mkwgame.dylib"
    for binary in (executable, host, game):
        assert output("lipo", "-archs", binary).strip() == "x86_64"
        assert "@loader_path" in output("otool", "-l", binary) or binary == host
    for binary, names in [(host, [*exports, "mkw_bind_host_v1"]), (game, ["mkwgame_tables"])]:
        assert set(output("nm", "-gjU", binary).split()) == {"_" + sym for sym in names}
        assert output("otool", "-D", binary).splitlines()[-1] == "@rpath/" + binary.name
    assert "@rpath/libmkw-host.1.dylib" in output("otool", "-L", game)
    local = output("nm", "-m", executable).splitlines()
    for sym in exports:
        assert any("non-external" in line and line.endswith(" _" + sym) for line in local), sym
    imports = output("nm", "-m", "-u", game)
    for symbol in ("rt_dispatch", "rt_sj_owner"):
        assert f"_{symbol} (from libmkw-host)" in imports
    assert "dynamically looked up" not in imports
    assert "TWOLEVEL" in output("otool", "-hv", game)
    for section in ("__mkw_ovr", "__mkw_obs", "__mkw_fns"):
        assert section in output("otool", "-l", executable)

    headers = Path(__file__).resolve().parents[2] / "runtime"
    with tempfile.TemporaryDirectory(prefix="mkw G2 à l'essai ") as tmp:
        relocated = Path(tmp)
        for binary in (executable, host, game):
            shutil.copy2(binary, relocated)
        runner = relocated / executable.name
        sidecar = relocated / "mkwgame.id"
        for content, expected in [(None, 1), (game_id + "-stale", 1), (game_id, 0)]:
            if content is not None:
                sidecar.write_text(content)
            present = subprocess.run([runner, relocated, "--present"], cwd="/", timeout=5)
            assert present.returncode == expected
        subprocess.run([runner, relocated], cwd="/", check=True, timeout=5)
        src = relocated / "invalid.c"
        for arch, entry, abi, tag, diagnostic in [
            ("x86_64", "mkwgame_tables", "RT_GAME_ABI", "other-target", "another version"),
            ("x86_64", "mkwgame_tables", "RT_GAME_ABI+1", game_id, "another version"),
            ("x86_64", "wrong_entry", "RT_GAME_ABI", game_id, "dlsym(mkwgame_tables)"),
            ("arm64", "mkwgame_tables", "RT_GAME_ABI", game_id, "dlopen("),
        ]:
            src.write_text(
                '#include "gamelib.h"\n'
                f"RT_GAME_EXPORT const RtGame *{entry}(void) {{\n"
                f'    static const RtGame g = {{.abi={abi}, .id="{tag}", .nmodules=1}}; return &g;\n}}\n'
            )
            dest = relocated / game.name
            dest.unlink()
            subprocess.run(
                [
                    "xcrun",
                    "--sdk",
                    "macosx",
                    "clang",
                    "-arch",
                    arch,
                    "-dynamiclib",
                    "-I",
                    headers / "cpu",
                    "-I",
                    headers / "core/boot",
                    src,
                    "-o",
                    dest,
                ],
                check=True,
            )
            assert output("lipo", "-archs", dest).strip() == arch
            result = subprocess.run([runner, relocated], capture_output=True, text=True, timeout=5)
            if arch == "arm64":
                assert "arm64" in result.stderr and "x86_64" in result.stderr, result.stderr
            assert result.returncode == 70 and diagnostic in result.stderr, (result.returncode, result.stderr)
            print(f"rejected {arch} {entry} ABI={abi} ID={tag} ({diagnostic})")


if __name__ == "__main__":
    check(Path(sys.argv[1]), sys.argv[2], sys.argv[3].split(";"))
