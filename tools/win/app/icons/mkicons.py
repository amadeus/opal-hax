# Subsets Microsoft's Fluent UI System Icons (MIT) to the glyphs Opal for Windows uses -> app/icons/fluent_icons.h
import json, sys, io
from fontTools import subset
from fontTools.ttLib import TTFont
src, js, out = sys.argv[1:4]
d = json.load(open(js))
want = ("mic mic_off video camera color flip_horizontal zoom_in video_person blur grid_dots lightbulb_person sparkle emoji "
        "weather_sunny weather_moon data_histogram text_font image hand_draw emoji_hand hand_right brightness_high drop "
        "temperature target lock_closed options lightbulb record power info chevron_down checkmark settings dismiss").split()
cps = {w: d[f'ic_fluent_{w}_20_regular'] for w in want}
opts = subset.Options(); opts.layout_features = []; opts.name_IDs = ['*']; opts.notdef_outline = True
f = TTFont(src); sub = subset.Subsetter(opts); sub.populate(unicodes=list(cps.values())); sub.subset(f)
buf = io.BytesIO(); f.save(buf); data = buf.getvalue()
with open(out, 'w') as o:
    o.write('// Fluent UI System Icons (Regular, 20 px set), subset for Opal for Windows by tools/win/app/icons/mkicons.py.\n'
            '// Copyright (c) 2020 Microsoft Corporation, MIT License (see LICENSE in this folder).\n#pragma once\n\n')
    o.write('#define ICON_MIN 0x%x\n#define ICON_MAX 0x%x\n' % (min(cps.values()), max(cps.values())))
    for w, c in cps.items():
        o.write('#define ICON_%s "%s"\n' % (w.upper(), ''.join('\\x%02x' % b for b in chr(c).encode())))
    o.write('\nstatic const unsigned char kFluentIconsTtf[%d] = {\n' % len(data))
    for i in range(0, len(data), 24): o.write('    ' + ','.join(str(b) for b in data[i:i+24]) + ',\n')
    o.write('};\n')
print(len(data), 'bytes,', len(cps), 'icons')
