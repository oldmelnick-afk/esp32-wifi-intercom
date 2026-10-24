"""Draw the current screenless ESP32 V1.302 wiring for Danya, room 3F-A.

The board is viewed from the solder side with USB on the left. This drawing is
an electrical netlist, not a claim that any particular third board is wired.
"""

from html import escape
from pathlib import Path


OUT = Path(__file__).resolve().parents[1] / "diagrams" / "third-floor-room3-assistant.svg"

TOP = [
    "3V3", "GND", "D15", "D2", "D4", "RX2/D16", "TX2/D17", "D5",
    "D18", "D19", "D21", "RX0", "TX0", "D22", "D23",
]
BOTTOM = [
    "VIN/5V", "GND", "D13", "D12", "D14", "D27", "D26", "D25",
    "D33", "D32", "D35", "D34", "VN", "VP", "EN",
]
PIN_COLORS = {
    "3V3": "#b45b20", "VIN/5V": "#b45b20", "GND": "#415464",
    "D2": "#b45b20", "TX2/D17": "#a0328b", "D18": "#16804d",
    "D19": "#16804d", "D21": "#16804d", "D33": "#16804d",
    "D32": "#16804d", "D14": "#2374a4", "D27": "#2374a4",
    "D34": "#2374a4", "D22": "#2374a4", "D26": "#2374a4",
    "D25": "#2374a4",
}


def text(x, y, value, size=23, weight=400, fill="#17354b", anchor="start"):
    return (f'<text x="{x}" y="{y}" font-size="{size}" font-weight="{weight}" '
            f'fill="{fill}" text-anchor="{anchor}">{escape(str(value))}</text>')


def card(x, y, width, height, title, accent, rows):
    parts = [f'<rect x="{x}" y="{y}" width="{width}" height="{height}" '
             f'rx="18" fill="#ffffff" stroke="{accent}" stroke-width="3"/>',
             f'<rect x="{x}" y="{y}" width="{width}" height="58" rx="18" '
             f'fill="{accent}"/>',
             f'<rect x="{x}" y="{y+36}" width="{width}" height="22" fill="{accent}"/>',
             text(x+24, y+39, title, 28, 700, "#ffffff")]
    yy = y + 98
    for row in rows:
        parts.append(text(x+24, yy, row, 23))
        yy += 47
    return "\n".join(parts)


def main():
    p = ['<svg xmlns="http://www.w3.org/2000/svg" width="1800" height="1830" '
         'viewBox="0 0 1800 1830">',
         '<style>text{font-family:Segoe UI,Arial,sans-serif}</style>',
         '<rect width="1800" height="1830" fill="#f5f9fc"/>',
         text(60, 78, "3-Й ЭТАЖ • комната 3 • 3F-A • ОТДЕЛЬНАЯ КНОПКА ИИ", 43, 700),
         text(60, 121, "ESP32 DevKit V1.302, 30 контактов, без экрана. Вид со стороны пайки: USB СЛЕВА.", 27),
         text(60, 162, "Номера верхнего и нижнего рядов считают слева направо; перед пайкой сверить надписи на своей плате.", 23),
         '<rect x="200" y="205" width="1400" height="515" rx="24" fill="#e9f1f5" stroke="#789bb1" stroke-width="4"/>',
         '<rect x="70" y="437" width="130" height="80" rx="10" fill="#bac7ce" stroke="#526a78" stroke-width="4"/>',
         text(78, 427, "USB ←", 26, 700),
         text(900, 407, "ESP32 V1.302 — ОБРАТНАЯ СТОРОНА", 36, 700, anchor="middle"),
         text(900, 453, "верхний ряд — над платой на рисунке", 23, anchor="middle"),
         text(900, 492, "нижний ряд — под платой на рисунке", 23, anchor="middle"),
         text(900, 535, "Зелёный: кнопки  •  Синий: звук  •  Красный: ИИ  •  Коричневый: питание/LED", 21, anchor="middle"),
         text(206, 191, "ВЕРХНИЙ РЯД", 24, 700),
         text(206, 753, "НИЖНИЙ РЯД", 24, 700)]

    for names, yy, label_y, number_y in ((TOP, 247, 295, 326), (BOTTOM, 680, 640, 610)):
        for i, name in enumerate(names):
            x = 258 + 92*i
            color = PIN_COLORS.get(name, "#768894")
            p.append(f'<circle cx="{x}" cy="{yy}" r="15" fill="#ffffff" '
                     f'stroke="{color}" stroke-width="6"/>')
            p.append(text(x, label_y, name, 18 if len(name) > 5 else 20, 700,
                          color, "middle"))
            p.append(text(x, number_y, f"{i+1:02d}", 17, 400, "#435566", "middle"))

    p.append(card(60, 785, 815, 380, "КНОПКИ (каждая замыкает GPIO на GND)", "#16804d", [
        "ВСЕМ  → нижний 10 D32 → большая кнопка → GND",
        "К1       → верхний 09 D18 → 1-й этаж → GND",
        "К2       → верхний 10 D19 → 2-й этаж → GND",
        "К3       → верхний 11 D21 → 3F-B → GND",
        "К4       → нижний 09 D33 → 4-й этаж → GND",
        "ИИ         → верхний 07 TX2/D17 → кнопка → GND",
    ]))
    p.append(card(925, 785, 815, 380, "ПИТАНИЕ И ИНДИКАТОР", "#b45b20", [
        "DC +5 В → нижний 01 VIN/5V; оттуда → AMP VIN",
        "DC минус → нижний 02 GND; все земли общие",
        "Верхний 01 3V3 → MIC 3V (только 3,3 В)",
        "Верхний 04 D2 → резистор 220–470 Ом → LED +",
        "LED − → общий GND; 330 Ом — удобный номинал",
        "Внешние 5 В и USB компьютера не включать вместе",
    ]))
    p.append(card(60, 1190, 815, 370, "МИКРОФОН ICS-43434", "#2374a4", [
        "SEL → GND                 3V → верхний 01 3V3",
        "LRCL → нижний 06 D27     GND → общий GND",
        "DOUT → нижний 12 D34",
        "BCLK → нижний 05 D14",
        "Акустическое отверстие не заливать клеем",
    ]))
    p.append(card(925, 1190, 815, 370, "УСИЛИТЕЛЬ MAX98357A + ДИНАМИК", "#2374a4", [
        "LRC → нижний 08 D25    BCLK → нижний 07 D26",
        "DIN → верхний 14 D22   VIN → проверенные +5 В",
        "GND → общий GND         GAIN и SD → свободны",
        "SPK+ / SPK− → только два вывода динамика",
        "Ни один вывод динамика НЕ вести на GND",
    ]))
    p.extend([
        '<rect x="60" y="1588" width="1680" height="195" rx="18" fill="#fff8e9" stroke="#e4b45d" stroke-width="3"/>',
        text(86, 1633, "НАСТРОЙКА И ПРОВЕРКА", 28, 700, "#744d13"),
        text(86, 1676, "Прошивка: screenless. По USB: ROOM 3 → ASSIST PIN 17 → перезапуск EN → STATUS.", 24),
        text(86, 1717, "BUTTONS проверяет пять переговорных кнопок; ИИ проверять удержанием при работающем шлюзе помощника.", 22),
        text(86, 1758, "Схема основана на коде 23.10.2026; физическая пайка третьей коробки ещё не проверена.", 22),
        '</svg>'
    ])
    OUT.write_text("\n".join(p), encoding="utf-8")
    print(OUT)


if __name__ == "__main__":
    main()
