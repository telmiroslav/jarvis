idf_component_register(
    SRCS "main.cc"
         "audio_in.cc"
         "audio_out.cc"
         "cmd_parse.cc"
         "console.cc"
         "display.cc"
         "dsp.cc"
         "gui.cc"
         "net.cc"
         "net_logic.cc"
         "settings.cc"
         "text_util.cc"
         "ui.cc"
         "ui_logic.cc"
         "fonts/jv_font_text_18.c"
         "fonts/jv_font_ui_14.c"
    INCLUDE_DIRS "."
)

# Шрифты сгенерированы lv_font_conv и подключают lvgl.h коротким путём
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
