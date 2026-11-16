cmake_minimum_required(VERSION 3.24)
project(public_studio LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
file(TO_CMAKE_PATH "${IMGUI_DIR}" IMGUI_DIR)
file(TO_CMAKE_PATH "${STB_DIR}" STB_DIR)
find_package(SDL3 CONFIG REQUIRED)
add_executable(public-studio WIN32 main.cpp process_runner.cpp
 "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
 "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp"
 "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
 "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp")
target_include_directories(public-studio PRIVATE vendor "${IMGUI_DIR}" "${IMGUI_DIR}/backends" "${STB_DIR}")
target_link_libraries(public-studio PRIVATE SDL3::SDL3 opengl32)
if(MINGW)
 target_link_options(public-studio PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set_target_properties(public-studio PROPERTIES OUTPUT_NAME "Workbench Studio")
