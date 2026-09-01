# EasyTier FFI 下载与导入目标配置

if(NOT DEFINED ET_FFI_VERSION)
    set(ET_FFI_VERSION "2.6.4")
endif()
message(STATUS "EasyTier FFI 版本: ${ET_FFI_VERSION}")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ET_FFI_PROCESSOR)

if(WIN32)
    if(NOT ET_FFI_PROCESSOR MATCHES "^(amd64|x86_64)$")
        message(FATAL_ERROR "EasyTier FFI 当前未提供 Windows ${CMAKE_SYSTEM_PROCESSOR} 预编译包")
    endif()
    set(ET_FFI_PACKAGE_ID "windows-amd64")
    set(ET_FFI_ARCHIVE_NAME "easytier-ffi-windows-amd64.zip")
    set(ET_FFI_LIBRARY_NAME "easytier_ffi.dll")
    set(ET_FFI_IMPLIB_NAMES "easytier_ffi.lib" "easytier_ffi.dll.lib")
elseif(APPLE)
    if(ET_FFI_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(ET_FFI_PACKAGE_ID "macos-aarch64")
        set(ET_FFI_ARCHIVE_NAME "easytier-ffi-macos-aarch64.tar.gz")
    elseif(ET_FFI_PROCESSOR MATCHES "^(amd64|x86_64)$")
        set(ET_FFI_PACKAGE_ID "macos-amd64")
        set(ET_FFI_ARCHIVE_NAME "easytier-ffi-macos-amd64.tar.gz")
    else()
        message(FATAL_ERROR "EasyTier FFI 当前未提供 macOS ${CMAKE_SYSTEM_PROCESSOR} 预编译包")
    endif()
    set(ET_FFI_LIBRARY_NAME "libeasytier_ffi.dylib")
elseif(UNIX)
    if(ET_FFI_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(ET_FFI_PACKAGE_ID "linux-aarch64")
        set(ET_FFI_ARCHIVE_NAME "easytier-ffi-linux-aarch64.tar.gz")
    elseif(ET_FFI_PROCESSOR MATCHES "^(amd64|x86_64)$")
        set(ET_FFI_PACKAGE_ID "linux-amd64")
        set(ET_FFI_ARCHIVE_NAME "easytier-ffi-linux-amd64.tar.gz")
    else()
        message(FATAL_ERROR "EasyTier FFI 当前未提供 Linux ${CMAKE_SYSTEM_PROCESSOR} 预编译包")
    endif()
    set(ET_FFI_LIBRARY_NAME "libeasytier_ffi.so")
else()
    message(FATAL_ERROR "EasyTier FFI 当前未支持该平台: ${CMAKE_SYSTEM_NAME}")
endif()

set(ET_FFI_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/easytier_ffi")
set(ET_FFI_ARCHIVE_PATH "${ET_FFI_DOWNLOAD_DIR}/${ET_FFI_ARCHIVE_NAME}")
set(ET_FFI_EXTRACT_DIR "${ET_FFI_DOWNLOAD_DIR}/extracted-${ET_FFI_PACKAGE_ID}")
set(ET_FFI_EXTRACT_STAMP "${ET_FFI_DOWNLOAD_DIR}/extracted-${ET_FFI_PACKAGE_ID}.stamp")
if(DOWNLOAD_FFI_FROM_CNB)
    set(ET_FFI_URL "https://cnb.cool/myqfeng/qteasytier/easytier-ffi-bin/-/releases/download/easytier-ffi-v${ET_FFI_VERSION}/${ET_FFI_ARCHIVE_NAME}")
    message(STATUS "EasyTier FFI 下载源: CNB")
else()
    set(ET_FFI_URL "https://github.com/qteasytier/easytier-ffi-bin/releases/download/easytier-ffi-v${ET_FFI_VERSION}/${ET_FFI_ARCHIVE_NAME}")
    message(STATUS "EasyTier FFI 下载源: GitHub")
endif()

file(MAKE_DIRECTORY "${ET_FFI_DOWNLOAD_DIR}")

if(EXISTS "${ET_FFI_ARCHIVE_PATH}")
    file(SIZE "${ET_FFI_ARCHIVE_PATH}" ET_FFI_ARCHIVE_SIZE)
    if(ET_FFI_ARCHIVE_SIZE EQUAL 0)
        file(REMOVE "${ET_FFI_ARCHIVE_PATH}")
    endif()
endif()

if(NOT EXISTS "${ET_FFI_ARCHIVE_PATH}")
    file(DOWNLOAD "${ET_FFI_URL}" "${ET_FFI_ARCHIVE_PATH}"
         SHOW_PROGRESS
         STATUS ET_FFI_DL_STATUS
         TIMEOUT 120)
    list(GET ET_FFI_DL_STATUS 0 ET_FFI_DL_CODE)
    if(NOT ET_FFI_DL_CODE EQUAL 0)
        list(GET ET_FFI_DL_STATUS 1 ET_FFI_DL_MSG)
        message(FATAL_ERROR "下载 EasyTier FFI 包失败 (${ET_FFI_URL}): ${ET_FFI_DL_MSG}")
    endif()
    message(STATUS "已下载 EasyTier FFI 包: ${ET_FFI_ARCHIVE_PATH}")
endif()

file(SIZE "${ET_FFI_ARCHIVE_PATH}" ET_FFI_ARCHIVE_SIZE)
if(ET_FFI_ARCHIVE_SIZE EQUAL 0)
    message(FATAL_ERROR "下载 EasyTier FFI 包为空文件: ${ET_FFI_ARCHIVE_PATH}")
endif()

if(EXISTS "${ET_FFI_EXTRACT_STAMP}")
    file(GLOB_RECURSE ET_FFI_EXISTING_EXTRACTED_FILES
        LIST_DIRECTORIES false
        "${ET_FFI_EXTRACT_DIR}/*")
    list(LENGTH ET_FFI_EXISTING_EXTRACTED_FILES ET_FFI_EXISTING_EXTRACTED_FILE_COUNT)
    if(ET_FFI_EXISTING_EXTRACTED_FILE_COUNT EQUAL 0)
        file(REMOVE "${ET_FFI_EXTRACT_STAMP}")
    endif()
endif()

if(NOT EXISTS "${ET_FFI_EXTRACT_STAMP}")
    file(REMOVE_RECURSE "${ET_FFI_EXTRACT_DIR}")
    file(MAKE_DIRECTORY "${ET_FFI_EXTRACT_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${ET_FFI_ARCHIVE_PATH}" DESTINATION "${ET_FFI_EXTRACT_DIR}")
    file(WRITE "${ET_FFI_EXTRACT_STAMP}" "${ET_FFI_ARCHIVE_NAME}\n")
    message(STATUS "已解压 EasyTier FFI 包: ${ET_FFI_EXTRACT_DIR}")
endif()

file(GLOB_RECURSE ET_FFI_EXTRACTED_FILES
    LIST_DIRECTORIES false
    "${ET_FFI_EXTRACT_DIR}/*")

set(ET_FFI_LIBRARY_FILE "")
set(ET_FFI_IMPLIB_FILE "")
set(ET_FFI_RUNTIME_FILES "")
set(ET_FFI_RUNTIME_BASENAMES "")
set(ET_FFI_EXCLUDED_RUNTIME_BASENAMES ".extract-stamp")

foreach(ET_FFI_FILE IN LISTS ET_FFI_EXTRACTED_FILES)
    get_filename_component(ET_FFI_FILE_NAME "${ET_FFI_FILE}" NAME)
    string(TOLOWER "${ET_FFI_FILE_NAME}" ET_FFI_FILE_NAME_LOWER)
    get_filename_component(ET_FFI_FILE_EXT "${ET_FFI_FILE}" EXT)
    string(TOLOWER "${ET_FFI_FILE_EXT}" ET_FFI_FILE_EXT_LOWER)

    if(ET_FFI_FILE_NAME STREQUAL ET_FFI_LIBRARY_NAME)
        set(ET_FFI_LIBRARY_FILE "${ET_FFI_FILE}")
    endif()
    if(WIN32)
        list(FIND ET_FFI_IMPLIB_NAMES "${ET_FFI_FILE_NAME}" ET_FFI_IMPLIB_INDEX)
        if(NOT ET_FFI_IMPLIB_INDEX EQUAL -1)
            set(ET_FFI_IMPLIB_FILE "${ET_FFI_FILE}")
        endif()
    endif()

    if(ET_FFI_FILE_NAME STREQUAL ".extract-stamp"
            OR ET_FFI_FILE_NAME STREQUAL "BUILD_INFO.txt"
            OR ET_FFI_FILE_NAME_LOWER MATCHES "\\.(lib|a)$")
        list(APPEND ET_FFI_EXCLUDED_RUNTIME_BASENAMES "${ET_FFI_FILE_NAME}")
        continue()
    endif()

    list(FIND ET_FFI_RUNTIME_BASENAMES "${ET_FFI_FILE_NAME}" ET_FFI_DUPLICATE_INDEX)
    if(NOT ET_FFI_DUPLICATE_INDEX EQUAL -1)
        message(FATAL_ERROR "EasyTier FFI 运行时文件扁平复制存在同名冲突: ${ET_FFI_FILE_NAME}")
    endif()
    list(APPEND ET_FFI_RUNTIME_BASENAMES "${ET_FFI_FILE_NAME}")
    list(APPEND ET_FFI_RUNTIME_FILES "${ET_FFI_FILE}")
endforeach()

list(REMOVE_DUPLICATES ET_FFI_EXCLUDED_RUNTIME_BASENAMES)

if(NOT ET_FFI_LIBRARY_FILE)
    message(FATAL_ERROR "EasyTier FFI 包中未找到动态库: ${ET_FFI_LIBRARY_NAME}")
endif()
if(WIN32 AND NOT ET_FFI_IMPLIB_FILE)
    message(FATAL_ERROR "EasyTier FFI 包中未找到 Windows 导入库: ${ET_FFI_IMPLIB_NAMES}")
endif()

add_library(easytier_ffi SHARED IMPORTED)
set_target_properties(easytier_ffi PROPERTIES
    IMPORTED_LOCATION "${ET_FFI_LIBRARY_FILE}"
    IMPORTED_NO_SONAME TRUE
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/easytier/include"
)

if(WIN32)
    set_target_properties(easytier_ffi PROPERTIES
        IMPORTED_IMPLIB "${ET_FFI_IMPLIB_FILE}"
    )
endif()
