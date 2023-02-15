cmake_minimum_required(VERSION 3.7)

project(ahx2play C)

find_package(SDL2 REQUIRED)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${ahx2play_SOURCE_DIR}/ahx2play/release/other/")

file(GLOB ahx2play_SRC
    "${ahx2play_SOURCE_DIR}/audiodrivers/sdl/*.c"
    "${ahx2play_SOURCE_DIR}/*.c"
    "${ahx2play_SOURCE_DIR}/ahx2play/src/*.c"
)

add_executable(ahx2play ${ahx2play_SRC})

target_include_directories(ahx2play SYSTEM
    PRIVATE ${SDL2_INCLUDE_DIRS})

if("${SDL2_LIBRARIES}" STREQUAL "")
    message(WARNING "SDL2_LIBRARIES wasn't set, manually setting to SDL2::SDL2")
    set(SDL2_LIBRARIES "SDL2::SDL2")
endif()

target_link_libraries(ahx2play
    PRIVATE m pthread ${SDL2_LIBRARIES})

target_compile_definitions(ahx2play
    PRIVATE AUDIODRIVER_SDL)

install(TARGETS ahx2play
    RUNTIME DESTINATION bin)
