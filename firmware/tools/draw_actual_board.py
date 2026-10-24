"""Make a self-contained pin overlay for Alex's actual ESP32 mounting photo.

This is a code-native SVG: the original JPEG is embedded unmodified and the
pin labels and arrows are deterministic vectors. Coordinates were checked
against the front-side V1.302 silkscreen with USB on the left in this view.
"""

from base64 import b64encode
from html import escape
from pathlib import Path
import argparse


parser = argparse.ArgumentParser()
parser.add_argument("--room", type=int, choices=(1, 2), default=1)
room = parser.parse_args().room
PHOTO = Path("C:/Users/Alex/Downloads/ee7ba4bf-6d46-4f84-b2bf-8a956b70ef9c.jpg")
diagram_name = "actual-board-back-usb-left.svg" if room == 1 else "second-board-back-usb-left.svg"
OUT = Path(__file__).resolve().parents[1] / "diagrams" / diagram_name
W, H = 2000, 2630
PX, PY = 360, 650

targets = (
    ("2-й этаж", "3-й этаж, комната 1", "3-й этаж, комната 2", "4-й этаж")
    if room == 1 else
    ("1-й этаж", "3-й этаж, комната 1", "3-й этаж, комната 2", "4-й этаж")
)
photo_targets = (
    ("2-й этаж", "3-й, ком. 1", "3-й, ком. 2", "4-й этаж")
    if room == 1 else
    ("1-й этаж", "3-й, ком. 1", "3-й, ком. 2", "4-й этаж")
)
unused = "Свободен; без LCD" if room == 2 else None

TOP = [
    ("3V3", "Питание MIC 3V" + ("; LCD VDD" if room == 1 else ""), "#b6481e"),
    ("GND", "Общий минус кнопок, LED, MIC" + (", LCD" if room == 1 else ""), "#344e64"),
    ("D15", "Свободен", "#71808a"),
    ("D2", "Резистор 220–470 Ом → LED +", "#b6481e"),
    ("D4", unused or "LCD D6", "#71808a" if unused else "#7354a4"),
    ("RX2 / D16", unused or "LCD RS", "#71808a" if unused else "#7354a4"),
    ("TX2 / D17", unused or "LCD E", "#71808a" if unused else "#7354a4"),
    ("D5", unused or "LCD D7", "#71808a" if unused else "#7354a4"),
    ("D18", "К1 → " + targets[0], "#15734a"),
    ("D19", "К2 → " + targets[1], "#15734a"),
    ("D21", "К3 → " + targets[2], "#15734a"),
    ("RX0", "Свободен; USB-порт", "#71808a"),
    ("TX0", "Свободен; USB-порт", "#71808a"),
    ("D22", "Усилитель MAX98357A DIN", "#126a9e"),
    ("D23", "Большая кнопка → ВСЕ" if room == 2 else "LCD D4",
     "#15734a" if room == 2 else "#7354a4"),
]

BOTTOM = [
    ("VIN / 5V", "+5 В от гнезда DC; также AMP VIN", "#b6481e"),
    ("GND", "Минус DC; общий GND", "#344e64"),
    ("D13", unused or "LCD D5", "#71808a" if unused else "#7354a4"),
    ("D12", "Свободен; не трогать при загрузке", "#71808a"),
    ("D14", "Микрофон BCLK", "#126a9e"),
    ("D27", "Микрофон LRCL", "#126a9e"),
    ("D26", "Усилитель MAX98357A BCLK", "#126a9e"),
    ("D25", "Усилитель MAX98357A LRC", "#126a9e"),
    ("D33", "К4 → " + targets[3], "#15734a"),
    ("D32", "Свободен; кнопка перенесена на D23" if room == 2 else "Большая кнопка → ВСЕ",
     "#71808a" if room == 2 else "#15734a"),
    ("D35", "Свободен", "#71808a"),
    ("D34", "Микрофон DOUT", "#126a9e"),
    ("VN", "Свободен", "#71808a"),
    ("VP", "Свободен", "#71808a"),
    ("EN", "Свободен; сброс платы", "#71808a"),
]

parts = []


def add(s):
    parts.append(s)


