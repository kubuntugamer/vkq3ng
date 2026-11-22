find_package(Threads REQUIRED)

# SOURCE AND HEADER FILES
# add all src files and create file hierarchy for IDE
file(GLOB_RECURSE SOURCES *.cpp *.c *.m *.mm *.hpp *.h ../qcommon/*.cpp ../qcommon/*.c ../qcommon/*.m ../qcommon/*.mm ../qcommon/*.hpp ../qcommon/*.h ../client/*.cpp ../client/*.c ../client/*.m ../client/*.mm ../client/*.hpp ../client/*.h ../server/*.cpp ../server/*.c ../server/*.m ../server/*.mm ../server/*.hpp ../server/*.h)

list(APPEND SOURCES "../game/q_math.c")
list(APPEND SOURCES "../game/bg_misc.c")
list(APPEND SOURCES "../game/q_shared.c")
list(APPEND SOURCES "../game/q_shared.h")
list(APPEND SOURCES "../ui/ui_syscalls.c")

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

#find_package(OpenGL REQUIRED)
#find_package(GLEW REQUIRED)

# PipeWire audio backend (replaces the obsolete OSS /dev/dsp backend)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
endif()
if(PIPEWIRE_FOUND)
    message(STATUS "Found PipeWire ${PIPEWIRE_VERSION}; using it for audio")
    list(FILTER SOURCES EXCLUDE REGEX "/linux_snd\\.c$")
    include_directories(${PIPEWIRE_INCLUDE_DIRS})
    link_directories(${PIPEWIRE_LIBRARY_DIRS})
else()
    message(STATUS "PipeWire not found; falling back to OSS audio")
endif()

# Native Wayland backend (xdg-shell + relative pointer + pointer constraints)
if(PkgConfig_FOUND)
    pkg_check_modules(WAYLAND QUIET wayland-client wayland-cursor)
    pkg_check_modules(XKBCOMMON QUIET xkbcommon)
endif()
if(WAYLAND_FOUND AND XKBCOMMON_FOUND)
    pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    if(WAYLAND_SCANNER AND EXISTS "${WAYLAND_PROTOCOLS_DIR}")
        string(REPLACE "//" "/" WAYLAND_PROTOCOLS_DIR "${WAYLAND_PROTOCOLS_DIR}")
        set(WL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols)
        file(MAKE_DIRECTORY ${WL_GEN_DIR})
        set(WL_XMLS
            ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
            ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
            ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)
        foreach(XML ${WL_XMLS})
            get_filename_component(NAME ${XML} NAME_WE)
            set(HDR ${WL_GEN_DIR}/${NAME}-client-protocol.h)
            set(SRC ${WL_GEN_DIR}/${NAME}-protocol.c)
            add_custom_command(OUTPUT ${HDR}
                COMMAND ${WAYLAND_SCANNER} client-header ${XML} ${HDR}
                DEPENDS ${XML}
                COMMENT "Generating Wayland ${NAME} header")
            add_custom_command(OUTPUT ${SRC}
                COMMAND ${WAYLAND_SCANNER} private-code ${XML} ${SRC}
                DEPENDS ${XML}
                COMMENT "Generating Wayland ${NAME} protocol code")
            list(APPEND WAYLAND_GENERATED ${HDR} ${SRC})
        endforeach()
        list(APPEND SOURCES ${WAYLAND_GENERATED})
        include_directories(${WL_GEN_DIR} ${WAYLAND_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
        link_directories(${WAYLAND_LIBRARY_DIRS} ${XKBCOMMON_LIBRARY_DIRS})
        set(HAVE_WAYLAND TRUE)
        message(STATUS "Found Wayland + xkbcommon; native Wayland backend enabled")
    endif()
endif()

# INCLUDE
include_directories(${OPENGL_INCLUDE_DIR})
include_directories(${X11_INCLUDE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/code/renderer)
include_directories(${CMAKE_SOURCE_DIR}/code/qcommon)
include_directories(${CMAKE_SOURCE_DIR}/code/client)
include_directories(${CMAKE_SOURCE_DIR}/code/botlib)

# EXECUTABLE
add_executable(${APP_UNIX} ${SOURCES})
target_link_libraries(${APP_UNIX} "${OPENGL_LIBRARIES}")
target_link_libraries(${APP_UNIX} "${X11_LIBRARIES}")
target_link_libraries(${APP_UNIX} "${CMAKE_DL_LIBS}")
find_library(XXF86DGA_LIBRARY NAMES Xxf86dga)
if(XXF86DGA_LIBRARY)
    target_link_libraries(${APP_UNIX} "${XXF86DGA_LIBRARY}")
endif()
target_link_libraries(${APP_UNIX} "-lm -lXxf86vm -lXext -lX11")
target_link_libraries(${APP_UNIX} Threads::Threads)
target_compile_definitions(${APP_UNIX} PRIVATE SMP=1)
target_link_libraries(${APP_UNIX} ${LIB_BOTLIB})
target_link_libraries(${APP_UNIX} ${LIB_RENDERER})
target_link_libraries(${APP_UNIX} ${LIB_SPLINES})
if(PIPEWIRE_FOUND)
    target_compile_definitions(${APP_UNIX} PRIVATE USE_PIPEWIRE=1)
    target_link_libraries(${APP_UNIX} ${PIPEWIRE_LIBRARIES})
endif()
if(HAVE_WAYLAND)
    target_compile_definitions(${APP_UNIX} PRIVATE HAVE_WAYLAND=1)
    target_link_libraries(${APP_UNIX} ${WAYLAND_LIBRARIES} ${XKBCOMMON_LIBRARIES})
endif()

# PROPERTIES
# when Quake III Arena was installed with GOG
set_target_properties(${APP_UNIX} PROPERTIES LINKER_LANGUAGE C)
IF(Q3_ARGUMENTS)
    #set_target_properties(${APP_WIN32} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS ${Q3_ARGUMENTS})
endif()
IF(BASEQ3_DIR)
    #set_target_properties(${APP_WIN32} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS ${BASEQ3_DIR})
endif()

# DEPENDENCIES
add_dependencies(${APP_UNIX} ${LIB_RENDERER} ${LIB_BOTLIB} ${LIB_SPLINES} ${LIB_GAME} ${LIB_CGAME} ${LIB_Q3UI})
