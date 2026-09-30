# 设计稿(printer-hmi-redesign.html)色彩 token → RGB565 换算
# 夜间 = 设计稿原始深色 HMI token;日间 = 同一语义结构的浅色适配。
# 另外输出常用半透明色的预混合值(仅作对照,固件内用 mix() 运行时计算)。

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def hx(s):
    s = s.lstrip('#')
    return int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)

def mix_hex(fg, bg, a):  # a = 0..1 不透明度
    fr, fgc, fb = hx(fg)
    br, bgc, bb = hx(bg)
    r = round(fr * a + br * (1 - a))
    g = round(fgc * a + bgc * (1 - a))
    b = round(fb * a + bb * (1 - a))
    return '#%02X%02X%02X' % (r, g, b), rgb565(r, g, b)

NIGHT = [
    ('bg',      '#0E1216'), ('panel',   '#1A222B'), ('panelAlt', '#222C37'),
    ('border',  '#2A3542'), ('line2',   '#394654'), ('muted',   '#A9B7C6'),
    ('ink3',    '#7E8C9C'), ('text',    '#EAF0F6'), ('active',  '#1C3E48'),
    ('accent',  '#3FD4E4'), ('warn',    '#FFB547'), ('good',    '#43D97E'),
    ('maroon',  '#521A20'), ('gRed',    '#FF5D5D'), ('gYellow', '#FFB547'),
    ('gMagenta','#B48CFF'), ('gCyan',   '#3FD4E4'), ('sky',     '#5CA8FF'),
    ('violet',  '#B48CFF'), ('orange',  '#FF8A3D'), ('lamp',    '#FFD147'),
]

DAY = [
    ('bg',      '#EAF0F6'), ('panel',   '#FFFFFF'), ('panelAlt', '#F0F4F7'),
    ('border',  '#DFE6EC'), ('line2',   '#C9D6E0'), ('muted',   '#44546A'),
    ('ink3',    '#647484'), ('text',    '#182430'), ('active',  '#D2ECF0'),
    ('accent',  '#0FA8BC'), ('warn',    '#D08315'), ('good',    '#1FA95C'),
    ('maroon',  '#8C2F39'), ('gRed',    '#D64545'), ('gYellow', '#D08315'),
    ('gMagenta','#7C4DC4'), ('gCyan',   '#0FA8BC'), ('sky',     '#2F7FD4'),
    ('violet',  '#7C4DC4'), ('orange',  '#E86A1C'), ('lamp',    '#C9A20E'),
]

def dump(name, rows):
    vals = [rgb565(*hx(v[1])) for v in rows]
    print(f'--- {name} ---')
    for (k, v), n in zip(rows, vals):
        print(f'  {k:9s} {v}  0x{n:04X}')
    print('C literal:')
    print('  {' + ', '.join(f'0x{n:04X}' for n in vals) + '}')
    print()

dump('NIGHT', NIGHT)
dump('DAY', DAY)

print('--- 半透明预混合(夜间, 底 #1A222B) ---')
for label, fg, a in [
    ('chip cyan 10%', '#3FD4E4', .10), ('chip cyan 38%', '#3FD4E4', .38),
    ('iconbox cyan 12%', '#3FD4E4', .12), ('iconbox amber 12%', '#FFB547', .12),
    ('pill green 10%', '#43D97E', .10), ('pill green 35%', '#43D97E', .35),
    ('pill amber 10%', '#FFB547', .10), ('pill orange 10%', '#FF8A3D', .10),
    ('pill lamp 10%', '#FFD147', .10), ('pill cyan 10%', '#3FD4E4', .10),
    ('pill red 10%', '#FF5D5D', .10),
    ('bar bg ~13% text', '#EAF0F6', .13),
    ('tile bar bg #232D38', '#232D38', 1.0),
]:
    h, n = mix_hex(fg, '#1A222B', a)
    print(f'  {label:22s} {h}  0x{n:04X}')

print('--- 半透明预混合(日间, 底 #FFFFFF) ---')
for label, fg, a in [
    ('chip cyan 10%', '#0FA8BC', .10), ('chip cyan 38%', '#0FA8BC', .38),
    ('pill green 10%', '#1FA95C', .10),
]:
    h, n = mix_hex(fg, '#FFFFFF', a)
    print(f'  {label:22s} {h}  0x{n:04X}')
