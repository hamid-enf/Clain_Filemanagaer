# FileManager - CMake build (STM32CubeMX / PlatformIO / ESP-IDF / desktop)
#
# Works both as a plain CMake project/library target and as an ESP-IDF
# component: when ESP-IDF is active the component API is used, otherwise
# a normal `filemanager` static library target is created.
#
#   add_subdirectory(FileManager)        # plain CMake / PlatformIO / CubeMX
#   target_link_libraries(app PRIVATE filemanager)
#
#   # ESP-IDF: place this directory under components/ (or add
#   # set(EXTRA_COMPONENT_DIRS path/to/FileManager)) - nothing else needed.
#
# Feature switches (defaults mirror fm_config.h / the host Makefile):
#
#   -DFM_USE_LITTLEFS=OFF    drop the vendored littlefs
#   -DFM_USE_MP3=OFF         drop the vendored minimp3
#   -DFM_USE_H264=OFF        drop the vendored h264bsd
#   -DFM_USE_MJPEG=OFF       drop the vendored picojpeg
#   -DFM_USE_FATFS=ON        enable the FatFS adapter (provide ff.h)
#   -DFM_USE_THREADS=ON -DFM_SYNC_IMPL=freertos|pthread|cmsis_rtos2
#   -DFM_USE_USER_CONFIG=ON  use your fm_config_user.h instead
#
cmake_minimum_required(VERSION 3.13)

option(FM_USE_TEXT        "Text file API (line based)"               ON)
option(FM_USE_BINARY      "Typed binary read/write"                  ON)
option(FM_USE_JSON        "JSON parser/writer"                       ON)
option(FM_USE_DATA_LOGGER "Data logger"                              ON)
option(FM_USE_EXPLORER    "File explorer / navigation"               ON)
option(FM_USE_ASYNC       "Cooperative async operations"             ON)
option(FM_USE_CRC         "CRC16 / CRC32 / checksums"                ON)
option(FM_USE_SAFE_WRITE  "Atomic / crash-safe writes"               ON)
option(FM_USE_MEDIA_DETECT "File type detection"                     ON)
option(FM_USE_STREAM      "Generic stream API"                       ON)
option(FM_USE_IMAGE       "BMP image support"                        ON)
option(FM_USE_AUDIO       "WAV/PCM audio support"                    ON)
option(FM_USE_MP3         "MP3 decoding (vendored minimp3)"          ON)
option(FM_USE_VIDEO       "Video containers and player support"      ON)
option(FM_USE_H264        "H.264 decoding (vendored h264bsd)"        ON)
option(FM_USE_MJPEG       "MJPEG decoding (vendored picojpeg)"       ON)
option(FM_USE_JPEG        "Still JPEG decode/encode (picojpeg / HW)" ON)
option(FM_USE_PNG         "Still PNG decode/encode"                  ON)
option(FM_USE_EEPROM      "EEPROM byte-storage adapter"              ON)
option(FM_USE_USB_MSC     "USB mass-storage bridge"                  ON)
option(FM_USE_DEVICE_ADAPTERS "Standard SD/Flash device adapters"    ON)
option(FM_USE_PRINTF      "Logger_Printf (vsnprintf)"                ON)
option(FM_USE_CACHE       "Optional file-level read cache"           OFF)
option(FM_USE_RAMDISK     "Pointer-backed memory storage adapter"    ON)
option(FM_USE_FMFS        "Built-in mini flash filesystem"           ON)
option(FM_USE_LITTLEFS    "LittleFS adapter (vendored littlefs)"     ON)
option(FM_USE_OPUS_LIB    "Real Opus decode (vendored libopus, decoder-only)" OFF)
option(FM_USE_FATFS       "FatFS adapter (requires your ff.h)"       OFF)
option(FM_USE_POSIX       "POSIX adapter (host builds only)"         OFF)
option(FM_USE_THREADS     "Thread-safe core"                         OFF)
option(FM_USE_USER_CONFIG "Use fm_config_user.h from the include path" OFF)
set(FM_SYNC_IMPL "freertos" CACHE STRING
    "Sync port when FM_USE_THREADS=ON: freertos | cmsis_rtos2 | pthread")

