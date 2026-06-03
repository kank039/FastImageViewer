cmake_minimum_required(VERSION 3.20)
project(FastImageViewer CXX)

# Require C++20 standard for modern features
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find the packages
find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)

# Add our source code
add_executable(FastImageViewer src/main.cpp)

# Link the libraries
target_link_libraries(FastImageViewer PRIVATE
    glfw
    imgui::imgui
    glad::glad
    Shell32
)