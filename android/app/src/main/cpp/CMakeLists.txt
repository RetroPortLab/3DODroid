cmake_minimum_required(VERSION 3.18)
project(threedodroid_native C)

# Emulator core and libchdr live at the repository root.
set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../..)
set(CHDR ${ROOT}/third_party/libchdr)
file(GLOB CORE_SRCS CONFIGURE_DEPENDS ${ROOT}/core/*.c)

add_library(chdr STATIC ${CHDR}/unity.c ${CHDR}/src/libchdr_codec_avhuff.c)
target_include_directories(chdr PRIVATE ${CHDR}/include ${CHDR}/deps/lzma-26.02/include
    ${CHDR}/deps/miniz-3.1.2 ${CHDR}/deps/zstd-1.5.7)
target_compile_definitions(chdr PRIVATE Z7_ST)
target_compile_options(chdr PRIVATE -w -O2)

add_library(threedodroid SHARED jni_bridge.c ${CORE_SRCS})
target_include_directories(threedodroid PRIVATE ${ROOT}/core ${CHDR}/include)
target_compile_options(threedodroid PRIVATE -O3 -Wno-unused)
target_link_libraries(threedodroid PRIVATE chdr android log aaudio m)
