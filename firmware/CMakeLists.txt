cmake_minimum_required(VERSION 3.16)

# Компоненты esp-tflite-micro и esp-nn (microWakeWord) пишут cmake_minimum_required(VERSION 3.5), а новый CMake (4.x) на это ругается
# («Compatibility with CMake < 3.10 will be removed»). Чужие файлы мы не правим: поднимаем для них нижнюю границу политик до 3.10
# (CMake до 4.0 эту переменную не знает и молча пропускает).
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jarvis)
