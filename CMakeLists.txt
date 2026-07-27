cmake_minimum_required(VERSION 3.16)
project(etest_2026 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED)

# =========================================================================
# 可执行文件
# =========================================================================
add_executable(etest_2026
    src/main.cpp
    src/core/config.cpp
    src/core/context.cpp
    src/core/logger.cpp
    src/state/state.cpp
    src/state/start.cpp
    src/state/search.cpp
    src/state/end.cpp
    src/state/error.cpp
    src/vision/camera.cpp
    src/vision/vision.cpp
    src/uart/uart.cpp
    src/uart/protocol.cpp
)

add_executable(etest_protocol
    test/test_protocol.cpp
    src/uart/protocol.cpp
)
target_include_directories(etest_protocol PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(etest_protocol PRIVATE -Wall -Wextra -pedantic)
add_test(NAME protocol_test COMMAND etest_protocol)

target_include_directories(etest_2026 PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_2026 PRIVATE ${OpenCV_LIBS} pthread)
target_compile_options(etest_2026 PRIVATE -Wall -Wextra -pedantic)

# =========================================================================
# 测试用可执行文件 (只用 assert 和 CTest)
# =========================================================================
add_executable(etest_test
    test/test.cpp
    src/core/config.cpp
)

target_include_directories(etest_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(etest_test PRIVATE -Wall -Wextra -pedantic)

enable_testing()

add_test(NAME config_test COMMAND etest_test)

# =========================================================================
# WSL 下 OpenCV 依赖附加库
# =========================================================================
if(WSL)
    target_link_libraries(etest_2026 PRIVATE opencv_core opencv_imgproc opencv_dnn opencv_videoio opencv_highgui)
    target_link_libraries(etest_test PRIVATE opencv_core)
endif()