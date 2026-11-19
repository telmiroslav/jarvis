idf_component_register(
    SRCS "main.cc"
         "audio_in.cc"
         "audio_out.cc"
         "cmd_parse.cc"
         "console.cc"
         "dialog.cc"
         "display.cc"
         "dsp.cc"
         "face_logic.cc"
         "gui.cc"
         "heap_diag.cc"
         "heap_logic.cc"
         "link.cc"
         "link_logic.cc"
         "mic_stream.cc"
         "mww.cc"
         "mww_core.cc"
         "mww_logic.cc"
         "net.cc"
         "net_logic.cc"
         "pcm_queue.cc"
         "playback.cc"
         "proto.cc"
         "sample_ring.cc"
         "session.cc"
         "settings.cc"
         "subtitle.cc"
         "text_util.cc"
         "ui.cc"
         "ui_logic.cc"
         "wake.cc"
         "wake_logic.cc"
         "wake_perf.cc"
         "fonts/jv_font_text_18.c"
         "fonts/jv_font_ui_14.c"
    INCLUDE_DIRS "."
    # Модель «Джарвис» для microWakeWord и её манифест кладутся в образ как есть (mww.cc: символы _binary_jarvis_ru_*_start/_end)
    EMBED_FILES "models/jarvis_ru.tflite" "models/jarvis_ru.json"
)

# Компонент признаков esphome/esp-micro-speech-features кладёт большие буферы в PSRAM только при USE_ESP32 (иначе обычный malloc,
# а он до 16 КБ берёт из внутренней памяти, которой у платы мало)
idf_component_get_property(mww_features_lib esphome__esp-micro-speech-features COMPONENT_LIB)
if(TARGET ${mww_features_lib})
    target_compile_definitions(${mww_features_lib} PRIVATE USE_ESP32)
endif()

# Предупреждения -Wshadow в заголовках самого TFLite Micro (reference/sub.h) это чужой код: правке не подлежат, в журнал сборки не пускаем
idf_component_get_property(mww_tflm_lib espressif__esp-tflite-micro COMPONENT_LIB)
if(TARGET ${mww_tflm_lib})
    target_compile_options(${mww_tflm_lib} PRIVATE -Wno-shadow)
endif()

# v0.5.1: откуда движок microWakeWord читает веса модели: 0 (по умолчанию) копия в PSRAM, 1 прямо из образа во flash (mww.cc). Какая быстрее на плате,
# показывает команда wake bench; включить: idf.py -DJARVIS_MWW_MODEL_IN_FLASH=1 build
if(NOT DEFINED JARVIS_MWW_MODEL_IN_FLASH)
    set(JARVIS_MWW_MODEL_IN_FLASH 0)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE JARVIS_MWW_MODEL_IN_FLASH=${JARVIS_MWW_MODEL_IN_FLASH})

# Шрифты сгенерированы lv_font_conv и подключают lvgl.h коротким путём
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
