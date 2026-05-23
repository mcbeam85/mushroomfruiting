#!/usr/bin/env python3
"""Erzeugt Mockup-Screenshots der drei Touchscreen-Bildschirme."""

from PIL import Image, ImageDraw, ImageFont
import os

W, H = 480, 320
SCALE = 2
SW, SH = W * SCALE, H * SCALE

# Farben (RGB)
CLR_BG       = (0, 0, 0)
CLR_TEXT     = (255, 255, 255)
CLR_HEADER   = (0, 0, 60)
CLR_GOOD     = (0, 255, 0)
CLR_WARN     = (255, 255, 0)
CLR_BAD      = (255, 0, 0)
CLR_INACTIVE = (66, 66, 66)
CLR_ACTIVE   = (0, 255, 255)
CLR_BTN      = (41, 69, 41)
CLR_BTN_HL   = (74, 105, 74)
CLR_SETPOINT = (189, 189, 247)
CLR_SEPARATOR = (50, 50, 50)

def s(v):
    return v * SCALE

def load_font(size):
    try:
        return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", s(size))
    except:
        try:
            return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", s(size))
        except:
            return ImageFont.load_default()

font_sm = load_font(9)
font_md = load_font(12)
font_lg = load_font(18)
font_xl = load_font(14)

def rounded_rect(draw, xy, fill, radius=6):
    x0, y0, x1, y1 = xy
    r = s(radius)
    draw.rounded_rectangle([x0, y0, x1, y1], radius=r, fill=fill)

