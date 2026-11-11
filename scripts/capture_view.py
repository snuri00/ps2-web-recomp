import html
import os
import sys

PSM = {0x00: 'CT32', 0x01: 'CT24', 0x02: 'CT16', 0x0A: 'CT16S', 0x13: 'T8', 0x14: 'T4', 0x1B: 'T8H', 0x24: 'T4HL', 0x2C: 'T4HH',
       0x30: 'Z32', 0x31: 'Z24', 0x32: 'Z16', 0x3A: 'Z16S'}
PRIMS = ['point', 'line', 'line strip', 'triangle', 'tri strip', 'tri fan', 'sprite', '?']
WRAP = ['repeat', 'clamp', 'region clamp', 'region repeat']
ZTST = ['never', 'always', 'gequal', 'greater']
ATST = ['never', 'always', 'less', 'lequal', 'equal', 'gequal', 'greater', 'notequal']
AFAIL = ['keep', 'fb only', 'zb only', 'rgb only']
BLEND = ['Cs', 'Cd', '0', '?']
ALPHA = ['As', 'Ad', 'FIX', '?']


def psm(v):
    return PSM.get(v, '%02X' % v)


def describe(fields):
    prim, frame, zbuf, tex0, tex1, clamp, alpha, test = (int(f, 16) for f in fields[4:12])
    flags = [name for bit, name in ((3, 'gouraud'), (4, 'textured'), (5, 'fog'), (6, 'blend'), (7, 'aa'), (8, 'uv')) if prim & (1 << bit)]
    out = {
        'prim': PRIMS[prim & 7] + (' ' + ' '.join(flags) if flags else ''),
        'target': 'fbp %X (bp %X) fbw %d %s mask %08X' % (frame & 0x1FF, (frame & 0x1FF) * 32, (frame >> 16) & 0x3F, psm((frame >> 24) & 0x3F), frame >> 32),
        'z': 'zbp %X %s%s' % (zbuf & 0x1FF, psm(0x30 | ((zbuf >> 24) & 0xF)), ' no write' if zbuf >> 32 & 1 else ''),
    }
    if prim & 0x10:
        out['texture'] = 'tbp %X tbw %d %s %dx%d %s%s, cbp %X' % (tex0 & 0x3FFF, (tex0 >> 14) & 0x3F, psm((tex0 >> 20) & 0x3F), 1 << ((tex0 >> 26) & 15),
                                                          1 << ((tex0 >> 30) & 15), 'rgba' if tex0 >> 34 & 1 else 'rgb',
                                                          ['modulate', 'decal', 'highlight', 'highlight2'][(tex0 >> 35) & 3], (tex0 >> 37) & 0x3FFF)
        out['wrap'] = 'u %s %d-%d, v %s %d-%d, %s' % (WRAP[clamp & 3], (clamp >> 4) & 0x3FF, (clamp >> 14) & 0x3FF, WRAP[(clamp >> 2) & 3],
                                                    (clamp >> 24) & 0x3FF, (clamp >> 34) & 0x3FF, 'bilinear' if tex1 >> 5 & 1 else 'nearest')
    if prim & 0x40:
        out['blend'] = '(%s - %s) * %s + %s, fix %d' % (BLEND[alpha & 3], BLEND[(alpha >> 2) & 3], ALPHA[(alpha >> 4) & 3], BLEND[(alpha >> 6) & 3], (alpha >> 32) & 0xFF)
    tests = []
    if test & 1:
        tests.append('alpha %s %d fail %s' % (ATST[(test >> 1) & 7], (test >> 4) & 0xFF, AFAIL[(test >> 12) & 3]))
    if test >> 14 & 1:
        tests.append('dest alpha %d' % ((test >> 15) & 1))
    if test >> 16 & 1:
        tests.append('z %s' % ZTST[(test >> 17) & 3])
    if tests:
        out['tests'] = ', '.join(tests)
    out['box'] = fields[12]
    if prim & 0x10:
        out['texels'] = fields[13]
    return out


def main():
    if len(sys.argv) < 2:
        print('usage: capture_view.py <folder written by PS2_CAPTURE>')
        return 1
    folder = sys.argv[1]
    try:
        from PIL import Image
    except ImportError:
        Image = None
    rows = []
    for line in open(os.path.join(folder, 'draws.tsv')):
        fields = line.rstrip('\n').split('\t')
        if fields[0] == 'draw':
            continue
        if fields[0] == 'image':
            name = fields[1]
            if Image:
                png = name[:-4] + '.png'
                Image.open(os.path.join(folder, name)).save(os.path.join(folder, png))
                name = png
            rows.append('<tr class="image"><td colspan="3">%s<br><img src="%s"></td></tr>' % (html.escape(fields[2] + ': ' + name), html.escape(name)))
        elif fields[0] == 'texture':
            name = fields[1]
            if Image:
                png = name[:-4] + '.png'
                Image.open(os.path.join(folder, name)).save(os.path.join(folder, png))
                name = png
            rows.append('<tr class="image"><td colspan="3">%s<br><img src="%s"></td></tr>' % (html.escape(fields[2] + ': ' + name), html.escape(name)))
        elif fields[0] in ('vblank', 'transfer'):
            rows.append('<tr class="event"><td colspan="3">%s</td></tr>' % html.escape(' '.join(fields)))
        else:
            info = describe(fields)
            text = '<br>'.join('<b>%s</b> %s' % (html.escape(k), html.escape(v)) for k, v in info.items())
            rows.append('<tr><td>%s</td><td>%s prims</td><td>%s</td></tr>' % (fields[0], fields[1], text))
    page = ('<!doctype html><meta charset="utf-8"><title>GS capture</title><style>body{font:13px monospace;background:#111;color:#ddd}'
            'td{border-bottom:1px solid #333;padding:4px;vertical-align:top}tr.event td{color:#8ac}tr.image td{color:#ca8}'
            'img{image-rendering:pixelated;max-width:100%%;border:1px solid #555}</style><table>%s</table>') % '\n'.join(rows)
    with open(os.path.join(folder, 'index.html'), 'w') as out:
        out.write(page)
    print('wrote %s' % os.path.join(folder, 'index.html'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
