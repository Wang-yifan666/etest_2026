cmake_minimum_required(VERSION 3.16)

    project(etest_2026 VERSION 0.1.0 LANGUAGES CXX)

        set(CMAKE_CXX_STANDARD 17) set(CMAKE_CXX_STANDARD_REQUIRED ON)
            set(CMAKE_CXX_EXTENSIONS OFF)

                find_package(OpenCV REQUIRED) find_package(
                    Threads REQUIRED)

                    add_executable(
                        etest_2026 src / main.cpp src / core
                        / config.cpp src / core / context.cpp src / core
                        / logger.cpp src / state / state.cpp src / state
                        / start.cpp src / state / search.cpp src / state
                        / end.cpp src / state / error.cpp src / vision
                        / camera.cpp src / vision / vision.cpp src
                        / uart / uart.cpp)

                        target_include_directories(
                            etest_2026 PRIVATE ${PROJECT_SOURCE_DIR}
                            / include ${OpenCV_INCLUDE_DIRS})

                            target_link_libraries(etest_2026 PRIVATE ${
                                OpenCV_LIBS} Threads::Threads)

                                target_compile_options(
                                    etest_2026 PRIVATE - Wall - Wextra
                                    - Wpedantic)
