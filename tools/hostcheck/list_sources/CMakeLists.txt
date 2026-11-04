# Writes out cmake/Q4Sources.cmake's lists, so that tools/hostcheck/build.py builds the same files as the console.
cmake_minimum_required(VERSION 3.24)
project(Q4NXListSources NONE)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(SRC ${ROOT}/src)
set(THIRD_PARTY ${ROOT}/third_party)
include(${ROOT}/cmake/Q4Sources.cmake)

set(text "")
foreach(name Q4_IDLIB_SOURCES Q4_GAME_SP_SOURCES Q4_GAME_MP_SOURCES Q4_BSE_SOURCES Q4_ENGINE_SOURCES
		Q4_RENDERER_SOURCES Q4_PLATFORM_SOURCES Q4_RETAINED_SOURCES Q4_RMLUI_SOURCES Q4_JSONCPP_SOURCES Q4_LIBTESS2_SOURCES)
	foreach(file IN LISTS ${name})
		string(APPEND text "${name} ${file}\n")
	endforeach()
endforeach()
file(WRITE ${OUTPUT} "${text}")
