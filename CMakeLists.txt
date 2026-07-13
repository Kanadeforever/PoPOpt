cmake_minimum_required(VERSION 3.20)
project(PoPOpt VERSION 0.29.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "PoPOpt targets 32-bit Windows only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure a Win32/x86 build. Prince of Persia (2008) is 32-bit.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB REQUIRED)

set(POPOPT_SOURCES
    src/Main.cpp
    src/core/Core.cpp
    src/core/PeUtils.cpp
    src/core/LanguageIds.cpp
    src/modules/SettingsRegistryModule.cpp
    src/modules/VoiceModule.cpp
    src/modules/DisplayModule.cpp
    src/modules/DpiModule.cpp
    src/modules/CpuModule.cpp
    src/modules/InputModule.cpp
    src/modules/GraphicsModule.cpp
    src/modules/TextureLoaderModule.cpp
    src/modules/DiagnosticsModule.cpp
    src/texture/TpfArchive.cpp
)

add_library(PoPOpt SHARED ${POPOPT_SOURCES})
target_include_directories(PoPOpt PRIVATE src)
target_compile_definitions(PoPOpt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(PoPOpt PRIVATE user32 advapi32 ZLIB::ZLIB)
set_target_properties(PoPOpt PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "PoP_UniversalPatch"
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES ${POPOPT_SOURCES})

if(MSVC)
    set_property(TARGET PoPOpt PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(PoPOpt PRIVATE /W4 /EHsc /permissive-)
    target_link_options(PoPOpt PRIVATE /MACHINE:X86)
elseif(MINGW)
    target_compile_options(PoPOpt PRIVATE -Wall -Wextra -Wno-cast-function-type)
    target_link_options(PoPOpt PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
endif()

add_custom_command(TARGET PoPOpt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/PoP_UniversalPatch.ini"
            "$<TARGET_FILE_DIR:PoPOpt>/PoP_UniversalPatch.ini"
    COMMENT "Copying default configuration beside the ASI"
)

install(TARGETS PoPOpt RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES config/PoP_UniversalPatch.ini DESTINATION .)
