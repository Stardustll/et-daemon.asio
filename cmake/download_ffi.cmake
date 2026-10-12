# EasyTier FFI 下载与导入目标配置
#
# 官方预编译包按「平台-架构」分开发布：Linux/Windows/macOS 各架构一个归档。
# macOS Universal（CMAKE_OSX_ARCHITECTURES 含多架构）时下载全部架构并用 lipo
# 合成 fat dylib，使 Universal 的 qtet-daemon 两个切片都能链接到 FFI。

if(NOT DEFINED ET_FFI_VERSION)
    set(ET_FFI_VERSION "2.6.4")
endif()
message(STATUS "EasyTier FFI 版本: ${ET_FFI_VERSION}")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ET_FFI_PROCESSOR)

set(ET_FFI_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/easytier_ffi")
file(MAKE_DIRECTORY "${ET_FFI_DOWNLOAD_DIR}")

if(DOWNLOAD_FFI_FROM_CNB)
    set(ET_FFI_BASE_URL "https://cnb.cool/myqfeng/qteasytier/easytier-ffi-bin/-/releases/download/easytier-ffi-v${ET_FFI_VERSION}")
    message(STATUS "EasyTier FFI 下载源: CNB")
else()
    set(ET_FFI_BASE_URL "https://github.com/qteasytier/easytier-ffi-bin/releases/download/easytier-ffi-v${ET_FFI_VERSION}")
    message(STATUS "EasyTier FFI 下载源: GitHub")
endif()

# ── 待下载目标列表：每项 "<package_id>|<archive_name>" ──
# 单架构一项；macOS Universal 多项，随后 lipo 合成。
set(ET_FFI_TARGETS "")
if(WIN32)
    if(NOT ET_FFI_PROCESSOR MATCHES "^(amd64|x86_64)$")
        message(FATAL_ERROR "EasyTier FFI 当前未提供 Windows ${CMAKE_SYSTEM_PROCESSOR} 预编译包")
    endif()
    set(ET_FFI_LIBRARY_NAME "easytier_ffi.dll")
    set(ET_FFI_IMPLIB_NAMES "easytier_ffi.lib" "easytier_ffi.dll.lib")
    list(APPEND ET_FFI_TARGETS "windows-amd64|easytier-ffi-windows-amd64.zip")
elseif(APPLE)
    set(ET_FFI_LIBRARY_NAME "libeasytier_ffi.dylib")
    # 架构来源：优先 CMAKE_OSX_ARCHITECTURES（父工程可能传 "arm64;x86_64"），否则宿主架构
    set(ET_FFI_APPLE_ARCHES "")
    if(CMAKE_OSX_ARCHITECTURES)
        foreach(ET_FFI_ARCH IN LISTS CMAKE_OSX_ARCHITECTURES)
            string(TOLOWER "${ET_FFI_ARCH}" ET_FFI_ARCH_LOWER)
            list(APPEND ET_FFI_APPLE_ARCHES "${ET_FFI_ARCH_LOWER}")
        endforeach()
    else()
        list(APPEND ET_FFI_APPLE_ARCHES "${ET_FFI_PROCESSOR}")
    endif()
    list(REMOVE_DUPLICATES ET_FFI_APPLE_ARCHES)
    foreach(ET_FFI_ARCH IN LISTS ET_FFI_APPLE_ARCHES)
        if(ET_FFI_ARCH MATCHES "^(arm64|aarch64)$")
            list(APPEND ET_FFI_TARGETS "macos-aarch64|easytier-ffi-macos-aarch64.tar.gz")
        elseif(ET_FFI_ARCH MATCHES "^(amd64|x86_64)$")
            list(APPEND ET_FFI_TARGETS "macos-amd64|easytier-ffi-macos-amd64.tar.gz")
        else()
            message(FATAL_ERROR "EasyTier FFI 当前未提供 macOS ${ET_FFI_ARCH} 预编译包")
        endif()
    endforeach()
elseif(UNIX)
    if(ET_FFI_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(ET_FFI_LIBRARY_NAME "libeasytier_ffi.so")
        list(APPEND ET_FFI_TARGETS "linux-aarch64|easytier-ffi-linux-aarch64.tar.gz")
    elseif(ET_FFI_PROCESSOR MATCHES "^(amd64|x86_64)$")
        set(ET_FFI_LIBRARY_NAME "libeasytier_ffi.so")
        list(APPEND ET_FFI_TARGETS "linux-amd64|easytier-ffi-linux-amd64.tar.gz")
    else()
        message(FATAL_ERROR "EasyTier FFI 当前未提供 Linux ${CMAKE_SYSTEM_PROCESSOR} 预编译包")
    endif()
else()
    message(FATAL_ERROR "EasyTier FFI 当前未支持该平台: ${CMAKE_SYSTEM_NAME}")
endif()

if(NOT ET_FFI_TARGETS)
    message(FATAL_ERROR "EasyTier FFI 未解析到任何下载目标 (${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR})")
endif()

# ── 逐目标下载 + 解压（各自独立缓存：归档非空且 stamp 存在则跳过）──
set(ET_FFI_EXTRACT_DIRS "")
set(ET_FFI_LIBRARY_CANDIDATES "")
foreach(ET_FFI_TARGET IN LISTS ET_FFI_TARGETS)
    string(REPLACE "|" ";" ET_FFI_TARGET_PARTS "${ET_FFI_TARGET}")
    list(GET ET_FFI_TARGET_PARTS 0 ET_FFI_PACKAGE_ID)
    list(GET ET_FFI_TARGET_PARTS 1 ET_FFI_ARCHIVE_NAME)

    set(ET_FFI_ARCHIVE_PATH "${ET_FFI_DOWNLOAD_DIR}/${ET_FFI_ARCHIVE_NAME}")
    set(ET_FFI_EXTRACT_DIR "${ET_FFI_DOWNLOAD_DIR}/extracted-${ET_FFI_PACKAGE_ID}")
    set(ET_FFI_EXTRACT_STAMP "${ET_FFI_DOWNLOAD_DIR}/extracted-${ET_FFI_PACKAGE_ID}.stamp")

    if(EXISTS "${ET_FFI_ARCHIVE_PATH}")
        file(SIZE "${ET_FFI_ARCHIVE_PATH}" ET_FFI_ARCHIVE_SIZE)
        if(ET_FFI_ARCHIVE_SIZE EQUAL 0)
            file(REMOVE "${ET_FFI_ARCHIVE_PATH}")
        endif()
    endif()

    if(NOT EXISTS "${ET_FFI_ARCHIVE_PATH}")
        file(DOWNLOAD "${ET_FFI_BASE_URL}/${ET_FFI_ARCHIVE_NAME}" "${ET_FFI_ARCHIVE_PATH}"
             SHOW_PROGRESS
             STATUS ET_FFI_DL_STATUS
             TIMEOUT 120)
        list(GET ET_FFI_DL_STATUS 0 ET_FFI_DL_CODE)
        if(NOT ET_FFI_DL_CODE EQUAL 0)
            list(GET ET_FFI_DL_STATUS 1 ET_FFI_DL_MSG)
            message(FATAL_ERROR "下载 EasyTier FFI 包失败 (${ET_FFI_BASE_URL}/${ET_FFI_ARCHIVE_NAME}): ${ET_FFI_DL_MSG}")
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

    list(APPEND ET_FFI_EXTRACT_DIRS "${ET_FFI_EXTRACT_DIR}")

    # 在本架构解压目录内定位库文件（可能位于包内子目录，用 GLOB_RECURSE）
    file(GLOB_RECURSE ET_FFI_ARCH_LIB_FILES
        LIST_DIRECTORIES false
        "${ET_FFI_EXTRACT_DIR}/${ET_FFI_LIBRARY_NAME}")
    list(LENGTH ET_FFI_ARCH_LIB_FILES ET_FFI_ARCH_LIB_COUNT)
    if(ET_FFI_ARCH_LIB_COUNT EQUAL 0)
        message(FATAL_ERROR "EasyTier FFI 包中未找到动态库: ${ET_FFI_LIBRARY_NAME} (${ET_FFI_PACKAGE_ID})")
    endif()
    list(GET ET_FFI_ARCH_LIB_FILES 0 ET_FFI_ARCH_LIB)
    list(APPEND ET_FFI_LIBRARY_CANDIDATES "${ET_FFI_ARCH_LIB}")
endforeach()

list(GET ET_FFI_EXTRACT_DIRS 0 ET_FFI_PRIMARY_DIR)

# ── 库文件：单架构直接取；多架构（macOS Universal）lipo 合成 fat 库 ──
list(LENGTH ET_FFI_LIBRARY_CANDIDATES ET_FFI_LIB_COUNT)
if(ET_FFI_LIB_COUNT GREATER 1)
    find_program(LIPO_EXECUTABLE lipo REQUIRED)
    set(ET_FFI_UNIVERSAL_DIR "${ET_FFI_DOWNLOAD_DIR}/universal")
    file(MAKE_DIRECTORY "${ET_FFI_UNIVERSAL_DIR}")
    set(ET_FFI_LIBRARY_FILE "${ET_FFI_UNIVERSAL_DIR}/${ET_FFI_LIBRARY_NAME}")
    execute_process(
        COMMAND "${LIPO_EXECUTABLE}" -create ${ET_FFI_LIBRARY_CANDIDATES} -output "${ET_FFI_LIBRARY_FILE}"
        RESULT_VARIABLE ET_FFI_LIPO_RESULT
    )
    if(NOT ET_FFI_LIPO_RESULT EQUAL 0)
        message(FATAL_ERROR "lipo 合成 Universal ${ET_FFI_LIBRARY_NAME} 失败（架构: ${CMAKE_OSX_ARCHITECTURES}）")
    endif()
    message(STATUS "已合成 Universal FFI 动态库: ${ET_FFI_LIBRARY_FILE}")
else()
    list(GET ET_FFI_LIBRARY_CANDIDATES 0 ET_FFI_LIBRARY_FILE)
endif()

# ── macOS：修正 install name ──
# 官方 dylib 的 install name 是 CI 构建机的绝对路径（/Users/runner/work/…），
# 链接时会原样写进 daemon 的 LC_LOAD_DYLIB，导致本机运行时找不到库。改为
# @rpath/<name>，配合 daemon 的 @loader_path RPATH 从自身目录加载。
# 改 Mach-O 会使原签名失效，须重新 ad-hoc 签名（arm64 上无签名无法加载）。
if(APPLE)
    find_program(INSTALL_NAME_TOOL_EXECUTABLE install_name_tool REQUIRED)
    execute_process(
        COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}" -id "@rpath/${ET_FFI_LIBRARY_NAME}" "${ET_FFI_LIBRARY_FILE}"
        RESULT_VARIABLE ET_FFI_INSTALL_NAME_RESULT
    )
    if(NOT ET_FFI_INSTALL_NAME_RESULT EQUAL 0)
        message(FATAL_ERROR "install_name_tool 修正 FFI install name 失败: ${ET_FFI_LIBRARY_FILE}")
    endif()

    find_program(CODESIGN_EXECUTABLE codesign REQUIRED)
    execute_process(
        COMMAND "${CODESIGN_EXECUTABLE}" --force --sign - "${ET_FFI_LIBRARY_FILE}"
        RESULT_VARIABLE ET_FFI_CODESIGN_RESULT
        OUTPUT_QUIET
    )
    if(NOT ET_FFI_CODESIGN_RESULT EQUAL 0)
        message(FATAL_ERROR "重新 ad-hoc 签名 FFI 动态库失败: ${ET_FFI_LIBRARY_FILE}")
    endif()
endif()

# ── 运行时文件收集：以「首个架构解压目录」为基准 ──
# 库文件（可能是主目录之外 lipo 合成的 universal 库）在末尾统一追加，
# 故循环内跳过同名库文件，避免重复与同名冲突误报。
file(GLOB_RECURSE ET_FFI_PRIMARY_FILES
    LIST_DIRECTORIES false
    "${ET_FFI_PRIMARY_DIR}/*")

set(ET_FFI_IMPLIB_FILE "")
set(ET_FFI_RUNTIME_FILES "")
set(ET_FFI_RUNTIME_BASENAMES "")
set(ET_FFI_EXCLUDED_RUNTIME_BASENAMES ".extract-stamp")

foreach(ET_FFI_FILE IN LISTS ET_FFI_PRIMARY_FILES)
    get_filename_component(ET_FFI_FILE_NAME "${ET_FFI_FILE}" NAME)
    string(TOLOWER "${ET_FFI_FILE_NAME}" ET_FFI_FILE_NAME_LOWER)
    get_filename_component(ET_FFI_FILE_EXT "${ET_FFI_FILE}" EXT)
    string(TOLOWER "${ET_FFI_FILE_EXT}" ET_FFI_FILE_EXT_LOWER)

    # 库文件交由 ET_FFI_LIBRARY_FILE 统一提交
    if(ET_FFI_FILE_NAME STREQUAL ET_FFI_LIBRARY_NAME)
        continue()
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

list(APPEND ET_FFI_RUNTIME_FILES "${ET_FFI_LIBRARY_FILE}")
list(APPEND ET_FFI_RUNTIME_BASENAMES "${ET_FFI_LIBRARY_NAME}")

list(REMOVE_DUPLICATES ET_FFI_EXCLUDED_RUNTIME_BASENAMES)

if(NOT ET_FFI_LIBRARY_FILE OR NOT EXISTS "${ET_FFI_LIBRARY_FILE}")
    message(FATAL_ERROR "EasyTier FFI 动态库缺失: ${ET_FFI_LIBRARY_FILE}")
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
