add_executable(WordDetectTest WordDetectTest.cpp)
target_link_libraries(WordDetectTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(WordDetectTest PRIVATE ${REPO_ROOT}/src)
gtest_discover_tests(WordDetectTest)
