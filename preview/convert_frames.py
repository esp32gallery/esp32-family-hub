from pathlib import Path
from PIL import Image, ImageDraw
root=Path(__file__).parent
for p in (root/'frames').glob('*.ppm'):
    Image.open(p).save(p.with_suffix('.png')); p.unlink()
film=Image.new('RGB',(1440,792),'#f5efdf'); d=ImageDraw.Draw(film)
for n,(to,i) in enumerate((to,i) for to in (3,5,6) for i in (0,2,4,6,9,12)):
    x=n%6*240; y=n//6*264
    film.paste(Image.open(root/f'frames/1-{to}-{i:02}.png').resize((240,240)),(x,y))
    d.text((x+5,y+243),f'1 -> {to} / {i}/12',fill='#222')
film.save(root/'throw-filmstrip.png')
