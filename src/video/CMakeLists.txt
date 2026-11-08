file(GLOB VIDEO_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(dxpc_video STATIC ${VIDEO_SOURCES})
target_include_directories(dxpc_video PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dxpc_video PUBLIC cxx_std_20)
