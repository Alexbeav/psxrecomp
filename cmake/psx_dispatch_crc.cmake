# Only the compatibility unit needs this capability. Evaluate it per target:
# source properties are shared when several game/BIOS targets use that unit.
function(psxrecomp_dispatch_crc target dispatcher compat_source)
    set(has_crc FALSE)
    if(EXISTS "${dispatcher}")
        file(READ "${dispatcher}" dispatch_source)
        if(dispatch_source MATCHES "const[ \t\r\n]+uint32_t[ \t\r\n]*\\*[ \t\r\n]*psx_game_source_page_crc32[ \t\r\n]*\\([^;{}]*\\)[ \t\r\n]*\\{")
            set(has_crc TRUE)
        endif()
    endif()
    set_property(TARGET ${target} PROPERTY PSX_DISPATCH_HAS_SOURCE_CRC ${has_crc})
    set_property(SOURCE "${compat_source}" APPEND PROPERTY COMPILE_DEFINITIONS
        "$<$<BOOL:$<TARGET_PROPERTY:PSX_DISPATCH_HAS_SOURCE_CRC>>:PSX_GAME_DISPATCH_HAS_SOURCE_PAGE_CRC32=1>")
endfunction()
