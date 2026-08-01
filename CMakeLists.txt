cmake_minimum_required(VERSION 3.16)
project(etest_2026 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── 默认 Release 编译 ──
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "Release flags" FORCE)

find_package(OpenCV REQUIRED)

# ── NCNN 可选支持 ──
option(ETEST_ENABLE_NCNN "Enable NCNN YOLO backend" ON)

if(ETEST_ENABLE_NCNN)
    # 提示 ncnn 的安装路径（若已装到系统路径则自动忽略）
    list(APPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local/ncnn")
    find_package(ncnn QUIET)
    if(ncnn_FOUND)
        set(ETEST_HAS_NCNN TRUE)
        message(STATUS "NCNN found: ${ncnn_VERSION}")
    else()
        set(ETEST_HAS_NCNN FALSE)
        message(WARNING "NCNN not found; building without NCNN backend")
    endif()
else()
    set(ETEST_HAS_NCNN FALSE)
    message(STATUS "NCNN backend disabled via ETEST_ENABLE_NCNN=OFF")
endif()

# ── etest_vision_core 静态库 ──
add_library(etest_vision_core STATIC
    src/vision/vision.cpp
    src/vision/vision_utils.cpp
    src/vision/ball_ncnn_manager.cpp
    src/vision/yolo_ball.cpp
    src/vision/yolo_detector.cpp
    src/vision/roi_utils.cpp
    src/vision/yolo_backend_opencv.cpp
)

target_include_directories(
    etest_vision_core
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(
    etest_vision_core
    PUBLIC
    ${OpenCV_LIBS}
    pthread
)

target_compile_options(
    etest_vision_core
    PRIVATE
    -Wall
    -Wextra
    -pedantic
)

# ── NCNN 后端条件编译 ──
if(ETEST_HAS_NCNN)
    target_sources(
        etest_vision_core
        PRIVATE
        src/vision/yolo_backend_ncnn.cpp
    )

    target_link_libraries(
        etest_vision_core
        PUBLIC
        ncnn
    )

    target_compile_definitions(
        etest_vision_core
        PUBLIC
        ETEST_HAS_NCNN=1
    )
endif()

# ── 主可执行文件 ──
add_executable(etest_2026
    src/main.cpp
    src/core/config.cpp
    src/core/context.cpp
    src/core/logger.cpp
    src/state/state.cpp
    src/state/start.cpp
    src/state/search.cpp
    src/state/calibration_logic.cpp
    src/state/end.cpp
    src/state/error.cpp
    src/stream/video_streamer.cpp
    src/vision/camera.cpp
    src/vision/latest_frame_capture.cpp
    src/vision/video_recorder.cpp
    src/uart/uart.cpp
    src/uart/protocol.cpp
    src/state/task_session.cpp
)

target_include_directories(etest_2026 PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_2026 PRIVATE etest_vision_core ${OpenCV_LIBS} pthread)
target_compile_options(etest_2026 PRIVATE -Wall -Wextra -pedantic)

# ── UART 模式测试 ──
add_executable(etest_uart_mode
    test/test_uart_mode.cpp
    src/uart/protocol.cpp
    src/uart/uart.cpp
    src/core/logger.cpp
)

target_include_directories(etest_uart_mode PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(etest_uart_mode PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(etest_uart_mode PRIVATE pthread)
add_test(NAME uart_mode_test COMMAND etest_uart_mode)

# ── 任务会话测试 ──
add_executable(etest_task_session
    test/test_task_session.cpp
    src/state/task_session.cpp
    src/core/logger.cpp
)

target_include_directories(etest_task_session PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_task_session PRIVATE ${OpenCV_LIBS} pthread)
target_compile_options(etest_task_session PRIVATE -Wall -Wextra -pedantic)
add_test(NAME task_session_test COMMAND etest_task_session)

# ── 协议测试 ──
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

# ── 配置测试 ──
add_executable(etest_test
    test/test.cpp
    src/core/config.cpp
)

target_include_directories(etest_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(etest_test PRIVATE -Wall -Wextra -pedantic)
add_test(NAME config_test COMMAND etest_test)

# ── 视觉测试 ──
add_executable(etest_vision_test
    test/test_vision.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_vision_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_vision_test PRIVATE etest_vision_core ${OpenCV_LIBS} pthread)
target_compile_options(etest_vision_test PRIVATE -Wall -Wextra -pedantic)
add_test(NAME vision_test COMMAND etest_vision_test)

# ── HSV 测试 ──
add_executable(etest_hsv_test
    test/test_hsv.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_hsv_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_hsv_test PRIVATE etest_vision_core ${OpenCV_LIBS} pthread)
target_compile_options(etest_hsv_test PRIVATE -Wall -Wextra -pedantic)
add_test(NAME hsv_test COMMAND etest_hsv_test)

# ── ROI 几何测试 ──
add_executable(etest_ball_roi
    test/test_ball_roi.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_ball_roi PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_ball_roi PRIVATE etest_vision_core ${OpenCV_LIBS} pthread)
target_compile_options(etest_ball_roi PRIVATE -Wall -Wextra -pedantic)
add_test(NAME ball_roi_test COMMAND etest_ball_roi)

# ── 视频回归测试 ──
add_executable(etest_ball_video_test
    test/test_ball_video.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_ball_video_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_ball_video_test PRIVATE etest_vision_core ${OpenCV_LIBS} pthread)
target_compile_options(etest_ball_video_test PRIVATE -Wall -Wextra -pedantic)
add_test(NAME ball_video_test COMMAND etest_ball_video_test ${CMAKE_SOURCE_DIR}/docs/videos/test5.mp4 ${CMAKE_SOURCE_DIR}/config/vision.toml)

# ── LatestFrameCapture 单元测试 ──
add_executable(etest_latest_frame_capture
    test/test_latest_frame_capture.cpp
    src/vision/latest_frame_capture.cpp
    src/vision/camera.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_latest_frame_capture PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_latest_frame_capture PRIVATE ${OpenCV_LIBS} pthread)
target_compile_options(etest_latest_frame_capture PRIVATE -Wall -Wextra -pedantic)
add_test(NAME latest_frame_capture_test COMMAND etest_latest_frame_capture ${CMAKE_SOURCE_DIR})

# ── YOLO benchmark ──
add_executable(etest_yolo_benchmark
    test/test_yolo_benchmark.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_yolo_benchmark PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_yolo_benchmark PRIVATE etest_vision_core ${OpenCV_LIBS} pthread)
target_compile_options(etest_yolo_benchmark PRIVATE -Wall -Wextra -pedantic)

# ── YOLO 解码测试 ──
add_executable(etest_yolo_decode
    test/test_yolo_decode.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_yolo_decode PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_yolo_decode PRIVATE etest_vision_core ${OpenCV_LIBS} pthread)
target_compile_options(etest_yolo_decode PRIVATE -Wall -Wextra -pedantic)

# ── YOLO 后端一致性测试 ──
add_executable(etest_yolo_backend_parity
    test/test_yolo_backend_parity.cpp
    src/core/config.cpp
    src/core/logger.cpp
)

target_include_directories(etest_yolo_backend_parity PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(etest_yolo_backend_parity PRIVATE etest_vision_core ${OpenCV_LIBS} pthread)
target_compile_options(etest_yolo_backend_parity PRIVATE -Wall -Wextra -pedantic)

# ── NCNN smoke test ──
if(ETEST_HAS_NCNN)
    add_executable(etest_yolo_ncnn_smoke
        test/test_yolo_ncnn_smoke.cpp
        src/core/config.cpp
        src/core/logger.cpp
    )

    target_include_directories(etest_yolo_ncnn_smoke PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(etest_yolo_ncnn_smoke PRIVATE etest_vision_core ${OpenCV_LIBS} pthread ncnn)
    target_compile_options(etest_yolo_ncnn_smoke PRIVATE -Wall -Wextra -pedantic)
    add_test(NAME yolo_ncnn_smoke COMMAND etest_yolo_ncnn_smoke)

    # ── 摄像头图传冒烟测试 ──
    add_executable(
        etest_camera_stream_test
        test/test_camera_stream.cpp
        src/vision/camera.cpp
        src/core/config.cpp
        src/core/logger.cpp
    )

    target_include_directories(
        etest_camera_stream_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(
        etest_camera_stream_test
        PRIVATE
        ${OpenCV_LIBS}
        pthread
    )

    target_compile_options(
        etest_camera_stream_test
        PRIVATE
        -Wall
        -Wextra
        -pedantic
    )

    # ── 标定采样 worker ──
    add_executable(
        etest_calibration_worker
        src/tools/calibration_worker.cpp
        src/core/config.cpp
        src/core/logger.cpp
    )

    target_include_directories(
        etest_calibration_worker
        PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(
        etest_calibration_worker
        PRIVATE
        etest_vision_core
        ${OpenCV_LIBS}
        pthread
        ncnn
    )

    target_compile_options(
        etest_calibration_worker
        PRIVATE
        -Wall
        -Wextra
        -pedantic
    )

    # ── 轴标定测试 ──
    add_executable(
        etest_axis_calibration_test
        test/test_axis_calibration.cpp
        src/core/config.cpp
        src/core/logger.cpp
    )

    target_include_directories(
        etest_axis_calibration_test
        PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(
        etest_axis_calibration_test
        PRIVATE
        etest_vision_core
        ${OpenCV_LIBS}
        pthread
    )

    target_compile_options(
        etest_axis_calibration_test
        PRIVATE
        -Wall
        -Wextra
        -pedantic
    )

    add_test(
        NAME axis_calibration_test
        COMMAND etest_axis_calibration_test
    )
endif()

enable_testing()

# WSL 下 OpenCV 依赖附加库
if(WSL)
    target_link_libraries(etest_2026 PRIVATE opencv_core opencv_imgproc opencv_dnn opencv_videoio opencv_highgui)
    target_link_libraries(etest_test PRIVATE opencv_core)
endif()