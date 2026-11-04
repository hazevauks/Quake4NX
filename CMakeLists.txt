cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{DEVKITPRO})
	set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Switch.cmake")
endif()

project(Quake4NX VERSION 0.1.0 LANGUAGES C CXX ASM)

if(NOT NINTENDO_SWITCH)
	message(FATAL_ERROR "Quake4NX targets the Nintendo Switch: configure it with the devkitA64 toolchain.")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

option(Q4NX_LTO "Link-time optimization (slower links, faster game)" OFF)
option(Q4NX_DEKO3D_DEBUG "Link deko3D's debug library, which reports errors instead of ending the process" ON)

set(SRC ${CMAKE_SOURCE_DIR}/src)
set(THIRD_PARTY ${CMAKE_SOURCE_DIR}/third_party)

# id's code leans on type punning, and frame pointers keep the crash reports useful.
add_compile_options(
	-fsigned-char		# as on the x86 PCs the game was written for; plain char is unsigned on ARM
	-fno-strict-aliasing
	-fno-omit-frame-pointer
	-w
	"$<$<COMPILE_LANGUAGE:CXX>:-fpermissive>"
	"$<$<COMPILE_LANGUAGE:CXX>:-fexceptions>"
	"$<$<COMPILE_LANGUAGE:CXX>:-frtti>"
)
add_compile_definitions(USE_OPENAL GLEW_NO_GLU)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(Q4NX_OPT_FLAGS "-O3 -DNDEBUG" CACHE STRING "Optimization flags; -O0 makes a quick check of the whole source tree")
set(CMAKE_C_FLAGS_RELEASE "${Q4NX_OPT_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "${Q4NX_OPT_FLAGS}")

if(Q4NX_LTO)
	add_compile_options(-flto=auto -fno-fat-lto-objects)
	add_link_options(-flto=auto)
endif()

include(cmake/Q4Sources.cmake)
include(cmake/Q4GameObject.cmake)
include(cmake/Q4Shaders.cmake)

# Headers the upstream build generates: fixed here, since they describe the port.
set(Q4_GENERATED_INCLUDE ${CMAKE_SOURCE_DIR}/src/sys/switch/generated)
include_directories(${Q4_GENERATED_INCLUDE})
add_compile_options("SHELL:-include switch_compat.h")

# ---------------------------------------------------------------------------
# The UI runtime and its libraries, kept apart from the engine's headers.
# ---------------------------------------------------------------------------
add_library(q4_retained STATIC ${Q4_RETAINED_SOURCES} ${Q4_RMLUI_SOURCES} ${Q4_JSONCPP_SOURCES} ${Q4_LIBTESS2_SOURCES})
target_include_directories(q4_retained PUBLIC
	${CMAKE_SOURCE_DIR}
	${THIRD_PARTY}/rmlui/Include
	${THIRD_PARTY}/jsoncpp/include
	${THIRD_PARTY}/libtess2/Include
)
target_compile_definitions(q4_retained PUBLIC RMLUI_STATIC_LIB RMLUI_NO_THIRDPARTY_CONTAINERS PRIVATE RMLUI_VERSION="6.3")

# ---------------------------------------------------------------------------
# The two game modules. Each has its own copy of idlib and of the engine's
# interface globals, as on PC. See cmake/Q4GameObject.cmake.
# ---------------------------------------------------------------------------
foreach(flavour SP MP)
	string(TOLOWER ${flavour} lower)
	add_library(q4_game_${lower}_code OBJECT ${Q4_GAME_${flavour}_SOURCES} ${Q4_IDLIB_SOURCES})
	target_include_directories(q4_game_${lower}_code PRIVATE
		${CMAKE_SOURCE_DIR} ${SRC} ${SRC}/idlib ${SRC}/renderer ${SRC}/ui ${SRC}/sys ${SRC}/bse ${SRC}/mayaimport
		${Q4_GENERATED_INCLUDE})
	target_compile_definitions(q4_game_${lower}_code PRIVATE GAME_DLL)
	set_target_properties(q4_game_${lower}_code PROPERTIES CXX_STANDARD 17)
	target_precompile_headers(q4_game_${lower}_code PRIVATE <idlib/precompiled.h>)
endforeach()
target_include_directories(q4_game_sp_code PRIVATE ${SRC}/game)
target_include_directories(q4_game_mp_code PRIVATE ${SRC}/mpgame ${SRC}/game)
target_compile_definitions(q4_game_mp_code PRIVATE GAME_MPAPI)
q4_add_game_object(q4_game_sp_object q4_game_sp_code GetGameAPI_SP)
q4_add_game_object(q4_game_mp_object q4_game_mp_code GetGameAPI_MP)

# ---------------------------------------------------------------------------
# The effects system, built without the engine's own defines, as upstream does.
# ---------------------------------------------------------------------------
add_library(q4_bse OBJECT ${Q4_BSE_SOURCES})
target_include_directories(q4_bse PRIVATE ${CMAKE_SOURCE_DIR} ${SRC} ${SRC}/bse ${SRC}/game ${Q4_GENERATED_INCLUDE})
target_precompile_headers(q4_bse PRIVATE <idlib/precompiled.h>)

# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------
# the C libraries, apart: a precompiled header is per language
set(Q4_ENGINE_C_SOURCES ${Q4_ENGINE_SOURCES})
list(FILTER Q4_ENGINE_C_SOURCES INCLUDE REGEX "[.]c$")
list(FILTER Q4_ENGINE_SOURCES EXCLUDE REGEX "[.]c$")
add_library(q4_engine_c OBJECT ${Q4_ENGINE_C_SOURCES} ${SRC}/external/glew/glew.c)
target_include_directories(q4_engine_c PRIVATE ${SRC})
set_source_files_properties(${SRC}/external/glew/glew.c PROPERTIES COMPILE_DEFINITIONS "OPENQ4_GLEW_SDL3_LOADER;GLAPI=extern")

add_library(q4_engine_code OBJECT ${Q4_ENGINE_SOURCES} ${Q4_RENDERER_SOURCES} ${Q4_IDLIB_SOURCES} ${Q4_PLATFORM_SOURCES})
target_include_directories(q4_engine_code PRIVATE
	${CMAKE_SOURCE_DIR} ${SRC} ${SRC}/game ${Q4_GENERATED_INCLUDE} ${SRC}/external/stb_vorbis
	${THIRD_PARTY}/rmlui/Include ${THIRD_PARTY}/jsoncpp/include ${THIRD_PARTY}/libtess2/Include
	${DEVKITPRO}/portlibs/switch/include)
target_compile_definitions(q4_engine_code PRIVATE __DOOM_DLL__ OPENQ4_RENDERER_DK RMLUI_STATIC_LIB RMLUI_NO_THIRDPARTY_CONTAINERS)
target_precompile_headers(q4_engine_code PRIVATE <idlib/precompiled.h>)

add_custom_target(q4_compile_all DEPENDS q4_retained q4_game_sp_code q4_game_mp_code q4_bse q4_engine_c q4_engine_code)

# ---------------------------------------------------------------------------
# The executable
# ---------------------------------------------------------------------------
add_executable(quake4
	$<TARGET_OBJECTS:q4_engine_code>
	$<TARGET_OBJECTS:q4_engine_c>
	$<TARGET_OBJECTS:q4_bse>
	${q4_game_sp_object_FILE}
	${q4_game_mp_object_FILE}
)
target_link_options(quake4 PRIVATE
	-Wl,--wrap=pthread_create
	-Wl,-Map=${CMAKE_BINARY_DIR}/quake4.map
	-Wl,--gc-sections
)
if(Q4NX_DEKO3D_DEBUG)
	set(Q4_DEKO3D_LIBRARY deko3dd)
else()
	set(Q4_DEKO3D_LIBRARY deko3d)
endif()
target_link_libraries(quake4 PRIVATE q4_retained ${Q4_DEKO3D_LIBRARY} openal z nx m)
add_dependencies(quake4 q4_game_sp_object_target q4_game_mp_object_target q4_shaders)

nx_generate_nacp(quake4.nacp
	NAME "Quake 4"
	AUTHOR "hazevauks"
	VERSION "${PROJECT_VERSION}"
)
nx_create_nro(quake4
	NACP quake4.nacp
	ICON ${CMAKE_SOURCE_DIR}/assets/icon.jpg
	ROMFS q4_romfs
)
