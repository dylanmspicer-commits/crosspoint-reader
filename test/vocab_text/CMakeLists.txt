add_executable(VocabTextTest VocabTextTest.cpp)
target_link_libraries(VocabTextTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(VocabTextTest PRIVATE ${REPO_ROOT}/src)
gtest_discover_tests(VocabTextTest)
