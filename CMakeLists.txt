cmake_minimum_required(VERSION 3.16)

project(Wake2Text VERSION 1.0.0 LANGUAGES CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set build type to Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Add compiler flags for Windows
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601)
    add_compile_definitions(NOMINMAX)
    if(MSVC)
        add_compile_options(/utf-8)
        # Enable parallel compilation
        add_compile_options(/MP)
    endif()
endif()

# Find required packages
find_package(PkgConfig)

# Try to find PulseAudio (optional, mainly for Linux)
if(PKG_CONFIG_FOUND AND NOT WIN32)
    pkg_check_modules(PULSEAUDIO libpulse-simple)
    if(PULSEAUDIO_FOUND)
        add_compile_definitions(HAVE_PULSEAUDIO)
    endif()
endif()

# Windows audio support
if(WIN32)
    set(AUDIO_LIBS winmm)
    add_compile_definitions(HAVE_WINMM)
else()
    set(AUDIO_LIBS "")
    if(PULSEAUDIO_FOUND)
        list(APPEND AUDIO_LIBS ${PULSEAUDIO_LIBRARIES})
    endif()
endif()

# Add cblas include path for snowman
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Add whisper.cpp subdirectory
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "Don't build whisper tests")
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "Don't build whisper examples")
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "Don't build whisper server")
add_subdirectory(whisper.cpp)

# Add snowman subdirectory
set(SNOWMAN_BUILD_APPS OFF CACHE BOOL "Don't build snowman apps")
set(SNOWMAN_BUILD_TESTS OFF CACHE BOOL "Don't build snowman tests")
set(SNOWMAN_BUILD_WASM OFF CACHE BOOL "Don't build snowman wasm")
add_subdirectory(snowman)

# Snowman include directories
set(SNOWMAN_INCLUDE_DIRS 
    ${CMAKE_CURRENT_SOURCE_DIR}/snowman/apps
    ${CMAKE_CURRENT_SOURCE_DIR}/snowman/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/snowman
)

# Create helper library for snowman apps code we need
file(GLOB SNOWMAN_APP_SOURCES 
    "snowman/apps/helper.cpp"
    "snowman/apps/pulseaudio_win.cpp"
    "snowman/apps/snowboy_compat.cpp"
)

add_library(snowman_helper STATIC 
    ${SNOWMAN_APP_SOURCES}
)

target_include_directories(snowman_helper PUBLIC ${SNOWMAN_INCLUDE_DIRS})

# Platform-specific settings for snowman helper
if(WIN32)
    target_compile_definitions(snowman_helper PRIVATE
        _CRT_SECURE_NO_WARNINGS
        HAVE_WINMM
    )
    target_link_libraries(snowman_helper PRIVATE winmm)
elseif(PULSEAUDIO_FOUND)
    target_compile_definitions(snowman_helper PRIVATE HAVE_PULSEAUDIO)
    target_include_directories(snowman_helper PRIVATE ${PULSEAUDIO_INCLUDE_DIRS})
    target_link_libraries(snowman_helper PRIVATE ${PULSEAUDIO_LIBRARIES})
endif()

# Create the main executable
add_executable(wake2text
    src/main.cpp
)

# Link libraries
target_link_libraries(wake2text PRIVATE
    whisper
    snowman
    snowman_helper
    ${AUDIO_LIBS}
)

# Include directories
target_include_directories(wake2text PRIVATE
    ${SNOWMAN_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/whisper.cpp/include
    src
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(wake2text PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        _WIN32_WINNT=0x0601
    )
    target_link_libraries(wake2text PRIVATE
        winmm
        kernel32
        user32
        shell32
        advapi32
    )
    
    # Post-build step to copy DLLs to the executable directory
    add_custom_command(TARGET wake2text POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:whisper>"
            "$<TARGET_FILE_DIR:wake2text>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ggml>"
            "$<TARGET_FILE_DIR:wake2text>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ggml-base>"
            "$<TARGET_FILE_DIR:wake2text>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ggml-cpu>"
            "$<TARGET_FILE_DIR:wake2text>"
        COMMENT "Copying required DLLs to executable directory"
    )
endif()

# Create resources directory structure
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/resources/pmdl)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/models)

# Copy any existing resource files to build directory
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/resources 
         DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()

# Installation
install(TARGETS wake2text
    RUNTIME DESTINATION bin
)

# Install resources if they exist
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources)
    install(DIRECTORY resources/
        DESTINATION share/wake2text/resources
    )
endif()

# Print configuration summary
message(STATUS "Wake2Text Configuration Summary:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
if(WIN32)
    message(STATUS "  Audio backend: WinMM")
elseif(PULSEAUDIO_FOUND)
    message(STATUS "  Audio backend: PulseAudio")
else()
    message(STATUS "  Audio backend: None found")
endif()
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
