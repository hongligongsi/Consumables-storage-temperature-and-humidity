from PIL import Image
import os

im = Image.open(r'C:\Users\hongliwang\Desktop\SCH_1-P1_2026-01-30-X3hS6itA.png')
outdir = r'C:\Users\hongliwang\Desktop\3-1\.workbuddy\sch3x3'
os.makedirs(outdir, exist_ok=True)

# 底边 pin23-26 (IO21/IO47/IO48/IO45) 与其网络标签
c = im.crop((850, 480, 1160, 800))
c = c.resize((c.width * 6, c.height * 6), Image.LANCZOS)
c.save(os.path.join(outdir, 'pins23_26.png'))
print('pins23_26', c.size)

# 主板风扇 Q7 模块（BOARD_FAN 网络）
c2 = im.crop((1750, 2250, 2650, 2800))
c2 = c2.resize((int(c2.width * 2.2), int(c2.height * 2.2)), Image.LANCZOS)
c2.save(os.path.join(outdir, 'board_fan.png'))
print('board_fan', c2.size)

# 模组下方注释全文
c3 = im.crop((270, 760, 920, 900))
c3 = c3.resize((c3.width * 4, c3.height * 4), Image.LANCZOS)
c3.save(os.path.join(outdir, 'note.png'))
print('note', c3.size)
