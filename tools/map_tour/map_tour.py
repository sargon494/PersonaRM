#!/usr/bin/env python3
"""Map tour: warp to every field area from one save state, capture 4:3 and 16:9, flag suspicious frames.

Developer tool (not shipped). See docs/MAP_TOUR.md.

    python tools/map_tour/map_tour.py --exe build/Release/Release/personarm.exe --disc <disc1.cue> \
        --state <field.state> --out <folder outside the repo> --areas 0-120 [--jobs 6]

Needs Pillow. Writes <out>/<area>/{43,169}/frame_*.png, <out>/<area>/*.log and <out>/index.html.
"""
import argparse
import concurrent.futures as cf
import html
import json
import os
import re
import subprocess
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("map_tour needs Pillow (pip install pillow)")


def parse_areas(spec):
    """'0-5,9,0x20' -> [0,1,2,3,4,5,9,32]"""
    out = []
    for part in spec.split(','):
        part = part.strip()
        if not part:
            continue
        if '-' in part[1:]:
            a, b = part.split('-', 1)
            out.extend(range(int(a, 0), int(b, 0) + 1))
        else:
            out.append(int(part, 0))
    return out


def run(exe, args, log, timeout):
    try:
        # cwd: the harness writes personarm_config.json next to it
        p = subprocess.run([exe] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
                           cwd=os.path.dirname(log))
        text = p.stdout.decode('utf-8', 'replace')
        code = p.returncode
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or b'').decode('utf-8', 'replace') + '\n[map_tour] TIMEOUT\n'
        code = 'timeout'
    with open(log, 'w', encoding='utf-8') as f:
        f.write(text)
    return code, text


def luma(img):
    return img.convert('L')


def is_black(img):
    return luma(img).getextrema()[1] < 16


def last_frame(folder):
    names = sorted(n for n in os.listdir(folder) if n.endswith('.png')) if os.path.isdir(folder) else []
    for n in reversed(names):
        img = Image.open(os.path.join(folder, n))
        if not is_black(img):
            return n, img
    return (names[-1], Image.open(os.path.join(folder, names[-1]))) if names else (None, None)


def column_means(img):
    g = luma(img)
    w, h = g.size
    px = g.load()
    return [sum(px[x, y] for y in range(h)) / h for x in range(w)], g


