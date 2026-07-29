cmake_minimum_required(VERSION 3.16)
project(etest_2026 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED)

# exe
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
    src/vision/video_recorder.cpp
    src/vision/vision.cpp
    src/vision/vision_utils.cpp
    src/uart/uart.cpp
    src/uart/protocol.cpp
)

add_executable(etest_protocol
    test/test_protocol.cpp
    src/uart/protocol.cpp
    src/uart/uart.cpp
    src/core/logger.cpp
)
target_include_directories(etest_protocol PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(etest_protocol PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(etest_protocol PRIVATE pthread)
add_test(NAME protocol_test COMMAND etest_protocol)

target_include_directories(etest_2026 PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_2026 PRIVATE ${OpenCV_LIBS} pthread)
target_compile_options(etest_2026 PRIVATE -Wall -Wextra -pedantic)

# 测试用exe
add_executable(etest_test
    test/test.cpp
    src/core/config.cpp
)

target_include_directories(etest_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(etest_test PRIVATE -Wall -Wextra -pedantic)

# 视觉测试
add_executable(etest_vision_test
    test/test_vision.cpp
    src/vision/vision.cpp
    src/vision/vision_utils.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_vision_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_vision_test PRIVATE ${OpenCV_LIBS} pthread)
target_compile_options(etest_vision_test PRIVATE -Wall -Wextra -pedantic)

# HSV 范围测试
add_executable(etest_hsv_test
    test/test_hsv.cpp
    src/vision/vision.cpp
    src/vision/vision_utils.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_hsv_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_hsv_test PRIVATE ${OpenCV_LIBS} pthread)
target_compile_options(etest_hsv_test PRIVATE -Wall -Wextra -pedantic)

enable_testing()

add_test(NAME config_test COMMAND etest_test)
add_test(NAME vision_test COMMAND etest_vision_test)
add_test(NAME hsv_test COMMAND etest_hsv_test)

# 视频回归测试
add_executable(etest_ball_video_test
    test/test_ball_video.cpp
    src/vision/vision.cpp
    src/vision/vision_utils.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_ball_video_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_ball_video_test PRIVATE ${OpenCV_LIBS} pthread)
target_compile_options(etest_ball_video_test PRIVATE -Wall -Wextra -pedantic)

add_test(NAME ball_video_test COMMAND etest_ball_video_test ${CMAKE_SOURCE_DIR}/docs/videos/test5.mp4 ${CMAKE_SOURCE_DIR}/config/vision.toml)

# WSL 下 OpenCV 依赖附加库
if(WSL)
    target_link_libraries(etest_2026 PRIVATE opencv_core opencv_imgproc opencv_dnn opencv_videoio opencv_highgui)
    target_link_libraries(etest_test PRIVATE opencv_core)
endif()