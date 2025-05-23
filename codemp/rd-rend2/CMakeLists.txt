# Make sure the user is not executing this script directly
if(NOT InOpenJK)
	message(FATAL_ERROR "Use the top-level cmake script!")
endif(NOT InOpenJK)

set(MPRend2IncludeDirectories
	"${SharedDir}"
	"${SharedDir}/rd-rend2"
	"${MPDir}"
	"${MPDir}/rd-rend2"
	"${GSLIncludeDirectory}"
	)

set(MPRend2Files
	"${SharedDir}/rd-rend2/iqm.h"
	"${SharedDir}/rd-rend2/glext.h"
	"${SharedDir}/rd-rend2/qgl.h"
	"${SharedDir}/rd-rend2/stb_image.h"
	"${MPDir}/rd-rend2/G2_API.cpp"
	"${MPDir}/rd-rend2/G2_bolts.cpp"
	"${MPDir}/rd-rend2/G2_bones.cpp"
	"${SharedDir}/rd-rend2/G2_gore_r2.cpp"
	"${SharedDir}/rd-rend2/G2_gore_r2.h"
	"${MPDir}/rd-rend2/G2_misc.cpp"
	"${MPDir}/rd-rend2/G2_surfaces.cpp"
	"${SharedDir}/rd-rend2/tr_allocator.cpp"
	"${SharedDir}/rd-rend2/tr_allocator.h"
	"${SharedDir}/rd-rend2/tr_animation.cpp"
	"${SharedDir}/rd-rend2/tr_backend.cpp"
	"${SharedDir}/rd-rend2/tr_bsp.cpp"
	"${SharedDir}/rd-rend2/tr_cache.cpp"
	"${SharedDir}/rd-rend2/tr_cache.h"
	"${SharedDir}/rd-rend2/tr_cmds.cpp"
	"${SharedDir}/rd-rend2/tr_curve.cpp"
	"${SharedDir}/rd-rend2/tr_debug.cpp"
	"${SharedDir}/rd-rend2/tr_decals.cpp"
	"${SharedDir}/rd-rend2/tr_extensions.cpp"
	"${SharedDir}/rd-rend2/tr_extramath.cpp"
	"${SharedDir}/rd-rend2/tr_extramath.h"
	"${SharedDir}/rd-rend2/tr_extratypes.h"
	"${SharedDir}/rd-rend2/tr_fbo.cpp"
	"${SharedDir}/rd-rend2/tr_fbo.h"
	"${SharedDir}/rd-rend2/tr_flares.cpp"
	"${SharedDir}/rd-rend2/tr_ghoul2.cpp"
	"${SharedDir}/rd-rend2/tr_glsl.cpp"
	"${SharedDir}/rd-rend2/tr_glsl_parse.cpp"
	"${SharedDir}/rd-rend2/tr_image.cpp"
	"${SharedDir}/rd-rend2/tr_image_stb.cpp"
	"${MPDir}/rd-rend2/tr_init.cpp"
	"${SharedDir}/rd-rend2/tr_light.cpp"
	"${SharedDir}/rd-rend2/tr_local.h"
	"${SharedDir}/rd-rend2/tr_main.cpp"
	"${SharedDir}/rd-rend2/tr_marks.cpp"
	"${SharedDir}/rd-rend2/tr_mesh.cpp"
	"${SharedDir}/rd-rend2/tr_model.cpp"
	"${SharedDir}/rd-rend2/tr_model_iqm.cpp"
	"${SharedDir}/rd-rend2/tr_postprocess.cpp"
	"${SharedDir}/rd-rend2/tr_postprocess.h"
	"${SharedDir}/rd-rend2/tr_scene.cpp"
	"${SharedDir}/rd-rend2/tr_shade.cpp"
	"${SharedDir}/rd-rend2/tr_shade_calc.cpp"
	"${SharedDir}/rd-rend2/tr_shader.cpp"
	"${SharedDir}/rd-rend2/tr_shadows.cpp"
	"${SharedDir}/rd-rend2/tr_skin.cpp"
	"${SharedDir}/rd-rend2/tr_sky.cpp"
	"${SharedDir}/rd-rend2/tr_smaa.h"
	"${SharedDir}/rd-rend2/tr_subs.cpp"
	"${SharedDir}/rd-rend2/tr_surface.cpp"
	"${SharedDir}/rd-rend2/tr_tangentspace.cpp"
	"${SharedDir}/rd-rend2/tr_vbo.cpp"
	"${SharedDir}/rd-rend2/tr_world.cpp"
	"${SharedDir}/rd-rend2/tr_weather.cpp"
	"${SharedDir}/rd-rend2/tr_weather.h")