def draw_button(draw, x, y, w, h, label, bg=CLR_BTN, fg=CLR_TEXT, font=font_md):
    rounded_rect(draw, (s(x), s(y), s(x+w), s(y+h)), bg, 6)
    bbox = draw.textbbox((0, 0), label, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    tx = s(x) + (s(w) - tw) // 2
    ty = s(y) + (s(h) - th) // 2
    draw.text((tx, ty), label, fill=fg, font=font)

def draw_output_indicator(draw, x, y, w, label, on):
    bg = CLR_ACTIVE if on else CLR_INACTIVE
    fg = CLR_BG if on else CLR_TEXT
    rounded_rect(draw, (s(x), s(y), s(x+w), s(y+30)), bg, 4)
    bbox1 = draw.textbbox((0, 0), label, font=font_sm)
    tw1 = bbox1[2] - bbox1[0]
    draw.text((s(x) + (s(w) - tw1) // 2, s(y+3)), label, fill=fg, font=font_sm)
    status = "EIN" if on else "AUS"
    bbox2 = draw.textbbox((0, 0), status, font=font_sm)
    tw2 = bbox2[2] - bbox2[0]
    draw.text((s(x) + (s(w) - tw2) // 2, s(y+16)), status, fill=fg, font=font_sm)


def draw_main_screen():
    img = Image.new("RGB", (SW, SH), CLR_BG)
    draw = ImageDraw.Draw(img)

    # Header
    draw.rectangle([0, 0, SW, s(32)], fill=CLR_HEADER)
    draw.text((s(10), s(8)), "PILZZUCHT-STEUERUNG", fill=CLR_TEXT, font=font_md)

    # WiFi/MQTT status
    draw.text((s(380), s(10)), "WiFi", fill=CLR_GOOD, font=font_sm)
    draw.text((s(430), s(10)), "MQTT", fill=CLR_GOOD, font=font_sm)

    # Column headers
    draw.text((s(170), s(40)), "Messwert", fill=CLR_SETPOINT, font=font_sm)
    draw.text((s(340), s(40)), "Sollwert", fill=CLR_SETPOINT, font=font_sm)

    # Sensor rows
    rows = [
        ("TEMPERATUR", "20.5 C",  CLR_GOOD, "20.0 C",  58),
        ("FEUCHTE",    "88.2 %",  CLR_WARN, "90.0 %",  118),
        ("CO2",        "750 ppm", CLR_GOOD, "800 ppm", 178),
    ]

    for label, value, color, setpoint, yy in rows:
        draw.text((s(10), s(yy + 8)), label, fill=CLR_TEXT, font=font_md)
        draw.text((s(170), s(yy + 4)), value, fill=color, font=font_xl)
        draw.text((s(340), s(yy + 8)), setpoint, fill=CLR_SETPOINT, font=font_md)
        draw.line([(s(10), s(yy + 38)), (s(470), s(yy + 38))], fill=CLR_SEPARATOR, width=SCALE)

    # Output indicators
    out_y = 232
    out_w = 78
    gap = 8
    start_x = 14
    outputs = [
        ("Heizung", False),
        ("Kuehl.", False),
        ("FAE", True),
        ("Befeuch.", True),
        ("Luefter", True),
    ]
    for i, (lbl, on) in enumerate(outputs):
        draw_output_indicator(draw, start_x + i * (out_w + gap), out_y, out_w, lbl, on)

    # Settings button
    draw_button(draw, W // 2 - 80, H - 42, 160, 34, "EINSTELLUNGEN", CLR_BTN, CLR_TEXT, font_md)

    return img


def draw_settings_screen():
    img = Image.new("RGB", (SW, SH), CLR_BG)
    draw = ImageDraw.Draw(img)

    # Header
    draw.rectangle([0, 0, SW, s(32)], fill=CLR_HEADER)
    draw.text((s(10), s(8)), "EINSTELLUNGEN", fill=CLR_TEXT, font=font_md)
    draw_button(draw, W - 90, 4, 80, 24, "ZURUECK", CLR_BTN, CLR_TEXT, font_sm)

    y0 = 50
    rowH = 52

    settings = [
        ("Temperatur:", "20.0 C", "0.5"),
        ("Feuchte:",    "90.0 %", "3.0"),
        ("CO2:",        "800 ppm", "100"),
    ]

    for i, (label, value, hyst) in enumerate(settings):
        yy = y0 + i * rowH
        draw.text((s(10), s(yy + 6)), label, fill=CLR_TEXT, font=font_md)

        # Setpoint -/+
        draw_button(draw, 155, yy, 30, 28, "-", CLR_BTN, CLR_TEXT, font_md)
        bbox = draw.textbbox((0, 0), value, font=font_md)
        tw = bbox[2] - bbox[0]
        draw.text((s(190) + (s(80) - tw) // 2, s(yy + 6)), value, fill=CLR_TEXT, font=font_md)
        draw_button(draw, 275, yy, 30, 28, "+", CLR_BTN, CLR_TEXT, font_md)

        # Hysteresis
        draw.text((s(320), s(yy + 2)), "Hyst:", fill=CLR_SETPOINT, font=font_sm)
        draw_button(draw, 320, yy + 16, 24, 22, "-", CLR_BTN, CLR_TEXT, font_sm)
        bbox2 = draw.textbbox((0, 0), hyst, font=font_sm)
        tw2 = bbox2[2] - bbox2[0]
        draw.text((s(348) + (s(50) - tw2) // 2, s(yy + 19)), hyst, fill=CLR_SETPOINT, font=font_sm)
        draw_button(draw, 402, yy + 16, 24, 22, "+", CLR_BTN, CLR_TEXT, font_sm)

        # Separator
        draw.line([(s(10), s(yy + rowH - 4)), (s(470), s(yy + rowH - 4))], fill=CLR_SEPARATOR, width=SCALE)

    # Fan settings button
    draw_button(draw, W // 2 - 90, H - 42, 180, 34, "LUEFTER-EINST.", CLR_BTN, CLR_TEXT, font_md)

    return img


def draw_fan_screen():
    img = Image.new("RGB", (SW, SH), CLR_BG)
    draw = ImageDraw.Draw(img)

    # Header
    draw.rectangle([0, 0, SW, s(32)], fill=CLR_HEADER)
    draw.text((s(10), s(8)), "LUEFTER-EINSTELLUNGEN", fill=CLR_TEXT, font=font_md)
    draw_button(draw, W - 90, 4, 80, 24, "ZURUECK", CLR_BTN, CLR_TEXT, font_sm)

    y0 = 55

    # Interval ON
    draw.text((s(10), s(y0 + 6)), "Intervall EIN:", fill=CLR_TEXT, font=font_md)
    draw_button(draw, 190, y0, 30, 28, "-", CLR_BTN, CLR_TEXT, font_md)
    val1 = "5:00 min"
    bbox1 = draw.textbbox((0, 0), val1, font=font_md)
    tw1 = bbox1[2] - bbox1[0]
    draw.text((s(225) + (s(100) - tw1) // 2, s(y0 + 6)), val1, fill=CLR_TEXT, font=font_md)
    draw_button(draw, 330, y0, 30, 28, "+", CLR_BTN, CLR_TEXT, font_md)

    draw.line([(s(10), s(y0 + 40)), (s(470), s(y0 + 40))], fill=CLR_SEPARATOR, width=SCALE)

    # Interval OFF
    y1 = y0 + 50
    draw.text((s(10), s(y1 + 6)), "Intervall AUS:", fill=CLR_TEXT, font=font_md)
    draw_button(draw, 190, y1, 30, 28, "-", CLR_BTN, CLR_TEXT, font_md)
    val2 = "30:00 min"
    bbox2 = draw.textbbox((0, 0), val2, font=font_md)
    tw2 = bbox2[2] - bbox2[0]
    draw.text((s(225) + (s(100) - tw2) // 2, s(y1 + 6)), val2, fill=CLR_TEXT, font=font_md)
    draw_button(draw, 330, y1, 30, 28, "+", CLR_BTN, CLR_TEXT, font_md)

    draw.line([(s(10), s(y1 + 40)), (s(470), s(y1 + 40))], fill=CLR_SEPARATOR, width=SCALE)

    # Interval mode toggle
    y2 = y1 + 55
    draw.text((s(10), s(y2 + 6)), "Intervall-Modus:", fill=CLR_TEXT, font=font_md)
    draw_button(draw, 210, y2, 80, 28, "EIN", CLR_ACTIVE, CLR_BG, font_md)

    # Control-triggered toggle
    y3 = y2 + 45
    draw.text((s(10), s(y3 + 6)), "Bei Regelung:", fill=CLR_TEXT, font=font_md)
    draw_button(draw, 210, y3, 80, 28, "EIN", CLR_ACTIVE, CLR_BG, font_md)

    return img


if __name__ == "__main__":
    out_dir = "/home/user/mushroomfruiting/screenshots"
    os.makedirs(out_dir, exist_ok=True)

    screens = [
        ("01_hauptbildschirm.png", draw_main_screen),
        ("02_einstellungen.png", draw_settings_screen),
        ("03_luefter_einstellungen.png", draw_fan_screen),
    ]

    for name, func in screens:
        img = func()
        path = os.path.join(out_dir, name)
        img.save(path)
        print(f"Gespeichert: {path}")