def txt(x, y, s, size=27, weight=400, fill="#193b54", anchor="start"):
    add(f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{escape(s)}</text>')


def rect(x, y, w, h, fill="#fff", stroke="#dce6eb", radius=18, sw=2):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>')


def line(x1, y1, x2, y2, color, sw=3, marker=False):
    ending = ' marker-end="url(#arrow)"' if marker else ""
    add(f'<path d="M{x1} {y1} L{x2} {y2}" stroke="{color}" stroke-width="{sw}" fill="none"{ending}/>')


def pin_table(data, y0):
    for i, (pin, destination, color) in enumerate(data):
        col, row = divmod(i, 5)
        x, y = 64 + col * 620, y0 + row * 77
        rect(x, y - 37, 590, 62, "#fff", "#dce6eb", 10, 1)
        rect(x + 8, y - 26, 42, 39, color, color, 8, 1)
        txt(x + 29, y + 1, f"{i+1:02}", 19, 700, "#fff", "middle")
        txt(x + 62, y - 7, pin, 22, 700, color)
        txt(x + 62, y + 17, destination, 17, 400, "#263944")


def pin_arrows(xs, cy, pin_y, color):
    for i, x in enumerate(xs, 1):
        add(f'<circle cx="{x:.1f}" cy="{cy}" r="15" fill="#fff" stroke="{color}" stroke-width="2.5"/>')
        txt(f"{x:.1f}", cy + 5.5, str(i), 15, 700, color, "middle")
        if cy < pin_y:
            line(f"{x:.1f}", cy + 16, f"{x:.1f}", pin_y - 3, color, 2, True)
        else:
            line(f"{x:.1f}", cy - 16, f"{x:.1f}", pin_y + 3, color, 2, True)


add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
add('<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10 Z" fill="#235a7a"/></marker></defs>')
add('<style>text {font-family: Arial, Helvetica, sans-serif;} .thin {paint-order:stroke;stroke:#fff;stroke-width:5px;stroke-linejoin:round}</style>')
add(f'<rect width="{W}" height="{H}" fill="#f8fbfd"/>')
txt(64, 70, "ПЕРВЫЙ ЭТАЖ • ESP32 V1.302" if room == 1 else "ВТОРОЙ ЭТАЖ • ESP32 V1.302 • БЕЗ ЭКРАНА", 43, 700)
txt(64, 111, "Вид со стороны пайки. USB находится СЛЕВА. Номера 01–15 считают слева направо.", 27)
rect(55, 135, 1890, 455, "#f5fafc", "#a8c4d3", 20, 2)
txt(76, 177, "ВЕРХНИЙ РЯД КОНТАКТОВ — НАД ПЛАТОЙ НА ФОТО", 29, 700, "#235a7a")
pin_table(TOP, 229)

photo_b64 = b64encode(PHOTO.read_bytes()).decode("ascii")
add(f'<image x="{PX}" y="{PY}" width="1280" height="960" href="data:image/jpeg;base64,{photo_b64}"/>')
add(f'<rect x="{PX}" y="{PY}" width="1280" height="960" fill="none" stroke="#6d8797" stroke-width="3"/>')
txt(64, 634, "ИСХОДНОЕ ФОТО БЕЗ ПОВОРОТА И ЗАМЕНЫ ДЕТАЛЕЙ" if room == 1 else "ФОТО ПЕРВОЙ КОРОБКИ — ОРИЕНТИР ДЛЯ ВТОРОЙ", 22, 700, "#385568")

# Identify the actual switches; existing wire colors do not establish polarity.
for bx, by, bw, title, sub in [
    (380, 665, 170, "К1", photo_targets[0]),
    (555, 665, 170, "К2", photo_targets[1]),
    (730, 665, 170, "К3", photo_targets[2]),
    (905, 665, 170, "К4", photo_targets[3]),
    (1235, 665, 245, "БОЛЬШАЯ", "Всем комнатам"),
]:
    rect(bx, by, bw, 61, "#fff", "#15734a", 12, 3)
    txt(bx + bw/2, by + 25, title, 21, 700, "#15734a", "middle")
    txt(bx + bw/2, by + 48, sub, 17, 400, "#214d38", "middle")
for x1, x2 in [(430, 384), (640, 585), (815, 767), (990, 923), (1360, 1200)]:
    line(x1, 726, x2, 764, "#15734a", 3, True)

rect(1650, 875, 330, 145, "#fff", "#b6481e", 14, 3)
txt(1670, 911, "ГНЕЗДО DC: 5 В", 22, 700, "#a43d19")
txt(1670, 946, "Проверить мультиметром", 18, 400)
txt(1670, 977, "центр +, корпус −", 18, 400)
txt(1670, 1008, "до подключения платы", 18, 400)
line(1650, 928, 1510, 935, "#b6481e", 3, True)

rect(15, 1165, 335, 145, "#fff", "#126a9e", 14, 3)
txt(35, 1202, "ДИНАМИК", 23, 700, "#126a9e")
txt(35, 1234, "Два вывода → SPK+", 18, 400)
txt(35, 1262, "и SPK− усилителя.", 18, 400)
txt(35, 1290, "Ни один → GND.", 18, 400)
line(350, 1225, 415, 1185, "#126a9e", 3, True)

# Actual pin centers in the attached 1280×960 photo. Top 466..850, bottom 448..837.
top_x = [PX + 466 + i * (384/14) for i in range(15)]
bottom_x = [PX + 448 + i * (389/14) for i in range(15)]
pin_arrows(top_x, 1027, PY + 430, "#235a7a")
pin_arrows(bottom_x, 1423, PY + 705, "#235a7a")
txt(136, 1086, "↑ ВЕРХ", 23, 700, "#235a7a")
txt(136, 1361, "↓ НИЗ", 23, 700, "#235a7a")

rect(55, 1650, 1890, 455, "#f5fafc", "#a8c4d3", 20, 2)
txt(76, 1692, "НИЖНИЙ РЯД КОНТАКТОВ — ПОД ПЛАТОЙ НА ФОТО", 29, 700, "#235a7a")
pin_table(BOTTOM, 1744)

rect(55, 2130, 925, 430, "#fff", "#b6481e", 17, 2)
txt(80, 2174, "ПИТАНИЕ И КРАСНЫЙ ИНДИКАТОР" if room == 1 else "ПИТАНИЕ И ИНДИКАТОР", 28, 700, "#a43d19")
txt(80, 2220, "DC +5 В → нижний 01 VIN; DC − → нижний 02 GND.", 22)
txt(80, 2260, "Внешние 5 В и USB одновременно не подключать.", 22)
txt(80, 2315, "Верхний 04 D2 → 220–470 Ом → LED анод (+).", 22)
txt(80, 2355, "LED катод (−) → GND. LED сам является диодом.", 22)
txt(80, 2405, "Обычный диод в цепи кнопок и питания не нужен.", 22)
txt(80, 2453, "После обрезки ножек полярность LED проверьте", 21)
txt(80, 2484, "мультиметром: длина уже не показатель.", 21)

rect(1000, 2130, 945, 430, "#fff", "#15734a", 17, 2)
txt(1025, 2174, "КНОПКИ И ОСТАЛЬНЫЕ МОДУЛИ", 28, 700, "#15734a")
txt(1025, 2220, "К1–К4: свой GPIO ↔ кнопка ↔ общий GND.", 21)
txt(1025, 2258, "Большая: D23 ↔ COM/NO ↔ GND." if room == 2 else
    "Большая: D32 ↔ COM/NO ↔ GND.", 21)
txt(1025, 2296, "Контакты кнопок сначала прозвонить.", 21)
if room == 1:
    txt(1025, 2346, "MIC 3V = 3V3; AMP VIN = 5 В.", 21)
    txt(1025, 2384, "MIC/AMP GND и LCD 1, 3, 5, 16 → GND.", 21)
    txt(1025, 2422, "LCD 15 → 5 В через отдельные 220 Ом.", 21)
    txt(1025, 2460, "AMP SPK+ / SPK− → только динамик.", 21)
    txt(1025, 2500, "MIC SEL → GND; провода ещё не прозвонены.", 21)
else:
    txt(1025, 2342, "MIC: 3V→3V3; GND, SEL→GND.", 20)
    txt(1025, 2378, "MIC: BCLK→D14; LRCL→D27; DOUT→D34.", 20)
    txt(1025, 2414, "AMP: VIN→5 В; GND→GND; DIN→D22.", 20)
    txt(1025, 2450, "AMP: BCLK→D26; LRC→D25.", 20)
    txt(1025, 2486, "AMP SPK+ / SPK− → только динамик.", 20)
    txt(1025, 2522, "Контакты LCD не подключать.", 20)
add('</svg>')
OUT.write_text("\n".join(parts), encoding="utf-8")
print(OUT)
