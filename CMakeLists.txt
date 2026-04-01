cmake_minimum_required(VERSION 3.16)
project(unity2godot LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- GLFW ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/glfw)

# --- ufbx (single-file C library) ---
add_library(ufbx STATIC thirdparty/ufbx/ufbx.c)
target_include_directories(ufbx PUBLIC thirdparty/ufbx)

# --- miniz ---
add_subdirectory(thirdparty/miniz)

# --- nativefiledialog-extended ---
set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/nfd-extended)

# --- Dear ImGui (no CMakeLists.txt, build manually) ---
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

# --- Main executable ---
add_executable(unity2godot
    src/main.cpp
    src/gui/app_window.cpp
    src/gui/converter_ui.cpp
    src/converter/converter.cpp
    src/converter/package_extractor.cpp
    src/converter/guid_table.cpp
    src/converter/unity_yaml_parser.cpp
    src/converter/texture_converter.cpp
    src/converter/material_converter.cpp
    src/converter/light_converter.cpp
    src/converter/camera_converter.cpp
    src/converter/scene_converter.cpp
    src/converter/prefab_converter.cpp
    src/converter/project_writer.cpp
)
target_include_directories(unity2godot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(unity2godot PRIVATE imgui ufbx miniz nfd)

# OpenGL
if(WIN32)
    target_link_libraries(unity2godot PRIVATE opengl32)
elseif(APPLE)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    target_link_libraries(unity2godot PRIVATE ${OPENGL_FRAMEWORK})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(unity2godot PRIVATE OpenGL::GL)
endif()
