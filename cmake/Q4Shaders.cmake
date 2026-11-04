# The renderer's own shaders: GLSL compiled with uam into the NRO's romfs.
set(Q4_ROMFS_DIR ${CMAKE_BINARY_DIR}/romfs)
file(MAKE_DIRECTORY ${Q4_ROMFS_DIR}/shaders)

file(GLOB Q4_SHADER_SOURCES CONFIGURE_DEPENDS
	${CMAKE_SOURCE_DIR}/shaders/*.vert
	${CMAKE_SOURCE_DIR}/shaders/*.frag
)

set(Q4_SHADER_OUTPUTS "")
foreach(source IN LISTS Q4_SHADER_SOURCES)
	get_filename_component(name ${source} NAME)
	get_filename_component(stage ${source} LAST_EXT)
	string(SUBSTRING ${stage} 1 -1 stage)
	set(output ${Q4_ROMFS_DIR}/shaders/${name}.dksh)
	add_custom_command(
		OUTPUT ${output}
		COMMAND ${NX_UAM_EXE} -s ${stage} -o ${output} ${source}
		DEPENDS ${source}
		COMMENT "Compiling shader ${name}"
		VERBATIM
	)
	list(APPEND Q4_SHADER_OUTPUTS ${output})
endforeach()

add_custom_target(q4_shaders DEPENDS ${Q4_SHADER_OUTPUTS})

dkp_add_asset_target(q4_romfs ${Q4_ROMFS_DIR})
add_dependencies(q4_romfs q4_shaders)
set_property(TARGET q4_romfs APPEND PROPERTY DKP_ASSET_FILES ${Q4_SHADER_OUTPUTS})