# Mirror dependency rules from fm_config.h so CMake does not build vendored
# codec sources that the public configuration will compile out.
if(NOT FM_USE_AUDIO)
    set(FM_USE_MP3 OFF CACHE BOOL "MP3 decoding (vendored minimp3)" FORCE)
endif()
if(NOT FM_USE_VIDEO)
    set(FM_USE_H264 OFF CACHE BOOL "H.264 decoding (vendored h264bsd)" FORCE)
    set(FM_USE_MJPEG OFF CACHE BOOL "MJPEG decoding (vendored picojpeg)" FORCE)
endif()
if(NOT FM_USE_IMAGE)
    set(FM_USE_JPEG OFF CACHE BOOL "Still JPEG decode/encode (picojpeg / HW)" FORCE)
    set(FM_USE_PNG OFF CACHE BOOL "Still PNG decode/encode" FORCE)
endif()
if(NOT FM_USE_STREAM AND (FM_USE_IMAGE OR FM_USE_BINARY))
    set(FM_USE_STREAM ON CACHE BOOL "Generic stream API" FORCE)
endif()

# ------------------------------------------------------------------ #
# Sources                                                             #
# ------------------------------------------------------------------ #
set(FM_SOURCES
    src/fm_core.c src/fm_crc.c src/fm_path.c
    src/fm_transfer.c src/fm_qspi.c src/fm_spiflash.c
    src/fm_eeprom_chips.c src/fm_mediacache.c src/fm_log.c
    src/fm_storage.c src/fm_filesystem.c
    src/fm_file.c src/fm_directory.c
    src/fm_stream.c src/fm_text.c src/fm_binary.c
    src/fm_json.c src/fm_safe.c
    src/fm_logger.c src/fm_explorer.c
    src/fm_media.c src/fm_async.c src/fm_fmfs.c
    src/fm_image.c src/fm_jpeg.c src/fm_png.c
    src/fm_audio.c src/fm_video.c
    src/fm_eeprom.c src/fm_usb_msc.c src/fm_device.c
    src/fm_h264.c src/fm_mjpeg.c src/fm_alloc.c
    src/fm_flac.c src/fm_vorbis.c src/fm_opus.c
    third_party/stb/stb_vorbis.c
    src/fm_hw.c src/fm_player.c src/fm_sd_hal.c
    # نکته: fm_log.c هیچ وابستگی سخت‌افزاری ندارد و همه‌جا ساخته
    # می‌شود. پل UART در include/fm_log_uart_hal.h است که header-only
    # است و فقط در main.c شما include می‌شود.
    backends/fm_ramdisk.c
)

set(FM_INCLUDE_DIRS
    include src backends
    third_party/littlefs
    third_party/h264bsd/src
    third_party/minimp3
    third_party/picojpeg
)

