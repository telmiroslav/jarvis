#!/usr/bin/env bash
# Пересоздаёт шрифты интерфейса (jv_font_text_18.c, jv_font_ui_14.c) из шрифта Inter.
# Нужен только при смене размера или набора символов; готовые .c лежат в репозитории,
# так что для обычной сборки ничего запускать не надо.
#
#   ./gen_fonts.sh            нужны node и npm, интернет (скачивает Inter и lv_font_conv нужных версий)
#
# Шрифт: Inter 5.3.0 (пакет @fontsource/inter), лицензия SIL Open Font License 1.1, см. OFL.txt.
# Набор символов: ASCII, русский алфавит с Ё и ё, « » № ° — … и ещё несколько типографских знаков.
# Он же перечислен в main/text_util.cc (font_has_codepoint): менять оба места вместе.
set -euo pipefail
cd "$(dirname "$0")"

INTER_VERSION=5.3.0
CONV_VERSION=1.5.3

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
(cd "$work" && npm pack "@fontsource/inter@$INTER_VERSION" >/dev/null && tar xzf "fontsource-inter-$INTER_VERSION.tgz")
files="$work/package/files"
cp "$work/package/LICENSE" OFL.txt

# латиница, знаки и кириллица лежат в разных файлах Inter, lv_font_conv склеивает их в один шрифт
LATIN='0x20-0x7E,0xA0,0xAB,0xB0,0xB7,0xBB,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x201E,0x2022,0x2026,0x20AC'
CYR='0x401,0x410-0x44F,0x451,0x2116'

gen() {  # имя размер вес
  local name="$1" size="$2" weight="$3" out="$1.c"
  npx --yes "lv_font_conv@$CONV_VERSION" --bpp 4 --size "$size" --format lvgl --no-compress \
    --lv-font-name "$name" \
    --font "$files/inter-latin-$weight-normal.woff" -r "$LATIN" \
    --font "$files/inter-cyrillic-$weight-normal.woff" -r "$CYR" \
    -o "$out.tmp" >/dev/null
  {
    echo "// Inter (c) 2016 The Inter Project Authors, https://github.com/rsms/inter"
    echo "// Лицензия: SIL Open Font License 1.1 (текст рядом: OFL.txt). Файл сгенерирован gen_fonts.sh, руками не править."
    sed "s#$files/#@fontsource/inter/files/#g" "$out.tmp"   # без путей временной папки: стабильный diff
  } > "$out"
  rm -f "$out.tmp"
  echo "$out: $(wc -c < "$out") байт исходника"
}

gen jv_font_text_18 18 500   # подписи
gen jv_font_ui_14 14 500     # строка состояния и мелкий текст
