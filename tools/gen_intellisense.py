#!/usr/bin/env python3
"""Regenerate .vscode/c_cpp_properties.json from the last build.

The ESP32 core passes most of its include dirs through gcc @response files,
so the only reliable source of truth is the actual compile command. Run
`make build` first, then this.

Usage: python3 tools/gen_intellisense.py
"""
import glob
import json
import os
import shlex
import sys

HOME = os.path.expanduser("~")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CCJSON = os.path.join(ROOT, "build", "compile_commands.json")


def load_sketch_command():
    if not os.path.isfile(CCJSON):
        sys.exit("No build/compile_commands.json - run 'make build' first.")
    cc = json.load(open(CCJSON))
    entry = next((e for e in cc if e["file"].endswith(".ino.cpp")), None)
    if entry is None:
        sys.exit("No .ino.cpp entry in compile_commands.json.")
    return entry.get("arguments") or shlex.split(entry["command"])


def parse(args):
    iprefix = ""
    for i, a in enumerate(args):
        if a == "-iprefix":
            iprefix = args[i + 1]
            break

    includes, defines = [], []

    def consume(toks):
        i = 0
        while i < len(toks):
            t = toks[i]
            if t in ("-iwithprefixbefore", "-iwithprefix", "-isystem", "-I"):
                if i + 1 < len(toks):
                    p = toks[i + 1]
                    if t.startswith("-iwithprefix"):
                        p = os.path.join(iprefix, p.lstrip("/"))
                    includes.append(p)
                    i += 2
                    continue
            elif t.startswith("-iwithprefixbefore"):
                includes.append(os.path.join(iprefix, t[18:].lstrip("/")))
            elif t.startswith("-iwithprefix"):
                includes.append(os.path.join(iprefix, t[12:].lstrip("/")))
            elif t.startswith("-I") and len(t) > 2:
                includes.append(t[2:])
            elif t.startswith("-D") and len(t) > 2:
                defines.append(t[2:])
            elif t == "-D" and i + 1 < len(toks):
                defines.append(toks[i + 1])
                i += 2
                continue
            i += 1

    consume(args)
    for a in args:
        if a.startswith("@") and os.path.isfile(a[1:]):
            consume(shlex.split(open(a[1:]).read()))
    return includes, defines


def core_dir(includes):
    for p in includes:
        if p.endswith("/cores/esp32"):
            return p
    return None


def main():
    args = load_sketch_command()
    includes, defines = parse(args)

    core = core_dir([os.path.normpath(p) for p in includes])
    if core:
        platform = os.path.dirname(os.path.dirname(core))
        includes += sorted(glob.glob(f"{platform}/libraries/*/src"))
    includes += [
        f"{HOME}/Documents/Arduino/libraries",
        f"{HOME}/Library/Arduino15/libraries",
    ]

    norm, seen = [], set()
    for p in includes:
        p = os.path.normpath(p)
        if p.endswith("/build/sketch") or not os.path.isdir(p):
            continue
        tok = "${workspaceFolder}" if p == ROOT else p.replace(HOME, "${env:HOME}")
        if tok not in seen:
            seen.add(tok)
            norm.append(tok)

    compiler = next((a for a in args if a.endswith("xtensa-esp32-elf-g++")), "")
    core_tok = next((p for p in norm if p.endswith("/cores/esp32")), None)

    cfg = {
        "version": 4,
        "configurations": [{
            "name": "ESP32",
            "includePath": norm,
            "defines": sorted(set(defines)),
            "compilerPath": compiler.replace(HOME, "${env:HOME}"),
            "compilerArgs": ["-mlongcalls"],
            "cStandard": "c11",
            "cppStandard": "c++17",
            "intelliSenseMode": "gcc-x64",
            "forcedInclude": ([core_tok + "/Arduino.h"] if core_tok else []),
        }],
    }

    out = os.path.join(ROOT, ".vscode", "c_cpp_properties.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    print(f"wrote {out}")
    print(f"  includePath: {len(norm)}  defines: {len(cfg['configurations'][0]['defines'])}")


if __name__ == "__main__":
    main()
