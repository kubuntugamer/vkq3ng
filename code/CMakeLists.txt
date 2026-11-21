
if(UNIX AND NOT APPLE)
	# qvk.h defines Vulkan function pointers in every translation unit
	add_compile_options(-fcommon)
endif()

# STATIC LIB
add_subdirectory(botlib)
add_subdirectory(renderer)
add_subdirectory(splines)

# DYNAMIC LIB
add_subdirectory(cgame)
add_subdirectory(game)
add_subdirectory(q3_ui)
add_subdirectory(ui)

# EXECUTABLE
if(WIN32)
	add_subdirectory(win32)
elseif(APPLE)
	add_subdirectory(macos)
else(UNIX)
	add_subdirectory(unix)
endif()

# tools
add_subdirectory(tools)