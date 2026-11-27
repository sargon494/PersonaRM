#!/usr/bin/env python3
"""Map tour: warp to every field area from one save state, capture 4:3 and 16:9, flag suspicious frames.

Developer tool (not shipped). See docs/MAP_TOUR.md.

    python tools/map_tour/map_tour.py --exe build/Release/Release/personarm.exe --disc <disc1.cue> \
        --state <field.state> --out <folder outside the repo> --areas 0-120 [--jobs 6]

Needs Pillow. Writes <out>/<area>/{43,169}/frame_*.png, <out>/<area>/*.log and <out>/index.html.
JP or US disc 1. --sdl <personarm_sdl.exe> adds the player-config pass (GPU 4x, --interp 120) per area.
"""
import argparse
import concurrent.futures as cf
import functools
import html
import json
import os
import re
import subprocess
import sys

try:
    from PIL import Image, ImageChops, ImageFilter, ImageStat
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
        keep = [f for f in r['flags'] if f.startswith('HIGH') and 'edge step' not in f and 'GPU' not in f]
        g = r.get('gpu') or {}
        gpu = [f for f in r['flags'] if 'GPU pass' in f] + gpu_flags(g.get('metrics') or {})
        r['flags'] = keep + image_flags(imgs.get('43'), imgs.get('169')) + gpu
        save_result(d, r)


# ---- Player-config pass (--sdl): personarm_sdl with the GPU renderer, 4x and 120 Hz interpolation ----------------
# Thresholds (luma 0..255). The software 60 Hz run of the same state and input at the same --resolution-scale
# (SCALE, no interpolation) is the reference: emulation does not depend on the renderer, so its frame f is the
# picture the game drew at f, and a non-interpolated GPU present must equal it exactly ('inexact').
SCALE = 4              # internal resolution scale of both the GPU run and its software reference
WINDOW = (-4, 3)       # software frames f-4..f+3 can be on screen at GPU present f (interpolation is late; the
                       # GPU picture of a double-buffered game can also be up to two frames ahead of the display)
BACK_LEVEL = 0.5       # a present closer to an older frame than to the one already shown, by this mean luma
JITTER_LEVEL = 8       # GPU present-to-present change, averaged over each 1x pixel's kxk block
JITTER_PIXELS = 20     # 1x pixels that may change between presents while still in the game: MED above
JITTER_HIGH = 300      # ... HIGH above (a visible flicker rather than a sub-pixel edge wobble)
LINE_LEVEL = 48        # GPU pixel vs the software frame upscaled (nearest): dither and filtering stay below
LINE_PIXELS = 400      # thin (1-2 px wide) stray hi-res pixels on still, edge-free areas before flagging
GROSS_LEVEL = 16       # median over presents of the mean |GPU downscaled - software| of the best frame of the window
GPU_TAIL = 90          # presents dumped for frames stop+1..stop+90: still until +60, then right held 30 frames
SETTLE = 8             # frames after the state load not judged (GPU upscaled VRAM and interpolation history refill)
SPIKE_LEVEL = 8        # one present's mean |GPU - software| above the median of the 8 presents around it
EXACT_PRESENTS = 1     # presents at phase 0 (no interpolation) that may differ from every software frame of the window: MED above
KIND_WORD = {'back': 'back in time', 'jitter': 'jitter', 'lines': 'lines', 'spike': 'spike', 'gross': 'differs'}  # crop -> flag


def mask(img, level):
    return img.point(lambda v: 255 if v > level else 0)


def count(m):
    return m.histogram()[255]


