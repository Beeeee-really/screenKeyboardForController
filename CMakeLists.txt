cmake_minimum_required(VERSION 3.16)
project(MyProject)

set(CMAKE_CXX_STANDARD 17)
set(SFML_DIR "E:/SFML-2.5.1/lib/cmake/SFML")

find_package(SFML REQUIRED COMPONENTS audio network graphics window system)

if (NOT SFML_FOUND)
    message(FATAL_ERROR "SFML NOT FOUND!")
else()
    message("SFML FOUND!")
endif()

include_directories(${SFML_INCLUDE_DIR})
add_executable(MyProject main.cpp)

# 链接SFML库和XInput库
target_link_libraries(MyProject
        sfml-audio
        sfml-network
        sfml-graphics
        sfml-window
        sfml-system
        XInput9_1_0
)

# 在Windows上添加其他系统库
if (WIN32)
    target_link_libraries(MyProject
            XInput9_1_0
            user32
            sfml-graphics
            sfml-window
            sfml-system
            sfml-audio
            sfml-network
    )
endif()