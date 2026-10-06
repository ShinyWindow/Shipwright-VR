# SOH [VR] Android: the third-party libraries the desktop builds take from vcpkg / the system
# (ogg, opus, vorbis, opusfile, SDL_net), built from source. SDL2 itself comes from
# libultraship/cmake/dependencies/android.cmake. Include from soh/CMakeLists.txt.
# Defines SOH_ANDROID_DEP_LIBS (link) and SOH_ANDROID_DEP_INCLUDES (include dirs).

include(FetchContent)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)

#=================== ogg ===================
FetchContent_Declare(
    libogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
)
FetchContent_MakeAvailable(libogg)

#=================== opus ===================
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
)
FetchContent_MakeAvailable(opus)

#=================== vorbis ===================
# vorbis' find_package(Ogg) goes through CMake/FindOgg.cmake; these satisfy it with the ogg built
# above (its Ogg::ogg alias target already exists, so FindOgg makes no imported target).
set(OGG_INCLUDE_DIR "${libogg_SOURCE_DIR}/include;${libogg_BINARY_DIR}/include" CACHE STRING "" FORCE)
set(OGG_LIBRARY ogg CACHE STRING "" FORCE)
FetchContent_Declare(
    libvorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7
)
FetchContent_MakeAvailable(libvorbis)

#=================== opusfile (no CMake project at v0.12) ===================
# Fetch only: SOURCE_SUBDIR points at a directory without a CMakeLists.txt, so nothing is added.
FetchContent_Declare(
    opusfile
    GIT_REPOSITORY https://github.com/xiph/opusfile.git
    GIT_TAG v0.12
    SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(opusfile)
add_library(opusfile STATIC
    ${opusfile_SOURCE_DIR}/src/info.c
    ${opusfile_SOURCE_DIR}/src/internal.c
    ${opusfile_SOURCE_DIR}/src/opusfile.c
    ${opusfile_SOURCE_DIR}/src/stream.c
)
target_include_directories(opusfile PUBLIC ${opusfile_SOURCE_DIR}/include)
target_link_libraries(opusfile PUBLIC opus ogg)

#=================== SDL_net ===================
set(SDL2NET_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2NET_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2NET_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# A static SDL_net looks for SDL2::SDL2-static; SDL2 is shared here (SDL's Java side loads
# libSDL2.so), and linking that from a static SDL_net is fine.
if(NOT TARGET SDL2::SDL2-static)
    add_library(SDL2::SDL2-static ALIAS SDL2)
endif()
FetchContent_Declare(
    SDL2_net
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_net.git
    GIT_TAG release-2.2.0
)
FetchContent_MakeAvailable(SDL2_net)

#=================== include layout the sources expect ===================
# soh includes <opus/opus.h> and <SDL2/SDL_net.h>; the source trees have them flat.
set(SOH_ANDROID_COMPAT_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/android-compat-include")
file(MAKE_DIRECTORY "${SOH_ANDROID_COMPAT_INCLUDE}/opus" "${SOH_ANDROID_COMPAT_INCLUDE}/SDL2")
file(GLOB _soh_opus_headers "${opus_SOURCE_DIR}/include/*.h")
file(COPY ${_soh_opus_headers} DESTINATION "${SOH_ANDROID_COMPAT_INCLUDE}/opus")
file(COPY "${sdl2_net_SOURCE_DIR}/SDL_net.h" DESTINATION "${SOH_ANDROID_COMPAT_INCLUDE}/SDL2")

set(SOH_ANDROID_DEP_INCLUDES
    ${SOH_ANDROID_COMPAT_INCLUDE}
    ${opus_SOURCE_DIR}/include
)
set(SOH_ANDROID_DEP_LIBS
    ogg
    opus
    vorbis
    vorbisenc
    vorbisfile
    opusfile
    SDL2_net::SDL2_net-static
)
