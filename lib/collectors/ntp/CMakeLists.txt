add_library(ntp 
    src/ntp.h
    src/ntp.cpp
)

target_include_directories(ntp
    PUBLIC ${CMAKE_SOURCE_DIR}
)

target_link_libraries(ntp
    abseil::abseil
    fmt::fmt
    logger
    tagging
)

add_executable(ntp_test
    test/ntp_test.cpp
)

target_link_libraries(ntp_test
    gtest::gtest
    logger
    measurement_utils
    ntp
    spectator
    util
)

add_test(
    NAME ntp_test
    COMMAND ntp_test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)