def edge_flags(img169):
    """Widescreen (anamorphic, 320 wide) frame: the old 4:3 picture spans x = w/8 .. 7w/8.
    Flags a sharp vertical luminance step at those columns (the 'square of shadows') and side bands
    much darker than the centre (geometry or 2D not extended)."""
    flags = []
    cols, g = column_means(img169)
    w, h = g.size
    px = g.load()
    def step(x):  # mean |a - b| across the column pair, per row (a hard seam shows on most rows)
        return sum(abs(px[x - 1, y] - px[x, y]) for y in range(h)) / h
    typical = sorted(step(x) for x in range(2, w - 1, 3))
    med = typical[len(typical) // 2] + 1.0
    for x in (w // 8, w - w // 8):
        s = step(x)
        if s > 12 and s > 4 * med:
            flags.append('HIGH edge step x=%d (%.0f vs %.0f)' % (x, s, med))
    side = (sum(cols[:w // 8]) + sum(cols[w - w // 8:])) / (2 * (w // 8))
    centre = sum(cols[w // 8:w - w // 8]) / (w - 2 * (w // 8))
    if centre > 30 and side < 0.25 * centre:
        flags.append('MED dark side bands (%.0f vs centre %.0f)' % (side, centre))
    return flags


def side_and_2d_flags(img43, img169):
    """2D pieces moving apart: the 2D layer (text boxes, HUD) is drawn in 4:3 space and should land in
    the 16:9 frame at the same place squeezed by 3/4 around the centre. Compare the bottom third (where
    the message window sits) of the 16:9 frame with the 4:3 one squeezed: a large mismatch there while
    the frames are otherwise similar is worth a look. Coarse: any 3D motion also raises it."""
    flags = []
    # Holes in the side bands: near-black pixels there, beyond what the 4:3 picture itself has.
    g43, g169 = luma(img43), luma(img169)
    w, h = g169.size
    b = w // 8
    side = [g169.crop((0, 0, b, h)), g169.crop((w - b, 0, w, h))]
    def black(im):
        hist = im.histogram()
        return sum(hist[:12]) / float(im.size[0] * im.size[1])
    base = black(g43)
    worst = max(black(s) for s in side)
    if worst > base + 0.15:
        flags.append('LOW black in side band %.0f%% (4:3 frame %.0f%%)' % (100 * worst, 100 * base))
    a = luma(img43).resize((240, 240)).crop((0, 160, 240, 240))
    b = luma(img169).crop((40, 160, 280, 240)).resize((240, 80))
    pa, pb = a.load(), b.load()
    diff = sum(abs(pa[x, y] - pb[x, y]) for y in range(80) for x in range(240)) / (240 * 80)
    return flags + (['MED 2D bottom mismatch %.0f' % diff] if diff > 40 else [])


def image_flags(i43, i169):
    if i43 is None or is_black(i43):
        return ['LOW black 4:3 frame (empty area or fade)']
    flags = edge_flags(i169) if i169 is not None else []  # also in dark rooms: a 4:3-only shade shows there
    if sum(luma(i43).histogram()[:12]) > 0.7 * i43.size[0] * i43.size[1]:
        return flags + ['LOW mostly black 4:3 frame (empty area, fade or dark room)']
    return flags + (side_and_2d_flags(i43, i169) if i169 is not None else [])


def reflag(out):
    """Recompute the image flags of a finished tour from its saved frames (no harness runs)."""
    for r in load_results(out):
        d = os.path.join(out, '%03d' % r['area'])
        imgs = {t: Image.open(os.path.join(d, v[0])) if v[0] else None for t, v in r['frames'].items()}
        keep = [f for f in r['flags'] if f.startswith('HIGH') and 'edge step' not in f]
        flags = keep + image_flags(imgs.get('43'), imgs.get('169'))
        with open(os.path.join(d, 'result.json'), 'w', encoding='utf-8') as f:
            json.dump({'area': r['area'], 'flags': flags, 'frames': {t: v[0] for t, v in r['frames'].items()}}, f)


def tour_area(o, area, start):
    d = os.path.join(o.out, '%03d' % area)
    os.makedirs(d, exist_ok=True)
    stop = start + o.frames
    res = {'area': area, 'flags': [], 'frames': {}}
    for tag, extra in (('43', []), ('169', ['--widescreen', '16:9'])):
        fd = os.path.join(d, tag)
        os.makedirs(fd, exist_ok=True)
        for n in os.listdir(fd):
            os.remove(os.path.join(fd, n))
        args = ['--boot', o.disc, '--load-state', o.state, '--warp-map', str(area), '--max-frames', str(stop),
                '--dump-frames', fd, '--every', str(o.every)] + extra
        code, text = run(o.exe, args, os.path.join(d, tag + '.log'), o.timeout)
        if code == 'timeout':
            res['flags'].append('HIGH hang (%s timeout)' % tag)
        elif code != 0:
            res['flags'].append('HIGH crash (%s exit %s)' % (tag, code))
        if tag == '43':
            if 'warp area' not in text:
                res['flags'].append('HIGH no warp (state not in the F0042 field?)')
            elif not re.search(r'warp area[\s\S]*load_file_by_id\(id=42 ', text):
                res['flags'].append('HIGH field not reloaded')
            m = re.search(r'unimplemented:([^\n]*)', text)
            if m and m.group(1).strip() != 'none':
                res['flags'].append('HIGH BIOS unimplemented:' + m.group(1))
            m = re.search(r'STOP: frame limit or engine stopped.*', text)
            last = re.findall(r'\[BOOT f=(\d+) ', text)
            if last and int(last[-1]) < stop and code == 0:
                res['flags'].append('HIGH engine stopped at f%s' % last[-1])
        name, img = last_frame(fd)
        res['frames'][tag] = (os.path.join(tag, name) if name else None, img)
    res['flags'] += image_flags(res['frames']['43'][1], res['frames']['169'][1])
    with open(os.path.join(d, 'result.json'), 'w', encoding='utf-8') as f:
        json.dump({'area': area, 'flags': res['flags'], 'frames': {t: v[0] for t, v in res['frames'].items()}}, f)
    return res


def load_results(out):
    """Every toured area in the folder, so tours of several ranges share one sheet."""
    results = []
    for n in sorted(os.listdir(out)):
        p = os.path.join(out, n, 'result.json')
        if os.path.isfile(p):
            with open(p, encoding='utf-8') as f:
                r = json.load(f)
            r['frames'] = {t: (v, None) for t, v in r['frames'].items()}
            results.append(r)
    return results


def write_sheet(out, results, title):
    bad = sum(1 for r in results if any(not f.startswith('LOW') for f in r['flags']))
    rows = []
    for r in results:
        cells = []
        for tag, width in (('43', 320), ('169', 427)):
            rel = r['frames'][tag][0]
            cells.append('<td>%s</td>' % ('<img src="%03d/%s" width="%d" height="240">' % (r['area'], rel.replace(os.sep, '/'), width)
                                          if rel else '-'))
        flags = '<br>'.join(html.escape(f) for f in r['flags']) or 'ok'
        sev = next((s.lower() for s in ('HIGH', 'MED', 'LOW') if any(f.startswith(s) for f in r['flags'])), 'ok')
        rows.append('<tr class="%s"><td>%d<br>0x%02X</td>%s<td>%s</td></tr>' % (sev, r['area'], r['area'], ''.join(cells), flags))
    page = ('<!doctype html><meta charset="utf-8"><title>Map tour</title><style>body{font:13px sans-serif;background:#111;color:#ddd}'
            'td{border-bottom:1px solid #333;padding:4px;vertical-align:top}tr.high td:last-child{color:#f66}tr.med td:last-child{color:#fb4}tr.low td:last-child{color:#999}img{image-rendering:pixelated}'
            '</style><h1>%s</h1><p>%d areas, %d with HIGH/MED flags (LOW: side bands with no geometry, usually a small room). 16:9 frames are anamorphic captures shown stretched to 16:9.</p>'
            '<label><input type="checkbox" onchange="document.querySelectorAll(\'tr.ok,tr.low\').forEach(r=>r.hidden=this.checked)"> HIGH/MED only</label>'
            '<table><tr><th>area</th><th>4:3</th><th>16:9</th><th>flags</th></tr>%s</table>') % (html.escape(title), len(results), bad, '\n'.join(rows))
    with open(os.path.join(out, 'index.html'), 'w', encoding='utf-8') as f:
        f.write(page)
    md = ['| area | flags |', '|---|---|'] + ['| %d | %s |' % (r['area'], '; '.join(r['flags']) or 'ok') for r in results]
    with open(os.path.join(out, 'summary.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(md) + '\n')
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe')
    ap.add_argument('--disc')
    ap.add_argument('--state', help='save state in the F0042 field (docs/MAP_TOUR.md)')
    ap.add_argument('--out', required=True, help='output folder (keep it outside the repo)')
    ap.add_argument('--areas', default='0-127')
    ap.add_argument('--frames', type=int, default=600, help='frames to run after the state (default 600)')
    ap.add_argument('--every', type=int, default=150)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--timeout', type=float, default=180)
    ap.add_argument('--title', default='PersonaRM map tour')
    ap.add_argument('--reflag', action='store_true', help='only recompute flags and the sheet from --out')
    o = ap.parse_args()
    if o.reflag:
        reflag(o.out)
        every = load_results(o.out)
        print('%d areas, %d flagged: %s' % (len(every), write_sheet(o.out, every, o.title), os.path.join(o.out, 'index.html')))
        return
    if not (o.exe and o.disc and o.state):
        ap.error('--exe, --disc and --state are needed for a tour')
    o.exe, o.disc, o.state, o.out = (os.path.abspath(p) for p in (o.exe, o.disc, o.state, o.out))
    os.makedirs(o.out, exist_ok=True)
    # Frame the state was saved at: the harness prints it on load.
    code, text = run(o.exe, ['--boot', o.disc, '--load-state', o.state, '--max-frames', '0'], os.path.join(o.out, 'probe.log'), 120)
    m = re.search(r'loaded state .* at frame (\d+)', text)
    if not m:
        sys.exit('cannot load the state (see probe.log)')
    start = int(m.group(1))
    areas = parse_areas(o.areas)
    results = []
    with cf.ThreadPoolExecutor(o.jobs) as ex:
        for r in ex.map(lambda a: tour_area(o, a, start), areas):
            print('area %3d: %s' % (r['area'], '; '.join(r['flags']) or 'ok'), flush=True)
            results.append(r)
    every = load_results(o.out)
    bad = write_sheet(o.out, every, o.title)
    print('%d areas toured now, %d in the sheet, %d flagged: %s' % (len(results), len(every), bad, os.path.join(o.out, 'index.html')))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--self-test':
        assert parse_areas('0-2,9,0x10') == [0, 1, 2, 9, 16]
        img = Image.new('RGB', (320, 240), (200, 200, 200))
        for x in range(40):  # dark left band with a hard seam at x=40
            for y in range(240):
                img.putpixel((x, y), (0, 0, 0))
        assert any('HIGH edge step x=40' in f for f in edge_flags(img)), edge_flags(img)
        assert edge_flags(Image.new('RGB', (320, 240), (90, 90, 90))) == []
        print('self-test ok')
    else:
        main()