source_group("renderer" FILES ${MPRend2Files})

file(GLOB MPRend2GLSLFiles "${SharedDir}/rd-rend2/glsl/*.glsl")
source_group("renderer\\glsl" FILES ${MPRend2GLSLFiles})
set(MPRend2Files ${MPRend2Files} ${MPRend2GLSLFiles})

set(MPRend2MikktFiles
	"${SharedDir}/rd-rend2/MikkTSpace/mikktspace.h"
	"${SharedDir}/rd-rend2/MikkTSpace/mikktspace.c")
source_group("MikktSpace" FILES ${MPRend2MikktFiles})
set(MPRend2Files ${MPRend2Files} ${MPRend2MikktFiles})

set(MPRend2Ghoul2Files
	"${MPDir}/ghoul2/g2_local.h"
	"${MPDir}/ghoul2/ghoul2_shared.h"
	"${MPDir}/ghoul2/G2_gore.cpp"
	"${MPDir}/ghoul2/G2_gore.h")
source_group("ghoul2" FILES ${MPRend2Ghoul2Files})
set(MPRend2Files ${MPRend2Files} ${MPRend2Ghoul2Files})

set(MPRend2RdCommonFiles
	"${MPDir}/rd-common/mdx_format.h"
	"${MPDir}/rd-common/tr_common.h"
	"${MPDir}/rd-common/tr_font.cpp"
	"${MPDir}/rd-common/tr_font.h"
	"${MPDir}/rd-common/tr_image_load.cpp"
	"${MPDir}/rd-common/tr_image_jpg.cpp"
	"${MPDir}/rd-common/tr_image_tga.cpp"
	"${MPDir}/rd-common/tr_image_png.cpp"
	"${MPDir}/rd-common/tr_noise.cpp"
	"${MPDir}/rd-common/tr_public.h"
	"${MPDir}/rd-common/tr_types.h")
source_group("rd-common" FILES ${MPRend2RdCommonFiles})
set(MPRend2Files ${MPRend2Files} ${MPRend2RdCommonFiles})

set(MPRend2CommonFiles
	"${MPDir}/qcommon/matcomp.cpp"
	"${MPDir}/qcommon/q_shared.cpp"
	"${SharedCommonFiles}")
source_group("common" FILES ${MPRend2CommonFiles})
set(MPRend2Files ${MPRend2Files} ${MPRend2CommonFiles})

set(MPRend2CommonSafeFiles
	${SharedCommonSafeFiles}
	)
source_group("common/safe" FILES ${MPRend2CommonSafeFiles})
set(MPRend2Files ${MPRend2Files} ${MPRend2CommonSafeFiles})

# Transparently use either bundled or system libjpeg.
list(APPEND MPRend2IncludeDirectories ${JPEG_INCLUDE_DIR})
list(APPEND MPRend2Libraries          ${JPEG_LIBRARIES})

# Transparently use either bundled or system libpng.  Order is important --
# libpng used zlib, so it must come before it on the linker command line.
list(APPEND MPRend2IncludeDirectories ${PNG_INCLUDE_DIRS})
list(APPEND MPRend2Libraries          ${PNG_LIBRARIES})

