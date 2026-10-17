# iPhonEmu desktop executable: opens an IPSW and boots it in a window.
#
# SDL3 comes from the system when one is installed (find_package), otherwise it
# is built from the pinned release below. Point IPHONEMU_SDL3_ARCHIVE at a local
# copy of the same tarball to build offline.

set(IPHONEMU_SDL3_ARCHIVE
    "https://github.com/libsdl-org/SDL/releases/download/release-3.2.10/SDL3-3.2.10.tar.gz"
    CACHE STRING "SDL3 source archive (URL or local path)")

find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL "${IPHONEMU_SDL3_ARCHIVE}"
        URL_HASH SHA256=f87be7b4dec66db4098e9c167b2aa34e2ca10aeb5443bdde95ae03185ed513e0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

add_executable(iphonemu WIN32
    iphonemu.c
    # The machine: firmware boot, guest display, input queues, audio handoff.
    ../vm/VMAudioBuffer.c
    ../vm/VMBootOptions.c
    ../vm/VMButtonQueue.c
    ../vm/VMFirmwareAudio.c
    ../vm/VMFirmwareBoot.c
    ../vm/VMFrameTelemetry.c
    ../vm/VMGuest.c
    ../vm/VMGuestInstall.c
    ../vm/VMNetworkSession.c
    ../vm/VMOptions.c
    ../vm/VMResumeCheckpoint.c
    ../vm/VMSnapshotCow.c
    ../vm/VMTouchMap.c
    ../vm/VMTouchQueue.c
    # The IPSW importer.
    ../vm/VMFirmwareDMG.c
    ../vm/VMFirmwareDigest.c
    ../vm/VMFirmwareImport.c
    ../vm/VMFirmwareInflate.c
    ../vm/VMFirmwareKeys.c
    ../vm/VMFirmwarePlist.c
    ../vm/VMFirmwareZip.c
)
set_target_properties(iphonemu PROPERTIES
    OUTPUT_NAME iPhonEmu
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(iphonemu PRIVATE ../vm ../tools ../core/include)
# Interactive timing, as in S5LBox's app: idle WFI waits in wall time, and
# active guest time follows the host clock, bounded by the work retired.
target_compile_definitions(iphonemu PRIVATE
    S5LBOX_IOS_WFI_REALTIME_PACING=1
    S5LBOX_IOS_ACTIVE_REALTIME_CLOCK=1)
target_link_libraries(iphonemu PRIVATE
    emucore
    host_ios3_bringup_gate
    host_file_block
    host_rootfs_work
    host_sha256
    nethost
    SDL3::SDL3)
if(MSVC)
    target_compile_options(iphonemu PRIVATE /experimental:c11atomics)
endif()
