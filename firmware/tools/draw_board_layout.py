"""Draw the provisional one-unit perfboard layout without external packages.

The purchased perfboard and ESP32 footprints have not yet been measured.
This drawing is a wiring/placement guide, not a drilling template.
"""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


OUT = Path(__file__).resolve().parents[1] / "diagrams" / "perfboard-layout.png"
W, H = 2200, 1590
img = Image.new("RGB", (W, H), "#f8fbfd")
d = ImageDraw.Draw(img)


def font(size: int, bold: bool = False):
    name = "arialbd.ttf" if bold else "arial.ttf"
    path = Path("C:/Windows/Fonts") / name
    return ImageFont.truetype(str(path), size)


F = {
    "title": font(48, True),
    "section": font(32, True),
    "label": font(28, True),
    "body": font(26),
    "small": font(22),
    "tiny": font(19),
}
NAVY = "#193b54"
TEAL = "#077c81"
RED = "#da5d31"
GREY = "#5d6a73"
PURPLE = "#7951a4"
GREEN = "#3b844d"


def text(x, y, value, style="body", fill=NAVY):
    d.text((x, y), value, font=F[style], fill=fill)


def card(box, fill, outline, radius=22, width=3):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def line(points, fill, width=5):
    d.line(points, fill=fill, width=width, joint="curve")


text(60, 35, "Одна переговорная точка: монтаж на перфоплате", "title")
text(62, 100, "Вид сверху. Расстановка примерная; номера выводов и электрические соединения — для этой прошивки.", "body")

# Approximate top-side component placement.
card((55, 175, 1210, 835), "#eef4f7", "#7290a0", 24, 5)
for x in range(85, 1200, 29):
    for y in range(205, 825, 29):
        d.ellipse((x - 2, y - 2, x + 2, y + 2), fill="#c4d4db")
text(85, 190, "ПЕРФОПЛАТА", "section")
text(842, 198, "1 плата на точку", "small", GREY)

# Four target buttons on the edge of the board.
button_data = [
    (135, "К1", "GPIO18"),
    (385, "К2", "GPIO19"),
    (635, "К3", "GPIO21"),
    (885, "К4", "GPIO33"),
]
for x, label, pin in button_data:
    card((x, 255, x + 190, 360), "#f6fff9", GREEN, 14, 3)
    d.rounded_rectangle((x + 69, 273, x + 121, 320), radius=12,
                        fill="#333b3f", outline="#18252d", width=2)
    text(x + 17, 327, f"{label}  {pin}", "tiny")
text(133, 375, "Четыре маленькие кнопки по краю корпуса", "small")

# Modules, deliberately not dimensionally modeled.
card((210, 425, 610, 752), "#e6f2fc", "#2e6695", 19, 4)
text(283, 448, "ESP32 DevKit", "section")
text(235, 505, "5V / VIN    GND    3V3", "small")
text(235, 546, "MIC: 14 / 27 / 34", "small")
text(235, 587, "AMP: 26 / 25 / 22", "small")
text(235, 628, "ВСЕМ: GPIO32", "small")
text(235, 669, "К1–К4: 18 / 19 / 21 / 33", "small")
text(235, 703, "LED: GPIO2 → резистор", "small")

card((670, 450, 935, 602), "#f5edfc", PURPLE, 19, 4)
text(707, 474, "MAX98357A", "label")
text(690, 520, "VIN  GND  BCLK", "small")
text(690, 554, "LRC  DIN  SPK±", "small")

card((975, 545, 1168, 714), "#e9f8ec", GREEN, 19, 4)
text(1002, 569, "ICS-43434", "label")
text(993, 614, "3V   GND", "small")
text(993, 645, "BCLK  LRCL", "tiny")
text(993, 677, "DOUT; SEL → GND", "tiny")

# Panel-mounted items, outside of the perfboard outline.
text(62, 865, "В КОРПУСЕ, РЯДОМ С ПЛАТОЙ:", "section")
card((65, 922, 410, 1020), "#fff7e7", "#c89539", 15, 3)
text(90, 941, "Большая кнопка ВСЕМ", "label")
text(106, 980, "GPIO32 ↔ GND", "small")
card((455, 922, 785, 1020), "#fff0e8", RED, 15, 3)
text(486, 941, "Гнездо DC 5 В", "label")
text(480, 980, "+5 В в центре / GND", "small")
card((830, 922, 1210, 1020), "#f4eefb", PURPLE, 15, 3)
text(860, 941, "Динамик 4 Ом", "label")
text(850, 980, "только SPK+ / SPK−", "small")

# Exact connection list.
card((1250, 175, 2150, 1020), "#ffffff", "#b5c9d3", 24, 3)
text(1290, 199, "ЧТО СОЕДИНИТЬ", "section")
rows = [
    ("ПИТАНИЕ", RED, True),
    ("DC центр +5 В → ESP32 5V/VIN, AMP VIN", NAVY, False),
    ("DC корпус GND → ESP32 GND, AMP GND", NAVY, False),
    ("ESP32 3V3 → MIC 3V", NAVY, False),
    ("MIC GND и MIC SEL → общий GND", NAVY, False),
    ("МИКРОФОН", TEAL, True),
    ("GPIO14 → MIC BCLK", NAVY, False),
    ("GPIO27 → MIC LRCL", NAVY, False),
    ("GPIO34 ← MIC DOUT", NAVY, False),
    ("УСИЛИТЕЛЬ И ДИНАМИК", PURPLE, True),
    ("GPIO26 → AMP BCLK", NAVY, False),
    ("GPIO25 → AMP LRC", NAVY, False),
    ("GPIO22 → AMP DIN", NAVY, False),
    ("AMP SPK+ / SPK− → динамик", NAVY, False),
    ("КНОПКИ", GREEN, True),
    ("GPIO32, 18, 19, 21, 33 → свои кнопки", NAVY, False),
    ("Другой контакт каждой → общий GND", NAVY, False),
    ("GPIO2 → 330 Ом → LED (+); LED (−) → GND", RED, False),
]
y = 250
for value, color, heading in rows:
    text(1292, y, value, "small" if heading else "tiny", color)
    y += 43 if heading else 39

# Enlarged electrical underside of the four-button strip.
card((55, 1060, 2150, 1535), "#ffffff", "#b5c9d3", 24, 3)
text(82, 1084, "Четыре кнопки: вид со стороны пайки, электрическая разводка", "section")
text(83, 1130, "У каждой 4-контактной кнопки берите по одному выводу из двух РАЗНЫХ групп контактов.", "small")

xs = [250, 700, 1150, 1600]
names = ["К1 → GPIO18", "К2 → GPIO19", "К3 → GPIO21", "К4 → GPIO33"]
for x, name in zip(xs, names):
    text(x - 55, 1192, name, "label", TEAL)
    line([(x + 58, 1234), (x + 58, 1275)], TEAL, 6)
    card((x - 6, 1275, x + 122, 1361), "#eef3f6", GREY, 13, 3)
    d.ellipse((x + 44, 1300, x + 72, 1328), fill="#33424b")
    text(x + 34, 1367, "NO", "small", GREY)
    line([(x + 58, 1405), (x + 58, 1440)], GREY, 6)
line([(150, 1440), (1880, 1440)], GREY, 7)
text(1910, 1425, "GND", "label", GREY)
text(83, 1476, "Не соединяйте GPIO с GND напрямую: между ними должна быть кнопка. Проверяйте её выводы прозвонкой.", "small")

OUT.parent.mkdir(parents=True, exist_ok=True)
img.save(OUT, optimize=True)
print(OUT)