# Transparently use either bundled or system zlib.
list(APPEND MPRend2IncludeDirectories ${ZLIB_INCLUDE_DIR})
list(APPEND MPRend2Libraries          ${ZLIB_LIBRARIES})

# Transparently use our bundled minizip.
list(APPEND MPRend2IncludeDirectories ${MINIZIP_INCLUDE_DIRS})
list(APPEND MPRend2Libraries          ${MINIZIP_LIBRARIES})

find_package(OpenGL REQUIRED)
set(MPRend2IncludeDirectories ${MPRend2IncludeDirectories} ${OPENGL_INCLUDE_DIR})
set(MPRend2Libraries ${MPRend2Libraries} ${OPENGL_LIBRARIES})

source_group("renderer"
	FILES
	${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.h
	${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp)
set(MPRend2Files
	${MPRend2Files}
	${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.h
	${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp)

set(MPRend2IncludeDirectories ${MPRend2IncludeDirectories} ${OpenJKLibDir})
add_library(${MPRend2} SHARED ${MPRend2Files})

if(NOT MSVC)
	# remove "lib" prefix for .so/.dylib files
	set_target_properties(${MPRend2} PROPERTIES PREFIX "")
endif()

if(WIN32)
	install(TARGETS ${MPRend2}
		RUNTIME
		DESTINATION ${JKAInstallDir}
		COMPONENT ${JKAMPClientComponent})
else(WIN32)
	if(MakeApplicationBundles AND BuildMPEngine)
		install(TARGETS ${MPRend2}
			LIBRARY
			DESTINATION "${JKAInstallDir}/${MPEngine}.app/Contents/MacOS/"
			COMPONENT ${JKAMPClientComponent})
	else()
		install(TARGETS ${MPRend2}
			LIBRARY
			DESTINATION ${JKAInstallDir}
			COMPONENT ${JKAMPClientComponent})
	endif()
endif()

set_target_properties(${MPRend2} PROPERTIES COMPILE_DEFINITIONS "${SharedDefines}")

# Hide symbols not explicitly marked public.
set_property(TARGET ${MPRend2} APPEND PROPERTY COMPILE_OPTIONS ${OPENJK_VISIBILITY_FLAGS})
set_target_properties(${MPRend2} PROPERTIES INCLUDE_DIRECTORIES "${MPRend2IncludeDirectories}")
set_target_properties(${MPRend2} PROPERTIES PROJECT_LABEL "MP Rend2 Renderer")
target_link_libraries(${MPRend2} ${MPRend2Libraries})
target_include_directories(${MPRend2} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# GLSL shader file generator
add_executable(compact_glsl
	${SharedDir}/rd-rend2/glsl/compact.cpp
	${SharedDir}/rd-rend2/tr_allocator.cpp
	${SharedDir}/rd-rend2/tr_allocator.h
	${SharedDir}/rd-rend2/tr_glsl_parse.cpp)
target_compile_definitions(compact_glsl PRIVATE "GLSL_BUILDTOOL" "NOMINMAX")
if (NOT WIN32 AND NOT APPLE)
	target_compile_definitions(compact_glsl PRIVATE "ARCH_STRING=\"${Architecture}\"")
endif()
target_include_directories(compact_glsl PRIVATE "${MPRend2IncludeDirectories}")
if (WIN32 OR APPLE)
add_custom_command(
	OUTPUT
		${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp
		${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.h
	COMMAND
		compact_glsl
			${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp
			${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.h
			${MPRend2GLSLFiles}
	DEPENDS
		compact_glsl ${MPRend2GLSLFiles})
else()
add_custom_command(
	OUTPUT
		${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp
		${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.h
	COMMAND
		${CMAKE_BINARY_DIR}/compact_glsl
			${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.cpp
			${CMAKE_CURRENT_BINARY_DIR}/glsl_shaders.h
			${MPRend2GLSLFiles}
	DEPENDS
		compact_glsl ${MPRend2GLSLFiles})
endif()
