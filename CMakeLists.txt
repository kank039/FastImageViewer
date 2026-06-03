cmake_minimum_required(VERSION 3.20)
project(FastImageViewer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

add_executable(FastImageViewer src/main.cpp)

target_link_libraries(FastImageViewer PRIVATE
    glfw
    imgui::imgui
    glad::glad
    Shell32
    windowscodecs
)