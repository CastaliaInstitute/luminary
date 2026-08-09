cmake_minimum_required(VERSION 3.22)
project(luminary_core C)

# The shallow-water solver is the SAME FILE the ESP32-P4 firmware and the
# host harness compile: portable C99, integer maths, bit-identical results
# on every platform. Do not copy it into this tree; one source of truth.
set(OCEAN_SIM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../tools/ocean-sim)

add_library(luminary_core SHARED
    luminary_render_core.c
    luminary_jni.c
    ${OCEAN_SIM_DIR}/ocean_sim.c
)

target_include_directories(luminary_core PRIVATE ${OCEAN_SIM_DIR})
# 1 m ocean grid over the 384x384 m domain -- 4x the P4's cell count. The
# phone has the headroom the microcontroller did not; the solver algorithm
# is unchanged, only its dimensions.
target_compile_definitions(luminary_core PRIVATE OCEAN_NX=384u OCEAN_NY=384u)
target_compile_options(luminary_core PRIVATE -O3 -Wall)

find_library(log-lib log)
find_library(android-lib android)
target_link_libraries(luminary_core ${log-lib} ${android-lib} m)