set(FM_FEATURE_DEFINES
    FM_USE_TEXT=$<BOOL:${FM_USE_TEXT}>
    FM_USE_BINARY=$<BOOL:${FM_USE_BINARY}>
    FM_USE_JSON=$<BOOL:${FM_USE_JSON}>
    FM_USE_DATA_LOGGER=$<BOOL:${FM_USE_DATA_LOGGER}>
    FM_USE_EXPLORER=$<BOOL:${FM_USE_EXPLORER}>
    FM_USE_ASYNC=$<BOOL:${FM_USE_ASYNC}>
    FM_USE_CRC=$<BOOL:${FM_USE_CRC}>
    FM_USE_SAFE_WRITE=$<BOOL:${FM_USE_SAFE_WRITE}>
    FM_USE_MEDIA_DETECT=$<BOOL:${FM_USE_MEDIA_DETECT}>
    FM_USE_STREAM=$<BOOL:${FM_USE_STREAM}>
    FM_USE_IMAGE=$<BOOL:${FM_USE_IMAGE}>
    FM_USE_AUDIO=$<BOOL:${FM_USE_AUDIO}>
    FM_USE_MP3=$<BOOL:${FM_USE_MP3}>
    FM_USE_VIDEO=$<BOOL:${FM_USE_VIDEO}>
    FM_USE_H264=$<BOOL:${FM_USE_H264}>
    FM_USE_MJPEG=$<BOOL:${FM_USE_MJPEG}>
    FM_USE_JPEG=$<BOOL:${FM_USE_JPEG}>
    FM_USE_PNG=$<BOOL:${FM_USE_PNG}>
    FM_USE_EEPROM=$<BOOL:${FM_USE_EEPROM}>
    FM_USE_USB_MSC=$<BOOL:${FM_USE_USB_MSC}>
    FM_USE_DEVICE_ADAPTERS=$<BOOL:${FM_USE_DEVICE_ADAPTERS}>
    FM_USE_PRINTF=$<BOOL:${FM_USE_PRINTF}>
    FM_USE_CACHE=$<BOOL:${FM_USE_CACHE}>
    FM_USE_RAMDISK=$<BOOL:${FM_USE_RAMDISK}>
    FM_USE_FMFS=$<BOOL:${FM_USE_FMFS}>
    FM_USE_LITTLEFS=$<BOOL:${FM_USE_LITTLEFS}>
    FM_USE_FATFS=$<BOOL:${FM_USE_FATFS}>
    FM_USE_POSIX=$<BOOL:${FM_USE_POSIX}>
    FM_USE_THREADS=$<BOOL:${FM_USE_THREADS}>
)
set(FM_DEFINES)
if(FM_USE_USER_CONFIG)
    list(APPEND FM_DEFINES FM_USE_USER_CONFIG)
else()
    list(APPEND FM_DEFINES ${FM_FEATURE_DEFINES})
endif()

if(FM_USE_LITTLEFS)
    list(APPEND FM_SOURCES
        third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c
        backends/fm_littlefs.c)
endif()
if(FM_USE_FATFS)
    list(APPEND FM_SOURCES backends/fm_fatfs.c)
endif()
if(FM_USE_POSIX)
    list(APPEND FM_SOURCES backends/fm_posix_fs.c)
