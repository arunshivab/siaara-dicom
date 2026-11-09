# Consumer project for the installed package: configure with
#   cmake -S tests/package -B build-consumer -DCMAKE_PREFIX_PATH=<install prefix>
# It builds one program against the shared and one against the static library; both print the
# toolkit version (run by the release workflow and docs/RELEASING.md).
cmake_minimum_required(VERSION 3.20)
project(siaara_consumer LANGUAGES C CXX)
find_package(siaara 0.1 REQUIRED CONFIG)
add_executable(consumer_shared main.c)
target_link_libraries(consumer_shared PRIVATE siaara::siaara)
add_executable(consumer_static main.c)
target_link_libraries(consumer_static PRIVATE siaara::siaara_static)
