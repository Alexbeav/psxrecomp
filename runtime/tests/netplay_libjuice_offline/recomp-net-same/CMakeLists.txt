# Stand-in for lib/recomp-net: the libjuice request from its CMakeLists.txt
# (recomp-net 859ba28b9), with the same URL and SHA-256.
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_SERVER ON CACHE BOOL "" FORCE)
set(USE_NETTLE OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libjuice
    URL https://github.com/paullouisageneau/libjuice/archive/refs/tags/v1.7.2.tar.gz
    URL_HASH SHA256=75159867c4a5a689a6559e11aa0d30c9eba12ce73a4ae3d898b521467e1f635d
)
FetchContent_MakeAvailable(libjuice)
