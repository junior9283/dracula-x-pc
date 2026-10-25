file(GLOB SATURNFS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(saturnfs STATIC ${SATURNFS_SOURCES})
target_include_directories(saturnfs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(saturnfs PUBLIC cxx_std_20)
