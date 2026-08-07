
# CMakeLists.txt for building ImGui as a library

# Minimum required CMake version
cmake_minimum_required(VERSION 3.0)

# Project name
project(imgui)

# Set source files
set(IMGUI_SOURCES
    imgui_widgets.cpp
    imgui_draw.cpp
    imgui_demo.cpp
    imgui.cpp
    imgui_tables.cpp
    imstb_textedit.h
    imstb_truetype.h
    imstb_rectpack.h
    backends/imgui_impl_opengl3.cpp
    backends/imgui_impl_android.cpp
)

# Add the library target
add_library(${PROJECT_NAME} STATIC ${IMGUI_SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PUBLIC imgui)
target_compile_options(${PROJECT_NAME} PRIVATE "-w")

# Optionally, if you want to link OpenGL, uncomment and adjust the following lines
# find_package(OpenGL REQUIRED)
# target_link_libraries(${PROJECT_NAME} PUBLIC OpenGL::GL)

# Optionally, if you need to link additional libraries, add them here
# target_link_libraries(${PROJECT_NAME} PUBLIC YourAdditionalLibraries)

# Installation
# You can uncomment and adjust this section if you want to install ImGui
# set(INSTALL_DESTINATION "path/to/installation")
# install(TARGETS ${PROJECT_NAME}
#         ARCHIVE DESTINATION ${INSTALL_DESTINATION}/lib
#         LIBRARY DESTINATION ${INSTALL_DESTINATION}/lib
#         RUNTIME DESTINATION ${INSTALL_DESTINATION}/bin
# )
# install(DIRECTORY imgui/ DESTINATION ${INSTALL_DESTINATION}/include/imgui)
