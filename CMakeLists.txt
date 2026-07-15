cmake_minimum_required(VERSION 3.24)
project(bean VERSION 0.2.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BEAN_ENABLE_LIBOBS "Enable libobs recorder engine implementation" OFF)
option(BEAN_ENABLE_VELOPACK "Enable Velopack updater integration" OFF)

set(BEAN_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")

add_library(bean_core
    src/core/RecordingOrchestrator.cpp
    src/core/RunRepository.cpp
    src/core/SettingsStore.cpp
    src/integrations/YouTubeUploader.cpp
    src/log/CombatLogWatcher.cpp
    src/log/MythicRunDetector.cpp
    src/obs/MockRecorderEngine.cpp
    src/obs/LibObsRecorderEngine.cpp
)

target_include_directories(bean_core PUBLIC src)

if(MSVC)
    target_compile_options(bean_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(bean_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BEAN_ENABLE_LIBOBS)
    target_compile_definitions(bean_core PUBLIC BEAN_ENABLE_LIBOBS=1)
endif()

add_executable(bean_app WIN32
    src/app/ClipPreviewEngine.cpp
    src/app/main.cpp
    src/app/AppDraw.cpp
    src/app/AppIconsTaskbar.cpp
    src/app/AppLayout.cpp
    src/app/BeanUpdater.cpp
    src/app/AppRecordingHelpers.cpp
    src/app/AppStatusLog.cpp
    src/app/AppUtilities.cpp
)
set_target_properties(bean_app PROPERTIES OUTPUT_NAME "bean")

set(BEAN_EMBEDDED_ICON "${CMAKE_SOURCE_DIR}/assets/icons/bean_idle.ico")
if(NOT EXISTS "${BEAN_EMBEDDED_ICON}")
    foreach(candidate
        "${CMAKE_SOURCE_DIR}/assets/icons/bean-256.ico"
        "${CMAKE_SOURCE_DIR}/assets/icons/bean-48.ico"
        "${CMAKE_SOURCE_DIR}/assets/icons/bean-32.ico"
        "${CMAKE_SOURCE_DIR}/assets/icons/bean-16.ico")
        if(EXISTS "${candidate}")
            set(BEAN_EMBEDDED_ICON "${candidate}")
            break()
        endif()
    endforeach()
endif()

if(EXISTS "${BEAN_EMBEDDED_ICON}")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/app/bean.rc.in"
        "${BEAN_GENERATED_DIR}/bean_app_icon.rc"
        @ONLY
    )
    target_sources(bean_app PRIVATE "${BEAN_GENERATED_DIR}/bean_app_icon.rc")
    target_compile_definitions(bean_app PRIVATE BEAN_HAS_EMBEDDED_ICON=1)
else()
    message(STATUS "No app icon found in assets/icons (expected bean_idle.ico or bean-icon-{16,32,48,256}.ico); executable icon resource will be skipped.")
endif()

add_custom_command(TARGET bean_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:bean_app>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:bean_app>/assets/spec-icons"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/icons"
        "$<TARGET_FILE_DIR:bean_app>/assets/icons"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/spec-icons"
        "$<TARGET_FILE_DIR:bean_app>/assets/spec-icons"
    COMMENT "Copying app icon assets"
)

target_include_directories(bean_app PRIVATE "${BEAN_GENERATED_DIR}")

target_link_libraries(bean_app PRIVATE bean_core)
target_link_libraries(bean_app PRIVATE Comctl32 Shell32 Ole32 Mf Mfplat Mfreadwrite Mfuuid Shlwapi)
target_link_libraries(bean_core PRIVATE Winhttp Bcrypt Crypt32)

if(BEAN_ENABLE_VELOPACK)
    set(BEAN_VELOPACK_SDK_DIR "" CACHE PATH "Path to extracted velopack_libc directory")
    set(BEAN_VELOPACK_UPDATE_URL "" CACHE STRING "Default update feed URL used by Velopack UpdateManager")

    if(NOT BEAN_VELOPACK_SDK_DIR)
        message(FATAL_ERROR "BEAN_ENABLE_VELOPACK is ON but BEAN_VELOPACK_SDK_DIR is not set")
    endif()

    target_compile_definitions(bean_app PRIVATE BEAN_ENABLE_VELOPACK=1)
    if(BEAN_VELOPACK_UPDATE_URL)
        target_compile_definitions(bean_app PRIVATE BEAN_VELOPACK_UPDATE_URL="${BEAN_VELOPACK_UPDATE_URL}")
    endif()

    target_include_directories(bean_app PRIVATE "${BEAN_VELOPACK_SDK_DIR}/include")
    find_library(BEAN_VELOPACK_LIBRARY
        NAMES
            velopack_libc_win_x64_msvc.dll.lib
            velopack_libc_win_x64_msvc.lib
            Velopack
            velopack
        PATHS "${BEAN_VELOPACK_SDK_DIR}/lib" "${BEAN_VELOPACK_SDK_DIR}/lib-static"
        NO_DEFAULT_PATH
        REQUIRED)
    target_link_libraries(bean_app PRIVATE "${BEAN_VELOPACK_LIBRARY}")

    set(BEAN_VELOPACK_DLL "")
    foreach(candidate
        "${BEAN_VELOPACK_SDK_DIR}/bin/Velopack.dll"
        "${BEAN_VELOPACK_SDK_DIR}/bin/velopack.dll"
        "${BEAN_VELOPACK_SDK_DIR}/lib/Velopack.dll"
        "${BEAN_VELOPACK_SDK_DIR}/lib/velopack.dll"
        "${BEAN_VELOPACK_SDK_DIR}/lib/velopack_libc_win_x64_msvc.dll")
        if(EXISTS "${candidate}")
            set(BEAN_VELOPACK_DLL "${candidate}")
            break()
        endif()
    endforeach()

    if(BEAN_VELOPACK_DLL)
        add_custom_command(TARGET bean_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BEAN_VELOPACK_DLL}"
                "$<TARGET_FILE_DIR:bean_app>/velopack_libc.dll"
            COMMENT "Copying Velopack runtime"
        )
    endif()
endif()

add_executable(bean_tests
    tests/MythicRunDetectorTests.cpp
)
target_link_libraries(bean_tests PRIVATE bean_core)

add_executable(bean_core_public_api_tests
    tests/CorePublicApiTests.cpp
)
target_link_libraries(bean_core_public_api_tests PRIVATE bean_core)

enable_testing()
add_test(NAME mythic_detector_tests COMMAND bean_tests)
add_test(NAME core_public_api_tests COMMAND bean_core_public_api_tests)
