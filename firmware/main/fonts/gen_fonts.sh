#!/usr/bin/env bash
# Пересоздаёт шрифты интерфейса (jv_font_text_18.c, jv_font_ui_14.c) из шрифта Inter и двух знаков из шрифтов Noto.
# Нужен только при смене размера или набора символов; готовые .c лежат в репозитории,
# так что для обычной сборки ничего запускать не надо.
#
#   ./gen_fonts.sh            нужны node и npm, интернет (скачивает шрифты и lv_font_conv нужных версий)
#
# Шрифты: Inter 5.3.0 (пакет @fontsource/inter), знак ♪ из Noto Sans Symbols 5.3.0 (@fontsource/noto-sans-symbols, нужен для строки
# музыки, v0.6.0) и знак паузы ⏸ из Noto Sans Symbols 2 5.3.0 (@fontsource/noto-sans-symbols-2). Лицензия у всех SIL Open Font License 1.1, см. OFL.txt.
# Набор символов: ASCII, русский алфавит с Ё и ё, « » № ° — … и ещё несколько типографских знаков, ♪ (U+266A) и ⏸ (U+23F8).
# Он же перечислен в main/text_util.cc (font_has_codepoint): менять оба места вместе.
set -euo pipefail
cd "$(dirname "$0")"

INTER_VERSION=5.3.0
NOTO_VERSION=5.3.0
CONV_VERSION=1.5.3

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fetch() {  # пакет папка: скачивает пакет npm и распаковывает в $work/<папка>/package
  mkdir -p "$work/$2"
  (cd "$work/$2" && npm pack "$1" >/dev/null && tar xzf ./*.tgz)
}
fetch "@fontsource/inter@$INTER_VERSION" inter
fetch "@fontsource/noto-sans-symbols@$NOTO_VERSION" sym
fetch "@fontsource/noto-sans-symbols-2@$NOTO_VERSION" sym2
files="$work/inter/package/files"
sym_files="$work/sym/package/files"
sym2_files="$work/sym2/package/files"
# лицензия: текст у всех трёх пакетов один (SIL OFL 1.1), отличаются строки об авторах
{
  echo "Знаки ♪ (U+266A) и ⏸ (U+23F8) в шрифтах интерфейса взяты из Noto Sans Symbols и Noto Sans Symbols 2:"
  head -n 1 "$work/sym/package/LICENSE"
  echo "Остальные символы это Inter:"
  cat "$work/inter/package/LICENSE"
} > OFL.txt

# латиница, знаки и кириллица лежат в разных файлах Inter, lv_font_conv склеивает их в один шрифт
LATIN='0x20-0x7E,0xA0,0xAB,0xB0,0xB7,0xBB,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x201E,0x2022,0x2026,0x20AC'
CYR='0x401,0x410-0x44F,0x451,0x2116'
NOTE='0x266A'     # ♪
PAUSE='0x23F8'    # ⏸

gen() {  # имя размер вес
  local name="$1" size="$2" weight="$3" out="$1.c"
  npx --yes "lv_font_conv@$CONV_VERSION" --bpp 4 --size "$size" --format lvgl --no-compress \
    --lv-font-name "$name" \
    --font "$files/inter-latin-$weight-normal.woff" -r "$LATIN" \
    --font "$files/inter-cyrillic-$weight-normal.woff" -r "$CYR" \
    --font "$sym_files/noto-sans-symbols-symbols-$weight-normal.woff" -r "$NOTE" \
    --font "$sym2_files/noto-sans-symbols-2-symbols-400-normal.woff" -r "$PAUSE" \
    -o "$out.tmp" >/dev/null
  {
    echo "// Inter (c) 2016 The Inter Project Authors, https://github.com/rsms/inter; знаки ♪ и ⏸ (c) 2022 The Noto Project Authors, https://github.com/notofonts/symbols"
    echo "// Лицензия: SIL Open Font License 1.1 (текст рядом: OFL.txt). Файл сгенерирован gen_fonts.sh, руками не править."
    # без путей временной папки: стабильный diff
    sed -e "s#$files/#@fontsource/inter/files/#g" -e "s#$sym_files/#@fontsource/noto-sans-symbols/files/#g" \
        -e "s#$sym2_files/#@fontsource/noto-sans-symbols-2/files/#g" "$out.tmp"
  } > "$out"
  rm -f "$out.tmp"
  echo "$out: $(wc -c < "$out") байт исходника"
}

gen jv_font_text_18 18 500   # подписи
gen jv_font_ui_14 14 500     # строка состояния и мелкий текст
