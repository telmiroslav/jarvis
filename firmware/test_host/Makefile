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
              $(MAIN)/proto.cc $(MAIN)/link_logic.cc $(MAIN)/pcm_queue.cc $(MAIN)/subtitle.cc $(MAIN)/dialog.cc $(MAIN)/face_logic.cc $(MAIN)/wake_logic.cc $(MAIN)/wake_perf.cc $(MAIN)/heap_logic.cc

all: $(OUT)/test_main $(OUT)/render

$(OUT)/test_main: $(LOGIC_SRCS) $(wildcard $(MAIN)/*.h)
	@mkdir -p $(OUT)
	g++ -std=c++17 -O1 -g $(WARN) -fsanitize=address,undefined -I$(MAIN) $(LOGIC_SRCS) -o $@ -lm

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
