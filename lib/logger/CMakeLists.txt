add_library(logger
    src/logger.cpp
    src/logger.h
)

target_include_directories(logger
    PUBLIC ${CMAKE_SOURCE_DIR}
)

target_link_libraries(logger
    PUBLIC
    fmt::fmt
    spdlog::spdlog
    abseil::abseil
)