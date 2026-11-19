# Сборка тестов на обычном компьютере (вызывается из run.sh, руками обычно не нужна).
#   make -j test_main     тесты чистой логики (быстро, без LVGL)
#   make -j render        рисует интерфейс через LVGL в картинки (LVGL собирается один раз и кэшируется)
OUT  ?= $(or $(TMPDIR),/tmp)/jarvis_test_host
LVGL ?= $(abspath ../managed_components/lvgl__lvgl)
MAIN := $(abspath ../main)
HERE := $(abspath .)

LVGL_SRCS := $(shell find $(LVGL)/src -name '*.c' 2>/dev/null)
LVGL_OBJS := $(patsubst $(LVGL)/%.c,$(OUT)/lvgl/%.o,$(LVGL_SRCS))
LVGL_CFLAGS := -O2 -std=gnu11 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I$(HERE) -I$(LVGL) -w

WARN := -Wall -Wextra -Wpedantic
LOGIC_SRCS := test_main.cc $(MAIN)/dsp.cc $(MAIN)/text_util.cc $(MAIN)/ui_logic.cc $(MAIN)/cmd_parse.cc $(MAIN)/net_logic.cc \
              $(MAIN)/proto.cc $(MAIN)/link_logic.cc $(MAIN)/pcm_queue.cc $(MAIN)/subtitle.cc $(MAIN)/dialog.cc $(MAIN)/face_logic.cc $(MAIN)/wake_logic.cc $(MAIN)/wake_perf.cc $(MAIN)/heap_logic.cc $(MAIN)/mww_logic.cc $(MAIN)/sample_ring.cc \
              $(MAIN)/music_logic.cc $(MAIN)/aec_logic.cc

# ---- Движок microWakeWord (mww_core.cc): микрофронтенд (C) и TFLite Micro (C++, эталонные ядра) из управляемых компонентов,
# они появляются после первой сборки прошивки idf.py build. Нет их, тест движка пропускается (run.sh пишет об этом).
FE_DIR := $(abspath ../managed_components/esphome__esp-micro-speech-features)
TFLM_DIR := $(abspath ../managed_components/espressif__esp-tflite-micro)
HAVE_MWW := $(if $(and $(wildcard $(FE_DIR)/src/frontend.c),$(wildcard $(TFLM_DIR)/tensorflow/lite/micro/micro_interpreter.cc)),1,)

