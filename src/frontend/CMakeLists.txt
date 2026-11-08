# Lógica do frontend sem SDL (menu, configuração, entrada, fonte): testável isoladamente.
add_library(dxpc_frontend STATIC font.cpp config.cpp fileio.cpp input.cpp menu.cpp states.cpp scenes.cpp gallery.cpp gallery_view.cpp)
target_include_directories(dxpc_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(dxpc_frontend PUBLIC cxx_std_20)
target_link_libraries(dxpc_frontend PUBLIC dxpc_video dxpc_audio saturnfs)
