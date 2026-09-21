#!/usr/bin/env python3
"""Превращает ui/ui.html в components/ld2450_zones/ui.h (строка для прошивки).

Запускайте после каждой правки ui.html:  python3 gen_ui_h.py
"""
from pathlib import Path

root = Path(__file__).parent
html = (root / "ui" / "ui.html").read_text(encoding="utf-8")

if ')HTML"' in html:
    raise SystemExit('В ui.html не должно быть последовательности )HTML"')

out = (
    "// Сгенерировано gen_ui_h.py из ui/ui.html, не редактируйте вручную.\n"
    "#pragma once\n\n"
    "namespace esphome {\nnamespace ld2450_zones {\n\n"
    'static const char UI_HTML[] = R"HTML(' + html + ')HTML";\n\n'
    "}  // namespace ld2450_zones\n}  // namespace esphome\n"
)
target = root / "components" / "ld2450_zones" / "ui.h"
target.write_text(out, encoding="utf-8")
print(f"OK: {target} ({len(html)} байт HTML)")
