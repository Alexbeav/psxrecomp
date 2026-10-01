# bios_emitter_fingerprint.cmake — THE emitter fingerprint, as a CMake script.
#
#   cmake -DPROFILE=bios/<stem>.toml [-DROOT=<framework>] [-DFP_DEBUG=1] \
#         -P tools/bios_emitter_fingerprint.cmake
#
# Prints one SHA-256 over every input that shapes the generated BIOS C: the
# psxrecomp-bios emitter sources, the cycle headers they bake in, the profile
# and its seeds. Not the ROM image: the profile pins its SHA-256 and the
# emitter refuses any other image, and CI has no dump.
#
# WHY CMAKE AND NOT BASH. runtime.cmake compares this against the committed
# generated/<stem>.emitter.sha at configure (fatal in release CI). A bash
# implementation hashed differently depending on which bash/coreutils CMake
# found on a Windows runner (Git Bash in the step shell agreed with Linux; the
# bash CMake located did not), so a byte-identical tree read as STALE there
# only. cmake is the one tool every configure already has, and
# file(READ)+string(SHA256) behave the same everywhere. Carriage returns are
# stripped before hashing so a CRLF checkout agrees with an LF one.
#
# Digest shape (kept identical to the previous bash form so existing stamps
# stay valid): per file, "<sha256 of CR-stripped bytes>  -\n"; the fingerprint
# is the SHA-256 of that concatenated listing.
cmake_minimum_required(VERSION 3.20)
if(NOT ROOT)
    get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT PROFILE)
    set(PROFILE "bios/SCPH1001.toml")
endif()
if(NOT IS_ABSOLUTE "${PROFILE}")
    set(PROFILE "${ROOT}/${PROFILE}")
endif()

set(_files
    recompiler/src/full_function_emitter.cpp
    recompiler/src/full_function_emitter.h
    recompiler/src/strict_translator.cpp
    recompiler/src/main_bios.cpp
    recompiler/src/function_discovery.cpp
    recompiler/src/bios_address_model.cpp
    recompiler/src/bios_address_model.h
    recompiler/src/config_loader.cpp
    recompiler/src/control_flow.cpp
    recompiler/src/function_analysis.cpp
    recompiler/src/mips_decoder.cpp
    recompiler/src/bios_slice_walker.cpp
    recompiler/src/basic_block.cpp
    runtime/include/psx_cyc.h
    runtime/include/psx_instr_cost.h)
set(_paths)
foreach(_f IN LISTS _files)
    list(APPEND _paths "${ROOT}/${_f}")
endforeach()

# Per-profile inputs: the profile itself and the seeds file it names.
if(EXISTS "${PROFILE}")
    list(APPEND _paths "${PROFILE}")
    file(STRINGS "${PROFILE}" _seed_lines REGEX "^[ \t]*seeds[ \t]*=[ \t]*\"[^\"]*\"")
    if(_seed_lines)
        list(GET _seed_lines 0 _seed_line)
        string(REGEX REPLACE "^[ \t]*seeds[ \t]*=[ \t]*\"([^\"]*)\".*$" "\\1" _seeds "${_seed_line}")
        if(_seeds)
            list(APPEND _paths "${ROOT}/${_seeds}")
        endif()
    endif()
else()
    list(APPEND _paths "${ROOT}/recompiler/seeds/phase2_ghidra_seeds.json")
endif()

set(_listing "")
foreach(_p IN LISTS _paths)
    if(NOT EXISTS "${_p}")
        continue()
    endif()
    file(READ "${_p}" _content)
    string(REPLACE "\r" "" _content "${_content}")
    string(SHA256 _d "${_content}")
    if(FP_DEBUG)
        file(RELATIVE_PATH _rel "${ROOT}" "${_p}")
        message("${_d}  ${_rel}")
    endif()
    string(APPEND _listing "${_d}  -\n")
endforeach()
string(SHA256 _fp "${_listing}")
# message() writes to stderr; the digest is the script's stdout contract.
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${_fp}")
