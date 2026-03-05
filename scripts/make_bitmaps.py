"""
Generate installer BMPs from logo_rls.png for WiX and NSIS.
Usage: python scripts/make_bitmaps.py
"""
from PIL import Image, ImageDraw

src = "src"
logo = Image.open(src + "/logo_rls.png").convert("RGBA")

# Banner 493x58: white background, RLS logo on right side
banner = Image.new("RGB", (493, 58), (255, 255, 255))
logo_sm = logo.resize((44, 44), Image.LANCZOS)
banner.paste(logo_sm, (441, 7), logo_sm)
d = ImageDraw.Draw(banner)
d.line([(0, 57), (493, 57)], fill=(210, 215, 220), width=1)
d.line([(418, 8), (418, 50)], fill=(210, 215, 220), width=1)
banner.save(src + "/banner.bmp", "BMP")
print("banner.bmp OK")

# Dialog 493x312: dark left panel with centered logo
dialog = Image.new("RGB", (493, 312), (248, 249, 250))
d2 = ImageDraw.Draw(dialog)
d2.rectangle([(0, 0), (163, 312)], fill=(13, 17, 23))
d2.rectangle([(0, 0), (163, 4)], fill=(31, 111, 235))
d2.line([(163, 0), (163, 312)], fill=(31, 111, 235), width=2)
logo_lg = logo.resize((100, 100), Image.LANCZOS)
dialog.paste(logo_lg, (32, 80), logo_lg)
d2.text((30, 192), "RLS Automacao", fill=(255, 255, 255))
d2.text((42, 212), "VPN Client", fill=(139, 148, 158))
d2.text((58, 236), "v1.5.5", fill=(80, 90, 100))
dialog.save(src + "/dialog.bmp", "BMP")
print("dialog.bmp OK")
