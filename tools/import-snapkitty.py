"""Rebuild lossless QOI runtime copies from the preserved original artwork (requires Pillow)."""
from pathlib import Path
from PIL import Image
import hashlib
import json
root=Path(__file__).resolve().parents[1]
original=root/"assets/snapkitty"
manifest=json.loads((original/"runtime-manifest.json").read_text())
for entry in manifest:
    src=root/entry["source"]
    if hashlib.sha256(src.read_bytes()).hexdigest()!=entry["sha256"]:
        raise ValueError("Source changed: "+str(src))
    image=Image.open(src).convert("RGBA")
    output=root/entry["runtime"]
    output.parent.mkdir(parents=True,exist_ok=True)
    image.save(output,format="QOI")
    with Image.open(output) as check:
        assert image.tobytes()==check.convert("RGBA").tobytes()
print("Four pixel-identical QOI textures prepared. Original images preserved.")
