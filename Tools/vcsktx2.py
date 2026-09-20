#!/usr/bin/env python3
"""Convert the HD texture pack to a format phone GPUs can actually read.

    python3 Tools/vcsktx2.py --limit 20     # a sample, to eyeball first
    python3 Tools/vcsktx2.py                # the whole pack

WHY THIS EXISTS
The pack ships as BC7 inside .dds, which is what desktop GPUs want and what no Mali, PowerVR or
Adreno phone can sample.  PPSSPP does not decompress it either - ReplacedTexture.cpp checks
whether the GPU supports the format and SKIPS the texture when it does not, so on a phone the
whole 748MB pack quietly does nothing and the PSP's own textures are drawn instead.

KTX2 with a UASTC payload is the format that travels: PPSSPP hands it to the Basis transcoder,
which turns it into ASTC, ETC2 or BC depending on what the GPU in your hand supports.  One pack,
every phone, and the desktop build reads it too.

HOW
Two steps per file, because no single tool does both ends:

    .dds (BC7)  --Pillow-->  RGBA  --basisu-->  .ktx2 (UASTC, zstd)

Pillow decodes BC7; the Basis encoder reads PNG.  The intermediate PNG is written to a scratch
directory and deleted, so the peak cost is one PNG per worker rather than a second copy of the
pack.

The encoder is built on demand from BinomialLLC/basis_universal into .deps - the same place
b-macos.sh puts SDL - because there is no prebuilt basisu for macOS worth depending on.  Build it
with STATIC=OFF: the project's own default asks the linker for crt0.o, which does not exist on a
Mac and fails every executable in the build.

WHAT IS NOT CONVERTED
The handful of .png files in the pack, including the empty shadow-sprite stand-ins.  PPSSPP reads
PNG replacements on every GPU already, so converting them would buy nothing and lose their alpha
being exactly what it is.
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACK = ROOT / "memstick" / "PSP" / "TEXTURES" / "ULUS10160"
BASISU = ROOT / ".deps" / "src" / "basisu" / "bin" / "basisu"


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def convert_one(args):
    src, dst, tmpdir, quality = args
    from PIL import Image

    try:
        with Image.open(src) as im:
            im = im.convert("RGBA")
            png = Path(tmpdir) / (dst.stem + ".png")
            im.save(png)
    except Exception as exc:  # a texture that will not decode is reported, not fatal
        return (src, f"decode failed: {exc}")

    dst.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(BASISU),
        "-uastc",
        "-uastc_level", str(quality),
        "-ktx2",
        "-mipmap",
        "-no_status_output",
        "-output_file", str(dst),
        str(png),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    png.unlink(missing_ok=True)
    if result.returncode != 0 or not dst.is_file():
        return (src, f"encode failed: {result.stdout.strip()[-200:] or result.stderr.strip()[-200:]}")
    return (src, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default=str(PACK), help="the pack to convert")
    ap.add_argument("--out", default=str(ROOT / "dist" / "textures-ktx2" / "ULUS10160"),
                    help="where the converted pack goes")
    ap.add_argument("--limit", type=int, default=0, help="convert only this many, for a look")
    ap.add_argument("--quality", type=int, default=1,
                    help="UASTC level 0-4. 1 is the sweet spot; 4 takes hours over a pack this size")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    if not BASISU.is_file():
        fail(f"{BASISU.relative_to(ROOT)} not found - see the note at the top of this file")
    src_dir = Path(args.src)
    if not (src_dir / "textures.ini").is_file():
        fail(f"{src_dir} does not look like a texture pack - no textures.ini")

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    jobs = []
    for src in sorted(src_dir.rglob("*.dds")):
        rel = src.relative_to(src_dir)
        # `new/` is where SaveNewTextures dumps; nothing in textures.ini points at it.
        if rel.parts and rel.parts[0] == "new":
            continue
        jobs.append((src, (out_dir / rel).with_suffix(".ktx2")))
    if args.limit:
        jobs = jobs[:args.limit]

    # Everything that is not a .dds travels as it is: the PNGs, and the ini itself once its
    # extensions have been rewritten.
    for extra in sorted(src_dir.rglob("*.png")):
        rel = extra.relative_to(src_dir)
        if rel.parts and rel.parts[0] == "new":
            continue
        (out_dir / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(extra, out_dir / rel)

    ini = (src_dir / "textures.ini").read_text(encoding="utf-8", errors="replace")
    (out_dir / "textures.ini").write_text(ini.replace(".dds", ".ktx2"), encoding="utf-8")

    print(f"converting {len(jobs)} textures with {args.jobs} jobs, UASTC level {args.quality}")
    failures = []
    done = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        work = [(src, dst, tmpdir, args.quality) for src, dst in jobs]
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
            for src, err in pool.map(convert_one, work, chunksize=4):
                done += 1
                if err:
                    failures.append((src, err))
                if done % 200 == 0 or done == len(jobs):
                    print(f"  {done}/{len(jobs)}", flush=True)

    total = sum(f.stat().st_size for f in out_dir.rglob("*") if f.is_file())
    before = sum(f.stat().st_size for f in src_dir.rglob("*.dds") if f.is_file())
    print(f"done: {len(jobs) - len(failures)} converted, {len(failures)} failed")
    print(f"{before / (1024 * 1024):.0f} MB of .dds -> {total / (1024 * 1024):.0f} MB in {out_dir}")
    for src, err in failures[:10]:
        print(f"  {src.name}: {err}")


if __name__ == "__main__":
    main()
