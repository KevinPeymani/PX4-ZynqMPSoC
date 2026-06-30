px4_add_library(arch_version
	board_identity.c
	board_mcu_version.c
)

# arch_version appears after px4_work_queue in the link order, so declaring
# px4_layer as a dep here ensures libpx4_layer.a is scanned after
# libpx4_work_queue.a, resolving the px4_task_spawn_cmd forward reference.
target_link_libraries(arch_version PRIVATE px4_layer)
