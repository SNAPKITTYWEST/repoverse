"""Export a read-only Unify menu from the authoritative C# manifest; no world-schema mutation."""
import argparse
import json
from pathlib import Path
def lua_string(value):
    # Encode bytes, not executable Lua. Fixed-width decimal escapes avoid delimiter injection.
    return '"' + ''.join(chr(92) + format(b, '03d') for b in str(value).encode('utf-8')) + '"'
def export(manifest):
    if manifest.get("schemaVersion") != 1:
        raise ValueError("Expected WorldManifest schemaVersion 1")
    rows=[]
    for b in manifest["buildings"]:
        floors=b["floors"]
        if type(floors) is not int or floors < 1: raise ValueError("Invalid floor count")
        fields={k:b[k]["value"] for k in ("district","category","activity","construction")}
        fields.update(repo=b["repo"],seed=b["seed"])
        text=",".join(k+"="+lua_string(v) for k,v in fields.items())
        rows.append("{"+text+",floors="+str(floors)+",rooms="+str(len(b["interior"]["rooms"]))+"}")
    return "{"+",".join(rows)+"}"
if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("manifest",type=Path)
    parser.add_argument("--out",type=Path,default=Path("unify/game/assets/scenes/repositories.lua"))
    args=parser.parse_args()
    template=Path(__file__).parent.joinpath("templates/repositories.lua").read_text()
    generated=template.replace("@BUILDINGS@",export(json.loads(args.manifest.read_text(encoding="utf-8"))))
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(generated,encoding="utf-8",newline="\n")
    print("Wrote",args.out)
