#!/usr/bin/env python3
"""Name the shaders AC2 actually runs, by hashing the game's MaterialTemplate assets.

Why this exists
---------------
The runtime knows a shader only by the FNV-1a 32 of its bytecode (that is what the
dump names files with, and what the report's PER-SHADER COVERAGE table lists). The
asset corpus knows shaders by template name. The two are byte-identical -- AC2's
shaders live in MaterialTemplate assets, not the exe, and pass through
CreateVertexShader unmodified -- so hashing every asset .dxbc maps hash -> name.

This is how `AC2_Water_DO_NOT_USE` was identified as the live water template
despite its name, and how a coverage table row becomes an actionable "this
template never converts".

Two traps, both real
--------------------
1. MANY TEMPLATES COMPILE TO IDENTICAL BYTECODE. One hash therefore maps to a LIST
   of names, not one name. A shader is only unambiguously template X if EVERY name
   it matches is X (or all its matches share the trait you care about). Counting
   each matched name as a separate hit inflates totals wildly -- 185 live shaders
   "matched" 114 Characters-Skin + 90 MapFogOfWar + ...
2. THE LIVE SHADER IS THE AUTHORITY, NOT THE CORPUS. The corpus contains variants
   the game never creates. Deriving behaviour from a variant that does not run
   cost this project a broken UV and a jitter. Use this tool to name what runs;
   read the DUMPED shader, not the asset, when porting one.

Usage
-----
    python tools/name_live_shaders.py <game-dir> [assets-dir]

    <game-dir>    dir containing ac2_rtx_dump/  (dumped vs_*.cso live shaders)
    [assets-dir]  dir of *.dxbc MaterialTemplate assets. Defaults to
                  <game-dir>/Extracted/DataPC.forge/Extracted/AllTemplates

    # name a specific hash from the report's coverage table:
    python tools/name_live_shaders.py <game-dir> | grep -i 0x0bee35a3
"""
import glob
import os
import re
import sys
from collections import defaultdict


def fnv1a32(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def template_of(name: str) -> str:
    """PixelShader_AC2_Water_DO_NOT_USE_0xDEAD -> AC2_Water_DO_NOT_USE"""
    t = re.sub(r'^(Vertex|Pixel)Shader_', '', name)
    return re.sub(r'_0x[0-9A-F]+.*$', '', t)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    game = sys.argv[1]
    assets = (sys.argv[2] if len(sys.argv) > 2
              else os.path.join(game, 'Extracted', 'DataPC.forge', 'Extracted', 'AllTemplates'))

    asset_files = glob.glob(os.path.join(assets, '*.dxbc'))
    if not asset_files:
        print('no .dxbc assets under %s' % assets)
        return 1

    by_hash = defaultdict(list)
    for p in asset_files:
        by_hash[fnv1a32(open(p, 'rb').read())].append(os.path.basename(p)[:-5])

    live = sorted(glob.glob(os.path.join(game, 'ac2_rtx_dump', 'shaders', 'vs_*.cso')))
    if not live:
        print('no live shader dumps under %s' % os.path.join(game, 'ac2_rtx_dump', 'shaders'))
        return 1

    print('%d live shaders, %d asset shaders hashed\n' % (len(live), len(asset_files)))
    print('%-12s %-9s %s' % ('hash', 'match', 'template(s)'))
    print('%-12s %-9s %s' % ('-' * 10, '-' * 9, '-' * 40))

    unmatched = []
    for p in live:
        h = int(os.path.basename(p)[3:-4], 16)
        names = by_hash.get(h)
        if not names:
            unmatched.append(os.path.basename(p)[:-4])
            continue
        tmpls = sorted({template_of(n) for n in names})
        # One template => unambiguous. Many => this bytecode is shared, and the row
        # says nothing about which template a given draw came from.
        tag = 'exact' if len(tmpls) == 1 else 'AMBIG:%d' % len(tmpls)
        shown = tmpls[0] if len(tmpls) == 1 else ', '.join(tmpls[:4]) + (' ...' if len(tmpls) > 4 else '')
        print('0x%08X   %-9s %s' % (h, tag, shown))

    if unmatched:
        print('\n%d live shaders match no asset (created from outside AllTemplates):' % len(unmatched))
        for u in unmatched:
            print('   ' + u)
    return 0


if __name__ == '__main__':
    sys.exit(main())
