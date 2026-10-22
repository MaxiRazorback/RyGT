cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(RygtDlssTests LANGUAGES CXX)
    enable_testing()
endif()

add_executable(rygt_dlss_mode_cache_tests ModeCacheTests.cpp)
target_compile_features(rygt_dlss_mode_cache_tests PRIVATE cxx_std_17)
add_test(NAME rygt_dlss_mode_cache COMMAND rygt_dlss_mode_cache_tests)

add_executable(rygt_dlss_temporal_history_tests TemporalHistoryTest.cpp)
target_compile_features(rygt_dlss_temporal_history_tests PRIVATE cxx_std_17)
add_test(NAME rygt_dlss_temporal_history COMMAND rygt_dlss_temporal_history_tests)
