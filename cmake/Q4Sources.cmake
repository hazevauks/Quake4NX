# What goes into each part of the build. Upstream's tools/build/meson_sources.py is the model.

function(q4_glob out)
	set(result "")
	foreach(pattern IN LISTS ARGN)
		file(GLOB found CONFIGURE_DEPENDS ${SRC}/${pattern})
		list(APPEND result ${found})
	endforeach()
	set(${out} ${result} PARENT_SCOPE)
endfunction()

# --- idlib: one copy in the engine and one in each game module
q4_glob(Q4_IDLIB_SOURCES
	idlib/*.cpp idlib/bv/*.cpp idlib/containers/*.cpp idlib/geometry/*.cpp idlib/hashing/*.cpp idlib/math/*.cpp)
list(FILTER Q4_IDLIB_SOURCES EXCLUDE REGEX "Simd_AltiVec[.]cpp$")

# --- game modules
file(GLOB_RECURSE Q4_GAME_SP_SOURCES CONFIGURE_DEPENDS ${SRC}/game/*.cpp)
list(FILTER Q4_GAME_SP_SOURCES EXCLUDE REGEX "/game/(gamesys/)?Callbacks[.]cpp$")
file(GLOB_RECURSE Q4_GAME_MP_SOURCES CONFIGURE_DEPENDS ${SRC}/mpgame/*.cpp)
list(FILTER Q4_GAME_MP_SOURCES EXCLUDE REGEX "/mpgame/(gamesys/)?Callbacks[.]cpp$")

# --- effects
file(GLOB_RECURSE Q4_BSE_SOURCES CONFIGURE_DEPENDS ${SRC}/bse/*.cpp)

# --- engine
q4_glob(Q4_ENGINE_SOURCES
	aas/*.cpp cm/*.cpp framework/*.cpp framework/async/*.cpp
	sound/*.cpp sound/OpenAL/*.cpp sys/*.cpp ui/*.cpp
	tools/compilers/aas/*.cpp tools/compilers/dmap/*.cpp tools/leveleditor/*.cpp
	rendergeo/*.cpp imagetools/*.cpp imagetools/Color/*.cpp imagetools/DXT/*.cpp imagetools/ETC/*.cpp
	external/jpeg-6/*.c external/stb_vorbis/*.c)
list(APPEND Q4_ENGINE_SOURCES
	${SRC}/ui/application/SystemSettingsHost.cpp
	${SRC}/ui/application/SystemDisplay.cpp
	${SRC}/sys/sdl3/TextProvenanceIngress.cpp
	${SRC}/sys/sdl3/TextClipboard.cpp
)

# --- renderer: the shared front end, without the OpenGL back end
q4_glob(Q4_RENDERER_SOURCES renderer/*.cpp)
list(FILTER Q4_RENDERER_SOURCES EXCLUDE REGEX "/renderer/(RendererGLModule|draw_arb2|draw_common|draw_pbr|draw_exp_stub|tr_backend|tr_render|ModernGLExecutor|ModernGLShaderLibrary|ModernLightImageAtlas|ModernSpecularProbeAtlas|RenderGraphResources|GLStateCache|GLDebugScope|RendererUpload)[.]cpp$")

# --- platform
q4_glob(Q4_PLATFORM_SOURCES sys/switch/*.cpp renderer/deko3d/*.cpp)

# --- UI runtime
set(Q4_RETAINED_SOURCES "")
foreach(name
	retained/Runtime retained/Document retained/DocumentEdit retained/Presentation retained/Behavior retained/State
	retained/Motion retained/Interaction retained/ValueControlView retained/NumberControlView retained/Input
	retained/TextInput retained/TextEdit retained/TextEditCommand retained/TextInputBroker retained/TextRun
	retained/Vector retained/VectorElement retained/HudElement retained/TextInputBrokerWire retained/ModelScene
	retained/PlayerSetupRequest retained/NativeTextDocument retained/NativeTextEditor retained/ScrollGeometry
	retained/ScrollbarView retained/PopupPlacement
	application/SettingsTransaction application/SettingsJournal application/SettingsEffectPlan
	application/SettingsDisplayController)
	list(APPEND Q4_RETAINED_SOURCES ${SRC}/ui/${name}.cpp)
endforeach()

# RmlUi's core only: the engine brings its own font engine and back end
file(GLOB_RECURSE Q4_RMLUI_SOURCES CONFIGURE_DEPENDS ${THIRD_PARTY}/rmlui/Source/Core/*.cpp)
set(Q4_JSONCPP_SOURCES
	${THIRD_PARTY}/jsoncpp/src/lib_json/json_reader.cpp
	${THIRD_PARTY}/jsoncpp/src/lib_json/json_value.cpp
	${THIRD_PARTY}/jsoncpp/src/lib_json/json_writer.cpp)
set(Q4_LIBTESS2_SOURCES
	${THIRD_PARTY}/libtess2/Source/bucketalloc.c
	${THIRD_PARTY}/libtess2/Source/dict.c
	${THIRD_PARTY}/libtess2/Source/geom.c
	${THIRD_PARTY}/libtess2/Source/mesh.c
	${THIRD_PARTY}/libtess2/Source/priorityq.c
	${THIRD_PARTY}/libtess2/Source/sweep.c
	${THIRD_PARTY}/libtess2/Source/tess.c)
