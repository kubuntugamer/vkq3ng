# SOURCE AND HEADER FILES
# add all src files and create file hierarchy for IDE
file(GLOB_RECURSE SOURCES *.cpp *.c *.m *.mm *.hpp *.h)

list(APPEND SOURCES "../game/q_math.c")

foreach(FILE ${SOURCES}) 
    # Get the directory of the source file
    get_filename_component(PARENT_DIR "${FILE}" DIRECTORY)

    # Remove common directory prefix to make the group
    string(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}" "" GROUP "${PARENT_DIR}")

    # Make sure we are using windows slashes
    string(REPLACE "/" "\\" GROUP "${GROUP}")

    # Group into "Source Files" and "Header Files"
    if ("${FILE}" MATCHES ".*\\.\(cpp|c|m\)")
       set(GROUP "Source Files${GROUP}")
    elseif("${FILE}" MATCHES ".*\\.\(hpp|h\)")
       set(GROUP "Header Files${GROUP}")
    endif()

    source_group("${GROUP}" FILES "${FILE}")
endforeach()

include_directories(${VULKAN_INCLUDE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/external/stb)
include_directories(${CMAKE_SOURCE_DIR}/external/TinyJPEG)
if(APPLE)
    include_directories(${CMAKE_SOURCE_DIR}/code/macos)
else(UNIX)
    add_definitions(-D__linux__ -D__linux)
endif()

# LIB
add_library(${LIB_RENDERER} STATIC ${SOURCES})
target_link_libraries(${LIB_RENDERER} "${OPENGL_LIBRARIES}")
target_link_libraries(${LIB_RENDERER} Vulkan::Vulkan)
if(UNIX AND NOT APPLE)
    # NVIDIA NGX loader shim (dlopen()s the installed driver library at runtime).
    target_include_directories(${LIB_RENDERER} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/ngx/include")
    target_link_libraries(${LIB_RENDERER} "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/ngx/lib/Linux_x86_64/libnvsdk_ngx.a" dl stdc++)
    # AMD FidelityFX FSR2 is C++ and uses the portable (non-MSVC) path.
    target_compile_definitions(${LIB_RENDERER} PRIVATE FFX_GCC=1)
    # Front-end/back-end render thread (classic Quake III SMP path).
    target_compile_definitions(${LIB_RENDERER} PRIVATE SMP=1)
    set_target_properties(${LIB_RENDERER} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endif()
set_target_properties(${LIB_RENDERER} PROPERTIES LINKER_LANGUAGE C)
