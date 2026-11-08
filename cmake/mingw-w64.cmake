# Cross-compilação para Windows x64 com mingw-w64 (Fedora/Nobara). Uso:
#   cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
# O SDL3 do Windows é procurado em deps-win/ (rpm mingw64-SDL3 extraído; ver tools/package/fetch-sdl3-mingw.sh) e no sys-root.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH
    ${CMAKE_CURRENT_LIST_DIR}/../deps-win/usr/x86_64-w64-mingw32/sys-root/mingw
    /usr/x86_64-w64-mingw32/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Runtime do C++ embutido: o pacote leva só o SDL3.dll.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive,-Bdynamic")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-s")