FE_SRCS := $(wildcard $(FE_DIR)/src/*.c)
FE_OBJS := $(patsubst $(FE_DIR)/src/%.c,$(OUT)/fe/%.o,$(FE_SRCS))

TF := $(TFLM_DIR)/tensorflow/lite
TFMICRO := $(TF)/micro
# Только то, что нужно модели (13 операций) и самому интерпретатору; ядра esp-nn на компьютере не нужны (там эталонные)
TFLM_KERNEL_NAMES := call_once var_handle reshape reshape_common read_variable concatenation strided_slice strided_slice_common assign_variable \
                     conv conv_common depthwise_conv depthwise_conv_common split_v fully_connected fully_connected_common logistic \
                     logistic_common quantize quantize_common kernel_util
TFLM_SRCS := $(addprefix $(TFMICRO)/,debug_log.cc flatbuffer_utils.cc memory_helpers.cc micro_allocation_info.cc micro_allocator.cc \
               micro_context.cc micro_interpreter_context.cc micro_interpreter_graph.cc micro_interpreter.cc micro_log.cc micro_op_resolver.cc \
               micro_profiler.cc micro_resource_variable.cc micro_time.cc micro_utils.cc recording_micro_allocator.cc system_setup.cc) \
             $(wildcard $(TFMICRO)/tflite_bridge/*.cc) $(wildcard $(TFMICRO)/memory_planner/*.cc) $(wildcard $(TFMICRO)/arena_allocator/*.cc) \
             $(addprefix $(TFMICRO)/kernels/,$(addsuffix .cc,$(TFLM_KERNEL_NAMES))) \
             $(TF)/kernels/kernel_util.cc $(TF)/core/c/common.cc $(TF)/core/api/flatbuffer_conversions.cc $(TF)/core/api/tensor_utils.cc \
             $(TF)/kernels/internal/common.cc $(TF)/kernels/internal/quantization_util.cc $(TF)/kernels/internal/portable_tensor_utils.cc \
             $(TF)/kernels/internal/tensor_utils.cc $(TF)/kernels/internal/tensor_ctypes.cc $(TF)/kernels/internal/reference/portable_tensor_utils.cc \
             $(TFLM_DIR)/tensorflow/compiler/mlir/lite/core/api/error_reporter.cc $(TFLM_DIR)/tensorflow/compiler/mlir/lite/schema/schema_utils.cc
TFLM_OBJS := $(patsubst $(TFLM_DIR)/%.cc,$(OUT)/tflm/%.o,$(TFLM_SRCS))
TFLM_INC := -I$(TFLM_DIR) -I$(TFLM_DIR)/third_party/gemmlowp -I$(TFLM_DIR)/third_party/flatbuffers/include -I$(TFLM_DIR)/third_party/ruy \
            -I$(TFLM_DIR)/third_party/kissfft

all: $(OUT)/test_main $(OUT)/render $(if $(HAVE_MWW),$(OUT)/test_mww_core $(OUT)/mww_check)

$(OUT)/test_main: $(LOGIC_SRCS) test_v060.inc $(wildcard $(MAIN)/*.h)
	@mkdir -p $(OUT)
	g++ -std=c++17 -O1 -g $(WARN) -fsanitize=address,undefined -I$(MAIN) $(LOGIC_SRCS) -o $@ -lm

ifneq ($(HAVE_MWW),)
# Чужие исходники собираются без предупреждений (-w): правке они не подлежат
$(OUT)/fe/%.o: $(FE_DIR)/src/%.c
	@mkdir -p $(dir $@)
	gcc -O2 -std=gnu11 -w -I$(FE_DIR)/include -I$(FE_DIR)/src -c $< -o $@

$(OUT)/libmww_fe.a: $(FE_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(OUT)/tflm/%.o: $(TFLM_DIR)/%.cc
	@mkdir -p $(dir $@)
	g++ -O2 -std=gnu++17 -fno-rtti -fno-exceptions -DTF_LITE_STATIC_MEMORY -DTF_LITE_DISABLE_X86_NEON -w $(TFLM_INC) -c $< -o $@

$(OUT)/libmww_tflm.a: $(TFLM_OBJS)
	@rm -f $@
	ar rcs $@ $^

# Свой код (mww_core.cc, mww_logic.cc) собирается строго и с проверкой памяти; чужие библиотеки готовые
$(OUT)/test_mww_core: test_mww_core.cc $(MAIN)/mww_core.cc $(MAIN)/mww_core.h $(MAIN)/mww_logic.cc $(MAIN)/mww_logic.h $(MAIN)/wake_perf.cc $(MAIN)/text_util.cc \
                      $(OUT)/libmww_tflm.a $(OUT)/libmww_fe.a
	@mkdir -p $(OUT)
	g++ -std=c++17 -O1 -g $(WARN) -fno-rtti -fno-exceptions -DTF_LITE_STATIC_MEMORY -DMODEL_DIR=\"$(MAIN)/models\" -fsanitize=address,undefined \
	    -I$(MAIN) -I$(FE_DIR)/include -isystem $(TFLM_DIR) -isystem $(TFLM_DIR)/third_party/gemmlowp -isystem $(TFLM_DIR)/third_party/flatbuffers/include \
	    -isystem $(TFLM_DIR)/third_party/ruy -isystem $(TFLM_DIR)/third_party/kissfft \
	    test_mww_core.cc $(MAIN)/mww_core.cc $(MAIN)/mww_logic.cc $(MAIN)/wake_perf.cc $(MAIN)/text_util.cc $(OUT)/libmww_tflm.a $(OUT)/libmww_fe.a -o $@ -lm

# Проверка модели на записях (инструмент, не тест): см. mww_check.cc
$(OUT)/mww_check: mww_check.cc $(MAIN)/mww_core.cc $(MAIN)/mww_core.h $(MAIN)/mww_logic.cc $(MAIN)/mww_logic.h $(MAIN)/wake_perf.cc $(MAIN)/text_util.cc \
                  $(OUT)/libmww_tflm.a $(OUT)/libmww_fe.a
	@mkdir -p $(OUT)
	g++ -std=c++17 -O2 $(WARN) -fno-rtti -fno-exceptions -DTF_LITE_STATIC_MEMORY -I$(MAIN) -I$(FE_DIR)/include -isystem $(TFLM_DIR) \
	    -isystem $(TFLM_DIR)/third_party/gemmlowp -isystem $(TFLM_DIR)/third_party/flatbuffers/include -isystem $(TFLM_DIR)/third_party/ruy \
	    -isystem $(TFLM_DIR)/third_party/kissfft \
	    mww_check.cc $(MAIN)/mww_core.cc $(MAIN)/mww_logic.cc $(MAIN)/wake_perf.cc $(MAIN)/text_util.cc $(OUT)/libmww_tflm.a $(OUT)/libmww_fe.a -o $@ -lm
endif

$(OUT)/liblvgl.a: $(LVGL_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(OUT)/lvgl/%.o: $(LVGL)/%.c $(HERE)/lv_conf.h
	@mkdir -p $(dir $@)
	gcc $(LVGL_CFLAGS) -c $< -o $@

# Шрифты это Си-файлы: собираем отдельно от C++
$(OUT)/font_%.o: $(MAIN)/fonts/%.c
	@mkdir -p $(OUT)
	gcc -O2 -std=gnu11 -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE -I$(HERE) -I$(LVGL) -Wall -Wextra -c $< -o $@

UI_CXX := ui.cc text_util.cc ui_logic.cc net_logic.cc link_logic.cc subtitle.cc
$(OUT)/render: render_main.cc $(addprefix $(MAIN)/,$(UI_CXX)) $(wildcard $(MAIN)/*.h) $(OUT)/liblvgl.a \
               $(OUT)/font_jv_font_text_18.o $(OUT)/font_jv_font_ui_14.o $(HERE)/lv_conf.h
	@mkdir -p $(OUT)
	g++ -std=c++17 -O1 -g $(WARN) -DLV_CONF_INCLUDE_SIMPLE -I$(HERE) -I$(MAIN) -isystem $(LVGL) \
	    render_main.cc $(addprefix $(MAIN)/,$(UI_CXX)) $(OUT)/font_jv_font_text_18.o $(OUT)/font_jv_font_ui_14.o \
	    $(OUT)/liblvgl.a -o $@ -lm -lz

.PHONY: all
