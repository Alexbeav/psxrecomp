# Stand-in for a recomp-net that moved to another libjuice. The configure must
# stop before anything is fetched, naming both pins.
include(FetchContent)
FetchContent_Declare(
    libjuice
    URL https://github.com/paullouisageneau/libjuice/archive/refs/tags/v9.9.9.tar.gz
    URL_HASH SHA256=0000000000000000000000000000000000000000000000000000000000000000
)
FetchContent_MakeAvailable(libjuice)
