idf_component_register(
    SRCS "luminary_animation_viewer.c" "ocean_sim.c"
    INCLUDE_DIRS "." "../assets/runtime"
)


# LUMV is an indexed stream of baseline JPEG frames. It remains valid in flash
# for the life of the application; each frame is copied into DMA-safe memory
# before the P4 JPEG engine consumes it.
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_base.jpg" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_water_mask.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_shore_distance.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_ocean_phase.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_ocean_depth.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_ocean_map.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_cloud_low.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_cloud_mid.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "../assets/runtime/nubble_runtime_cloud_high.bin" BINARY)
