include_guard(GLOBAL)
include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/psx_dependency_archive.cmake")

# libwebp builds its AVX2 files whenever the compiler accepts -mavx2. GCC before
# 11 accepts the flag but its immintrin.h has no _mm256_cvtsi256_si32, so the
# call compiles as an implicit declaration and the product fails to link. The
# portable Linux release container (Ubuntu 20.04) has GCC 9.4. Give those
# compilers the expression the newer headers define the intrinsic as.
function(psxrecomp_webp_avx2_compat)
    include(CheckCSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-mavx2")
    check_c_source_compiles("#include <immintrin.h>
int main(void) { return _mm_cvtsi128_si32(_mm256_castsi256_si128(_mm256_setzero_si256())); }"
        PSX_HOST_HAS_AVX2_INTRINSICS)
    check_c_source_compiles("#include <immintrin.h>
int main(void) { return _mm256_cvtsi256_si32(_mm256_setzero_si256()); }"
        PSX_HOST_HAS_MM256_CVTSI256_SI32)
    cmake_pop_check_state()
    if(PSX_HOST_HAS_AVX2_INTRINSICS AND NOT PSX_HOST_HAS_MM256_CVTSI256_SI32)
        foreach(_target IN ITEMS webpdspdecode webpdsp)
            if(TARGET ${_target})
                target_compile_options(${_target} PRIVATE
                    "-D_mm256_cvtsi256_si32(a)=_mm_cvtsi128_si32(_mm256_castsi256_si128(a))")
            endif()
        endforeach()
    endif()
endfunction()

function(psxrecomp_add_texture_image_decoder)
    if(TARGET psx_texture_image_decode)
        return()
    endif()
    psxrecomp_dependency_source_dir(psx_libwebp ENV PSX_LIBWEBP_SOURCE_DIR OUT _webp_src)
    psxrecomp_dependency_archive(psx_libwebp SOURCE_DIR "${_webp_src}"
        OUT_URL _webp_url OUT_HASH _webp_hash)
    set(_webp_timestamp_args "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        list(APPEND _webp_timestamp_args DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()
    # Populate with MakeAvailable, then explicitly exclude unused encoder/tool
    # targets from ALL. Only the static webpdecoder target is linked below.
    FetchContent_Declare(psx_libwebp URL "${_webp_url}" URL_HASH "${_webp_hash}"
        SOURCE_SUBDIR _psx_no_automatic_subdirectory ${_webp_timestamp_args})
    FetchContent_MakeAvailable(psx_libwebp)
    set(BUILD_SHARED_LIBS OFF)
    set(WEBP_LINK_STATIC ON)
    set(WEBP_USE_THREAD OFF)
    foreach(_option IN ITEMS ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP
                             WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS WEBP_JS FUZZTEST)
        set(WEBP_BUILD_${_option} OFF)
    endforeach()
    add_subdirectory("${psx_libwebp_SOURCE_DIR}" "${psx_libwebp_BINARY_DIR}" EXCLUDE_FROM_ALL)
    psxrecomp_webp_avx2_compat()
    add_library(psx_texture_image_decode STATIC "${PSXRECOMP_ROOT}/runtime/src/texture_image_decode.cpp")
    target_include_directories(psx_texture_image_decode PUBLIC "${PSXRECOMP_ROOT}/runtime/include")
    target_include_directories(psx_texture_image_decode PRIVATE "${psx_libwebp_SOURCE_DIR}/src")
    target_compile_features(psx_texture_image_decode PUBLIC cxx_std_17)
    target_link_libraries(psx_texture_image_decode PUBLIC webpdecoder)
    set_property(TARGET psx_texture_image_decode PROPERTY PSX_WEBP_INCLUDE_DIR "${psx_libwebp_SOURCE_DIR}/src")
endfunction()

psxrecomp_add_texture_image_decoder()
