include(FetchContent)

# Pin GoogleTest release for deterministic, reproducible test builds
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(martingale_tests
    test_smoke.cpp
    test_types.cpp
    test_order_trade.cpp
    test_price_level.cpp
    test_order_book.cpp
    test_matching_engine.cpp
    test_gateway_multi_instrument.cpp
    test_journal_and_market_data.cpp
    test_latency_recorder.cpp
)

target_link_libraries(martingale_tests
    PRIVATE
        martingale_core
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(martingale_tests)
