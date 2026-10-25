include(FetchContent)
FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.7.1)
FetchContent_MakeAvailable(Catch2)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
add_executable(tests ${TEST_SOURCES})
target_link_libraries(tests PRIVATE saturnfs Catch2::Catch2WithMain)
add_test(NAME tests COMMAND tests)
