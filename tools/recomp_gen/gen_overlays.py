#!/usr/bin/env python3
"""Generate one recompiled module per code overlay, at its slot base (local use only).

    python -I gen_overlays.py <disc.cue> <overlay_map.json> <out-dir> <tools-dir> [--disc d1] [--ids 0,42,...]

<overlay_map.json> is the research export (docs/research/OVERLAY_MAP.md): file id, status, inferred
base, entries and external call targets per overlay. Each file is extracted with psx_analyze
--extract-file and handed to recomp_gen with every known entry as a --cfg-entry, plus the function
starts found by scanning (a word right after `jr ra` + delay slot): callbacks reached only through
pointers would otherwise stay interpreted. A wrong start only adds a function boundary (a tail call)
or code nobody enters. Output: <out-dir>/ovl_<disc>_F<id>.cpp, for -DPRM_RECOMP_GENERATED_DIR; it
must stay outside the repository (generated game code is never committed).
"""
import json
import os
import struct
import subprocess
import sys


def starts_after_returns(data, base):
    """Addresses of nonzero words that follow `jr ra` and its delay slot."""
    out = []
    for o in range(0, len(data) - 12, 4):
        if struct.unpack_from('<I', data, o)[0] == 0x03E00008 and struct.unpack_from('<I', data, o + 8)[0] != 0:
            out.append(base + o + 8)
    return out


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    opts = dict(zip(argv[1::1], argv[2::1]))
    if len(args) < 4:
        print(__doc__)
        return 2
    cue, map_path, out_dir, tools = args[:4]
    disc = opts.get('--disc', 'd1')
    ids = set(int(x) for x in opts['--ids'].split(',')) if '--ids' in opts else None
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    out_dir = os.path.abspath(out_dir)
    if os.path.commonpath([repo, out_dir]) == repo:
        print('out-dir must be outside the repository')
        return 2
    os.makedirs(os.path.join(out_dir, 'bin'), exist_ok=True)
    exe = '.exe' if os.name == 'nt' else ''
    psx = os.path.join(tools, 'psx_analyze' + exe)
    gen = os.path.join(tools, 'recomp_gen' + exe)
    overlays = json.load(open(map_path, encoding='utf-8'))['overlays']
    done = 0
    for ov in overlays:
        fid, base = ov['file_id'], ov.get('inferred_base')
        if not base or (ids is not None and fid not in ids):
            continue
        base = int(base, 16)
        name = 'ovl_%s_F%04d' % (disc, fid)
        binf = os.path.join(out_dir, 'bin', 'F%04d.BIN' % fid)
        if not os.path.exists(binf):
            subprocess.run([psx, '--extract-file', cue, str(fid), binf], check=True, stdout=subprocess.DEVNULL)
        data = open(binf, 'rb').read()
        known = [int(a, 16) for a in list(ov.get('entries', {}).keys()) + ov.get('external_call_targets', [])]
        cand = sorted(set(a for a in known + starts_after_returns(data, base) + [base]
                          if base <= a < base + len(data) - 4 and a % 4 == 0))
        entf = os.path.join(out_dir, 'bin', name + '.cfg')
        with open(entf, 'w') as f:
            f.write(''.join('0x%08x\n' % a for a in cand))
        r = subprocess.run([gen, binf, hex(base), os.path.join(out_dir, name + '.cpp'), '--module', name,
                            '--cfg-entries-file', entf], capture_output=True, text=True)
        print(name, r.stdout.splitlines()[0] if r.stdout else r.stderr.strip())
        done += r.returncode == 0
    print('%d modules' % done)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