endif()
if(FM_USE_H264)
    file(GLOB FM_H264BSD_SRCS third_party/h264bsd/src/*.c)
    list(APPEND FM_SOURCES ${FM_H264BSD_SRCS})
endif()
if(FM_USE_MJPEG)
    list(APPEND FM_SOURCES third_party/picojpeg/picojpeg.c)
endif()
if(FM_USE_OPUS_LIB)
    # Decoder-only vendor tree (see third_party/README.md).  No config
    # defines needed: the vendor carries IDE-safe defaults.
    file(GLOB FM_OPUS_SRCS
        third_party/opus/src/*.c
        third_party/opus/celt/*.c
        third_party/opus/celt/arm/*.c
        third_party/opus/silk/*.c
        third_party/opus/silk/arm/*.c)
    list(APPEND FM_SOURCES ${FM_OPUS_SRCS})
    list(APPEND FM_INCLUDE_DIRS
        third_party/opus/include third_party/opus/celt
        third_party/opus/silk third_party/opus/src)
    list(APPEND FM_DEFINES FM_OPUS_USE_LIBOPUS=1)
endif()

# Vendored third-party code is upstream code.  Relax warnings for it, and
# compile h264bsd with the FileManager allocator hooks just like the Makefile
# build does (-Dmalloc=FM_Alloc -Dfree=FM_Free plus fm_alloc.h pre-included).
set(FM_THIRD_PARTY_RELAXED_OPTIONS)
set(FM_H264_ALLOC_INCLUDE_OPTIONS)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(FM_THIRD_PARTY_RELAXED_OPTIONS
        -Wno-conversion -Wno-sign-conversion -Wno-shadow -Wno-pedantic
        -Wno-unused-function -Wno-unused-parameter
        -Wno-unused-but-set-variable -Wno-maybe-uninitialized
        -Wno-sequence-point)
    set(FM_H264_ALLOC_INCLUDE_OPTIONS
        -include "${CMAKE_CURRENT_SOURCE_DIR}/include/fm_alloc.h")
    # h264bsd type-puns pointers (mv_t via u32*) - aliasing analysis
    # has been observed to miscompile it on some targets.
    set(FM_H264_ALLOC_INCLUDE_OPTIONS ${FM_H264_ALLOC_INCLUDE_OPTIONS}
        -fno-strict-aliasing)
    set(FM_PICOJPEG_SPEED_OPTIONS -O3)
elseif(MSVC)
    set(FM_H264_ALLOC_INCLUDE_OPTIONS
        /FI"${CMAKE_CURRENT_SOURCE_DIR}/include/fm_alloc.h")
endif()

foreach(_tp_src IN LISTS FM_SOURCES)
    if(_tp_src MATCHES "third_party/h264bsd/")
        set(_tp_options ${FM_THIRD_PARTY_RELAXED_OPTIONS}
                        ${FM_H264_ALLOC_INCLUDE_OPTIONS})
        set_source_files_properties(${_tp_src} PROPERTIES
            COMPILE_OPTIONS "${_tp_options}"
            COMPILE_DEFINITIONS "malloc=FM_Alloc;free=FM_Free")
    elseif(_tp_src MATCHES "third_party/")
        set(_tp_opts ${FM_THIRD_PARTY_RELAXED_OPTIONS})
        if(_tp_src MATCHES "picojpeg" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            list(APPEND _tp_opts ${FM_PICOJPEG_SPEED_OPTIONS})
        endif()
        set_source_files_properties(${_tp_src} PROPERTIES
            COMPILE_OPTIONS "${_tp_opts}")
    endif()
endforeach()

# ------------------------------------------------------------------ #
# Plain CMake / ESP-IDF dispatch                                     #
# ------------------------------------------------------------------ #
if(COMMAND idf_component_register)
    idf_component_register(
        SRCS ${FM_SOURCES}
        INCLUDE_DIRS ${FM_INCLUDE_DIRS}
        REQUIRES ${FM_IDF_REQUIRES}
    )
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ${FM_DEFINES})
    if(FM_USE_THREADS)
        if(NOT DEFINED FM_SYNC_IMPL OR FM_SYNC_IMPL STREQUAL "")
            message(FATAL_ERROR "FM_USE_THREADS=ON requires FM_SYNC_IMPL")
        endif()
        target_compile_definitions(${COMPONENT_LIB} PUBLIC
            FM_SYNC_CUSTOM)
        target_include_directories(${COMPONENT_LIB} PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/ports/${FM_SYNC_IMPL}")
    endif()
else()
    add_library(filemanager STATIC ${FM_SOURCES})
    target_include_directories(filemanager PUBLIC ${FM_INCLUDE_DIRS})
    target_compile_definitions(filemanager PUBLIC ${FM_DEFINES})

    if(NOT CMAKE_C_STANDARD)
        set_target_properties(filemanager PROPERTIES C_STANDARD 99)
    endif()

    if(FM_USE_THREADS)
        target_compile_definitions(filemanager PUBLIC FM_SYNC_CUSTOM)
        target_include_directories(filemanager PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/ports/${FM_SYNC_IMPL}")
        if(FM_SYNC_IMPL STREQUAL "pthread")
            find_package(Threads REQUIRED)
            target_link_libraries(filemanager PUBLIC Threads::Threads)
        endif()
    endif()
endif()
