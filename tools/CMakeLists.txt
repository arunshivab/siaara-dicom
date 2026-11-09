add_executable(siaara-dump siaara_dump.cpp)
target_link_libraries(siaara-dump PRIVATE siaara_static)
target_compile_options(siaara-dump PRIVATE ${SIAARA_WARN})
