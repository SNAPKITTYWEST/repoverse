"""Package the verified native build without machine-specific asset paths."""
from pathlib import Path
import hashlib
import shutil
import zipfile

root = Path(__file__).resolve().parents[1]
build = root / "unify/build-game"
out = root / "out/Snapkitty-Windows"
if out.exists():
    raise SystemExit("Package directory already exists; choose a fresh output before packaging.")
required = [build / "unify_game.exe", build / "SDL2.dll"]
for item in required:
    if not item.is_file():
        raise SystemExit(f"Missing build artifact: {item}")
out.mkdir(parents=True)
for item in required:
    shutil.copy2(item, out / item.name)
shutil.copytree(root / "unify/game/assets", out / "assets", ignore=shutil.ignore_patterns("*.usav"))
shutil.copytree(root / "assets/snapkitty", out / "source-artwork")
shutil.copy2(build / "vendor/sdl2/SDL2-2.32.10/LICENSE.txt", out / "SDL2-LICENSE.txt")
shutil.copytree(root / "unify/third_party/lua/doc", out / "licenses/lua")
shutil.copy2(root / "docs/SNAPKITTY-GAME.md", out / "README.md")
(out / "Play.cmd").write_text('@echo off\ncd /d "%~dp0"\nunify_game.exe --assets "%~dp0assets"\n', encoding="ascii")
files = sorted(p for p in out.rglob("*") if p.is_file())
(out / "SHA256SUMS.txt").write_text("".join(hashlib.sha256(p.read_bytes()).hexdigest() + "  " + p.relative_to(out).as_posix() + "\n" for p in files), encoding="utf-8")
archive = root / "out/Snapkitty-Windows.zip"
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
    for p in sorted(out.rglob("*")):
        if p.is_file():
            z.write(p, p.relative_to(out.parent))
print(archive)
print(hashlib.sha256(archive.read_bytes()).hexdigest())