def hotspot(m, w=80, h=60):
    """Box (in m's pixels) of w x h around the 16x16 cell with the most set pixels."""
    cw, ch = max(1, m.size[0] // 16), max(1, m.size[1] // 16)
    grid = list(m.resize((cw, ch), Image.BOX).tobytes())
    i = grid.index(max(grid))
    cx, cy = (i % cw) * 16 + 8, (i // cw) * 16 + 8
    x0 = min(max(0, cx - w // 2), max(0, m.size[0] - w))
    y0 = min(max(0, cy - h // 2), max(0, m.size[1] - h))
    return (x0, y0, x0 + w, y0 + h)


def save_crop(path, imgs, box, k):
    """Side by side crops (box in 1x pixels) of the widest images (k x) and of 1x ones, all shown at k x."""
    parts, wide = [], max(im.size[0] for im in imgs)
    for im in imgs:
        s = max(1, round(im.size[0] * k / wide))
        c = im.crop(tuple(v * s for v in box))
        parts.append(c.resize(((box[2] - box[0]) * k, (box[3] - box[1]) * k), Image.NEAREST).convert('RGB'))
    out = Image.new('RGB', (sum(p.size[0] for p in parts) + 4 * (len(parts) - 1), parts[0].size[1]), (255, 0, 255))
    x = 0
    for p in parts:
        out.paste(p, (x, 0))
        x += p.size[0] + 4
    out.save(path)


def thin_strays(gpu_l, sw_l, still_hi, k):
    """Hi-res pixels far from the upscaled software frame, away from its edges, on still areas, and thin
    (gone after a 3x3 opening): seams, outlines and stray lines rather than shape or shading changes."""
    up = sw_l.resize(gpu_l.size, Image.NEAREST)  # no-op when the reference has the GPU's size
    edges = mask(sw_l.filter(ImageFilter.FIND_EDGES), 24).filter(ImageFilter.MaxFilter(2 * k + 1)).resize(gpu_l.size, Image.NEAREST)
    stray = ImageChops.darker(ImageChops.subtract(mask(ImageChops.difference(gpu_l, up), LINE_LEVEL), edges), still_hi)
    return ImageChops.subtract(stray, stray.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(3)))


def analyse_gpu(pg, ps, gd, start, k=SCALE):
    """Metrics, crops and a representative picture from the dumped GPU presents (pg) and software frames (ps),
    both k x. Pixels are compared at k x; counts and crops are in 1x units like the thresholds."""
    sw, swc = {}, {}
    for n in os.listdir(ps):
        m = re.match(r'f(\d+)_0_t\d+\.png$', n)
        if m:
            swc[int(m.group(1))] = Image.open(os.path.join(ps, n)).convert('RGB')
            sw[int(m.group(1))] = luma(swc[int(m.group(1))])
    pres = []
    for n in sorted(os.listdir(pg)):
        m = re.match(r'f(\d+)_(\d+)_t(\d+)\.png$', n)
        if m and int(m.group(1)) >= start + SETTLE:
            pres.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), os.path.join(pg, n)))  # frame, sub-present, phase t
    if not pres or not sw:
        return ['HIGH GPU pass: no pictures'], [], None, {}
    still = {}
    def still_mask(f):  # 255 where the software frames of the window agree (grown by one pixel)
        if f not in still:
            fs = [sw.get(i) for i in range(f + WINDOW[0], f + WINDOW[1] + 1)]
            if any(x is None for x in fs) or any(x.size != fs[-1].size for x in fs):
                still[f] = None
            else:
                ch = Image.new('L', fs[-1].size, 0)
                for a, b in zip(fs, fs[1:]):
                    ch = ImageChops.lighter(ch, mask(ImageChops.difference(a, b), 0))
                still[f] = ImageChops.invert(ch.filter(ImageFilter.MaxFilter(3)))
        return still[f]
    worst = {'jitter': (0, None), 'lines': (0, None), 'spike': (0, None), 'gross': (0, None), 'back': (0, None)}
    inexact = 0
    seq, prev, shot = [], None, None  # seq: (gross, label, small GPU, best software) per comparable present
    for f, s, t, path in pres:
        rgb = Image.open(path).convert('RGB')
        g = luma(rgb)
        ref = sw.get(f)
        if not ref or g.size != ref.size or ref.size[0] % k:
            prev = None
            continue  # 24bpp / display mode change: not comparable
        base = (ref.size[0] // k, ref.size[1] // k)
        dist = {i: ImageStat.Stat(ImageChops.difference(g, sw[i])).mean[0]
                for i in range(f + WINDOW[0], f + WINDOW[1] + 1) if i in sw and sw[i].size == ref.size}
        bi = min(dist, key=dist.get)
        gr = dist[bi]
        shown = [i for i in dist if dist[i] <= gr + BACK_LEVEL]  # the game frames this present can be
        label = 'f%d/%d' % (f, s)
        if t in (0, 999) and not any(ImageChops.difference(rgb, swc[i]).getbbox() is None for i in dist):
            inexact += 1  # a game frame the GPU drew must equal one the software renderer drew
        # Back in time: this present is an older game frame than one a previous present already showed.
        if prev is not None and prev[4] and max(shown) < prev[4] and prev[4] in dist and dist[prev[4]] > gr + BACK_LEVEL:
            back = prev[4] - max(shown)
            if back > worst['back'][0]:
                change = ImageChops.difference(prev[3], rgb).point(lambda v: min(255, 4 * v))
                hs = [v * k for v in hotspot(mask(ImageChops.difference(prev[2], g).resize(base, Image.BOX), JITTER_LEVEL))]
                worst['back'] = (back, (hs, [prev[3], rgb, change], 1, '%s shows f%d after %s showed f%d' % (label, max(shown), prev[1], prev[4])))
        small, best1 = rgb.resize(base, Image.BOX), swc[bi].resize(base, Image.BOX)  # 1x for the sheet
        gr1 = ImageStat.Stat(ImageChops.difference(luma(small), luma(best1))).mean[0]  # the thresholds below are in 1x units
        seq.append((gr1, label, small, best1, t in (0, 999)))
        if gr1 > worst['gross'][0]:
            worst['gross'] = (gr1, ((0, 0) + base, [small, best1], 1, label))
        sm = still_mask(f)
        if sm is not None:
            if prev is not None and prev[0] >= f - 1 and prev[2].size == g.size:
                sm1 = sm.resize(base, Image.BOX).point(lambda v: 255 if v > 249 else 0)  # still over the whole 1x pixel
                jit = ImageChops.darker(mask(ImageChops.difference(prev[2], g).resize(base, Image.BOX), JITTER_LEVEL), sm1)
                if count(jit) > worst['jitter'][0]:
                    change = ImageChops.difference(prev[3], rgb).point(lambda v: min(255, 4 * v))
                    worst['jitter'] = (count(jit), ([v * k for v in hotspot(jit)], [prev[3], rgb, change], 1, '%s vs %s' % (prev[1], label)))
            th = thin_strays(g, sw[bi], sm, k)
            if count(th) > worst['lines'][0]:
                hs = hotspot(th.resize(base, Image.BOX).point(lambda v: 255 if v else 0))
                worst['lines'] = (count(th), ([v * k for v in hs], [rgb, swc[bi]], 1, label))
        newest = max(min(shown), prev[4] if prev is not None and prev[4] else 0)  # newest frame shown so far
        prev = (f, label, g, rgb, newest)
        if f <= start + 60:
            shot = rgb
    # A spike: one present much farther from the game's frames than the presents around it (a pop or flash).
    for i, (gr, label, small, best, exact) in enumerate(seq):  # exact presents are 0 away, blends are not: compare like with like
        around = sorted(x[0] for x in seq[max(0, i - 8):i] + seq[i + 1:i + 9] if x[4] == exact)
        if around and gr - around[len(around) // 2] > worst['spike'][0]:
            worst['spike'] = (gr - around[len(around) // 2], ((0, 0) + small.size, [seq[i - 1][2] if i else small, small, best], 1,
                                                            '%s (previous | this | software)' % label))
    grs = sorted(x[0] for x in seq)
    metrics = {'jitter_px': worst['jitter'][0], 'line_px': worst['lines'][0], 'spike': round(worst['spike'][0], 1),
               'back_frames': worst['back'][0], 'inexact': inexact,
               'gross_median': round(grs[len(grs) // 2], 1) if grs else 0, 'presents': len(pres)}
    crops = []
    for kind in ('back', 'jitter', 'lines', 'spike', 'gross'):  # the worst case of each, flagged or not (--reflag)
        if worst[kind][1]:
            box, imgs, k, what = worst[kind][1]
            save_crop(os.path.join(gd, 'crop_%s.png' % kind), imgs, box, k)
            crops.append(('crop_%s.png' % kind, '%s %s' % (kind, what)))
    return gpu_flags(metrics), crops, shot, metrics


def gpu_flags(m):
    flags = []
    if m.get('back_frames', 0) > 0:
        flags.append('HIGH GPU present went back in time (%d frames)' % m['back_frames'])
    if m.get('jitter_px', 0) > JITTER_HIGH:
        flags.append('HIGH GPU jitter in a still scene (%d px)' % m['jitter_px'])
    elif m.get('jitter_px', 0) > JITTER_PIXELS:
        flags.append('MED GPU jitter in a still scene (%d px)' % m['jitter_px'])
    if m.get('line_px', 0) > LINE_PIXELS:
        flags.append('MED GPU thin stray lines (%d hi-res px)' % m['line_px'])
    if m.get('gross_median', 0) > GROSS_LEVEL:
        flags.append('MED GPU differs from software (median %.0f)' % m['gross_median'])
    if m.get('inexact', 0) > EXACT_PRESENTS:
        flags.append('MED GPU frame differs from software at the same scale (%d presents)' % m['inexact'])
    if m.get('spike', 0) > SPIKE_LEVEL:
        flags.append('MED GPU spike: one present off the game frames (+%.0f)' % m['spike'])
    return flags


def gpu_pass(o, area, d, stop, state):
    """personarm_sdl from the area's state: GPU 4x 120 Hz presents and the software 4x 60 Hz reference."""
    gd = os.path.join(d, 'gpu')
    pg, ps, sd = (os.path.join(gd, x) for x in ('pg', 'ps', 'sd'))
    for x in (pg, ps, sd):
        os.makedirs(x, exist_ok=True)
        for n in os.listdir(x):
            os.remove(os.path.join(x, n))
    for n in os.listdir(gd):
        if n.startswith('crop_') or n == 'shot.png':
            os.remove(os.path.join(gd, n))
    os.replace(state, os.path.join(sd, o.serial + '.state0'))  # --load-state N reads <save_dir>/<serial>.state<N>
    with open(os.path.join(gd, 'settings.json'), 'w', encoding='utf-8') as f:
        json.dump({'general': {'save_dir': sd, 'pause_on_focus_loss': False}}, f)
    with open(os.path.join(gd, 'input.txt'), 'w', encoding='utf-8') as f:
        f.write('%d right 30\n' % (stop + 61))
    end = stop + GPU_TAIL
    flags = []
    for tag, dump, extra in (('gpu', pg, ['--renderer', 'gpu', '--resolution-scale', str(SCALE), '--interp', '120']),
                             ('sw', ps, ['--renderer', 'software', '--resolution-scale', str(SCALE), '--interp', '60'])):
        last = end + (WINDOW[1] if tag == 'sw' else 0)
        args = [o.disc, '--settings', os.path.join(gd, 'settings.json'), '--hidden', '--unthrottled', '--mute',
                '--no-recomp', '--load-state', '0', '--frames', str(last), '--dump-dir', dump, '--dump-from',
                str(stop + 1), '--dump-to', str(last), '--input-script', os.path.join(gd, 'input.txt')] + extra
        code, text = run(o.sdl, args, os.path.join(gd, tag + '.log'), o.timeout)
        if code == 'timeout':
            flags.append('HIGH GPU pass: hang (%s)' % tag)
        elif code != 0:
            flags.append('HIGH GPU pass: crash (%s exit %s)' % (tag, code))
        if 'loaded slot 0' not in text:
            flags.append('HIGH GPU pass: could not load the state (%s)' % tag)
        if '[gpu] present failed' in text:
            flags.append('HIGH GPU pass: present failed (fell back to software)')
    if flags:
        return flags, [], None, {}
    f2, crops, shot, metrics = analyse_gpu(pg, ps, gd, stop)
    if shot is not None:
        shot.resize((640, 480), Image.BOX).save(os.path.join(gd, 'shot.png'))
    if not o.keep_presents:
        for x in (pg, ps, sd):
            for n in os.listdir(x):
                os.remove(os.path.join(x, n))
    return f2, crops, 'gpu/shot.png' if shot is not None else None, metrics


def tour_area(o, area, start):
    d = os.path.join(o.out, '%03d' % area)
    os.makedirs(d, exist_ok=True)
    stop = start + o.frames
    res = {'area': area, 'flags': [], 'frames': {}}
    state = os.path.join(d, 'area.state')
    for tag, extra in (('43', []), ('169', ['--widescreen', '16:9'])):
        fd = os.path.join(d, tag)
        os.makedirs(fd, exist_ok=True)
        for n in os.listdir(fd):
            os.remove(os.path.join(fd, n))
        args = ['--boot', o.disc, '--load-state', o.state, '--warp-map', str(area), '--max-frames', str(stop),
                '--dump-frames', fd, '--every', str(o.every)] + extra
        if tag == '43' and o.sdl:
            args += ['--save-state-at', str(stop), state]
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
    if o.sdl:
        if os.path.isfile(state):
            gf, crops, shot, metrics = gpu_pass(o, area, d, stop, state)
            res['flags'] += gf
            res['gpu'] = {'crops': crops, 'shot': shot, 'metrics': metrics}
        else:
            res['flags'].append('HIGH GPU pass: no area state from the harness')
    save_result(d, res)
    return {'area': area, 'flags': res['flags']}


def save_result(d, r):
    with open(os.path.join(d, 'result.json'), 'w', encoding='utf-8') as f:
        json.dump({'area': r['area'], 'flags': r['flags'], 'frames': {t: v[0] for t, v in r['frames'].items()},
                   'gpu': r.get('gpu')}, f)


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
    gpu = any(r.get('gpu') for r in results)
    for r in results:
        cells = []
        for tag, width in (('43', 320), ('169', 427)):
            rel = r['frames'][tag][0]
            cells.append('<td>%s</td>' % ('<img src="%03d/%s" width="%d" height="240">' % (r['area'], rel.replace(os.sep, '/'), width)
                                          if rel else '-'))
        if gpu:
            g = r.get('gpu') or {}
            cells.append('<td>%s</td>' % ('<img src="%03d/%s" width="320" height="240">' % (r['area'], g['shot']) if g.get('shot') else '-'))
            cells.append('<td>%s<br><small>%s</small></td>' % (
                ''.join('<div><img src="%03d/gpu/%s" height="120"><br>%s</div>' % (r['area'], n, html.escape(w)) for n, w in g.get('crops', [])
                        if any(KIND_WORD[w.split()[0]] in f for f in r['flags'])),
                html.escape(' '.join('%s=%s' % kv for kv in sorted((g.get('metrics') or {}).items())))))
        flags = '<br>'.join(html.escape(f) for f in r['flags']) or 'ok'
        sev = next((s.lower() for s in ('HIGH', 'MED', 'LOW') if any(f.startswith(s) for f in r['flags'])), 'ok')
        rows.append('<tr class="%s"><td>%d<br>0x%02X</td>%s<td>%s</td></tr>' % (sev, r['area'], r['area'], ''.join(cells), flags))
    page = ('<!doctype html><meta charset="utf-8"><title>Map tour</title><style>body{font:13px sans-serif;background:#111;color:#ddd}'
            'td{border-bottom:1px solid #333;padding:4px;vertical-align:top}tr.high td:last-child{color:#f66}tr.med td:last-child{color:#fb4}tr.low td:last-child{color:#999}img{image-rendering:pixelated}'
            '</style><h1>%s</h1><p>%d areas, %d with HIGH/MED flags (LOW: side bands with no geometry, usually a small room). 16:9 frames are anamorphic captures shown stretched to 16:9.</p>'
            '<label><input type="checkbox" onchange="document.querySelectorAll(\'tr.ok,tr.low\').forEach(r=>r.hidden=this.checked)"> HIGH/MED only</label>'
            '%s<table><tr><th>area</th><th>4:3</th><th>16:9</th>%s<th>flags</th></tr>%s</table>') % (
                html.escape(title), len(results), bad,
                '<p>GPU: personarm_sdl --renderer gpu --resolution-scale 4 --interp 120, 4:3, still for 60 frames then right held 30; '
                'crops: GPU | software 1x upscaled (lines), present | next present (jitter), live | synthesized | live (interp).</p>' if gpu else '',
                '<th>GPU 4x 120 Hz</th><th>GPU crops</th>' if gpu else '', '\n'.join(rows))
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
    ap.add_argument('--sdl', help='personarm_sdl.exe: also run the player-config pass (GPU 4x, --interp 120)')
    ap.add_argument('--keep-presents', action='store_true', help='keep the dumped GPU/software presents (large)')
    o = ap.parse_args()
    if o.reflag:
        reflag(o.out)
        every = load_results(o.out)
        print('%d areas, %d flagged: %s' % (len(every), write_sheet(o.out, every, o.title), os.path.join(o.out, 'index.html')))
        return
    if not (o.exe and o.disc and o.state):
        ap.error('--exe, --disc and --state are needed for a tour')
    o.exe, o.disc, o.state, o.out = (os.path.abspath(p) for p in (o.exe, o.disc, o.state, o.out))
    o.sdl = os.path.abspath(o.sdl) if o.sdl else None
    os.makedirs(o.out, exist_ok=True)
    # Frame the state was saved at: the harness prints it on load.
    code, text = run(o.exe, ['--boot', o.disc, '--load-state', o.state, '--max-frames', '0'], os.path.join(o.out, 'probe.log'), 120)
    m = re.search(r'loaded state .* at frame (\d+)', text)
    if not m:
        sys.exit('cannot load the state (see probe.log)')
    start = int(m.group(1))
    m = re.search(r'\] disc (\S+) entry', text)
    o.serial = m.group(1) if m else 'unknown'
    areas = parse_areas(o.areas)
    results = []
    with cf.ProcessPoolExecutor(o.jobs) as ex:  # processes: the GPU-pass image analysis is CPU bound
        for r in ex.map(functools.partial(tour_area, o, start=start), areas):
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
        sw = Image.new('L', (320, 240), 100)
        still = Image.new('L', (1280, 960), 255)
        hi = sw.resize((1280, 960))
        assert count(thin_strays(hi, sw, still, 4)) == 0
        seam = hi.copy()
        seam.paste(255, (0, 500, 1280, 501))  # a 1 px line on a flat area: a seam
        assert count(thin_strays(seam, sw, still, 4)) > LINE_PIXELS
        blob = hi.copy()
        blob.paste(255, (400, 400, 480, 480))  # a solid shape: not thin
        assert count(thin_strays(blob, sw, still, 4)) < 50, count(thin_strays(blob, sw, still, 4))
        assert gpu_flags({'inexact': EXACT_PRESENTS + 1})[0].startswith('MED GPU frame differs')
        assert gpu_flags({'inexact': EXACT_PRESENTS}) == []
        assert gpu_flags({'jitter_px': JITTER_HIGH + 1})[0].startswith('HIGH GPU jitter')
        assert gpu_flags({'jitter_px': JITTER_PIXELS, 'line_px': 0, 'spike': 0, 'gross_median': 0}) == []
        print('self-test ok')
    else:
        main()
