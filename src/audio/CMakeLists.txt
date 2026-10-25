file(GLOB AUDIO_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
add_library(dxpc_audio STATIC ${AUDIO_SOURCES})
target_include_directories(dxpc_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dxpc_audio PUBLIC cxx_std_20)
