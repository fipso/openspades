
set(OS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB AUDIO_FILES Audio/*.cpp Audio/*.h)
file(GLOB AUDIO_AL_FILES Audio/AL/*.cpp Audio/AL/*.h)
set(BINPACK_FILES binpack2d/binpack2d.hpp)
file(GLOB CLIENT_FILES Client/*.cpp Client/*.h)
file(GLOB CORE_FILES Core/*.c Core/*.cpp Core/*.h)
file(GLOB DRAW_FILES Draw/*.cpp Draw/*.h)
file(GLOB ENET_FILES ENet/*.c ENet/*.h)
file(GLOB ENET_INCLUDE ENet/include/enet/*.h)
file(GLOB GUI_FILES Gui/*.cpp Gui/*.h)
file(GLOB IMPORTS_FILES Imports/*.h)
file(GLOB KISS_FILES kiss_fft130/*.c kiss_fft130/*.h)
file(GLOB JSON_FILES json/*.cpp json/*.h json/*.inl)
file(GLOB JSON_INCLUDE json/include/json/*.h)
file(GLOB POLY2TRI_FILES poly2tri/*.cc poly2tri/*.h)
file(GLOB POLY2TRI_COMMON_FILES poly2tri/common/*.cc poly2tri/common/*.h)
file(GLOB POLY2TRI_SWEEP_FILES poly2tri/sweep/*.cc poly2tri/sweep/*.h)
file(GLOB SCRIPTBINDING_FILES ScriptBindings/*.cpp ScriptBindings/*.h)
file(GLOB UNZIP_FILES unzip/*.c unzip/*.h)

add_subdirectory(AngelScript/projects/cmake)
add_subdirectory(AngelScript/projects/cmake_addons)
set(ANGELSCRIPT_LIBS Angelscript Angelscript_addons)

include_directories(BEFORE ".")
include_directories(BEFORE "json/include")
include_directories(BEFORE "ENet/include")

if(WIN32)
	if(MINGW)	# = gcc, can someone confirm that this works?
		add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/OpenSpades_rc.o COMMAND windres.exe -I${CMAKE_CURRENT_BINARY_DIR}
			-i${CMAKE_CURRENT_BINARY_DIR}/OpenSpades.rc -o ${CMAKE_CURRENT_BINARY_DIR}/OpenSpades_rc.o)
		SET(RESOURCE_FILES ${CMAKE_CURRENT_BINARY_DIR}/OpenSpades_rc.o)
	else()
		set(RESOURCE_FILES ${PROJECT_BINARY_DIR}/OpenSpades.rc)	# msvc
	endif()
endif()

add_executable(OpenSpades ${AUDIO_FILES} ${AUDIO_AL_FILES} ${BINPACK_FILES} ${CLIENT_FILES} ${CORE_FILES} ${DRAW_FILES} ${ENET_FILES} ${ENET_INCLUDE} ${GUI_FILES}
	${IMPORTS_FILES} ${KISS_FILES} ${JSON_FILES} ${JSON_INCLUDE} ${POLY2TRI_COMMON_FILES} ${POLY2TRI_SWEEP_FILES} ${UNZIP_FILES} ${SCRIPTBINDING_FILES} ${RESOURCE_FILES})
set_target_properties(OpenSpades PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(OpenSpades PROPERTIES OUTPUT_NAME openspades)
set_target_properties(OpenSpades PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_target_properties(OpenSpades PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_target_properties(OpenSpades PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_dependencies(OpenSpades Angelscript Angelscript_addons)

if(WIN32)
	source_group("Resources" ${RESOURCE_FILES})
	foreach(LIB ${SDL2_LIBRARY} ${SDL2_IMAGE_LIBRARY})
		string(REGEX REPLACE "\\.lib$" ".dll" SDL_DLL ${LIB})
		if(EXISTS "${SDL_DLL}")
			add_custom_command(TARGET OpenSpades POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL_DLL}" "${CMAKE_BINARY_DIR}/bin/\$\(Configuration\)/")
			install(PROGRAMS ${SDL_DLL} DESTINATION bin)
		endif()
	endforeach()
	add_definitions(-DCURL_STATICLIB)
	get_filename_component(GLEWLIBFILE ${GLEW_LIBRARY} NAME_WE)
	if("glew32s" STREQUAL ${GLEWLIBFILE})
		add_definitions(-DGLEW_STATIC)	#static linked
	endif()
	if(MSVC)
		#lm: this sucks, msvc fucks up some linking (conflicts)
		set_target_properties(OpenSpades PROPERTIES LINK_FLAGS_RELEASE "/NODEFAULTLIB:\"libcmt.lib\" /NODEFAULTLIB:\"libcmtd.lib\"")
	endif()
endif()

source_group("Audio" FILES ${AUDIO_FILES})
source_group("Audio\\AL" FILES ${AUDIO_AL_FILES})
source_group("libs\\binpack2d" FILES ${BINPACK_FILES})
source_group("Client" FILES ${CLIENT_FILES})
source_group("Core" FILES ${CORE_FILES})
source_group("Draw" FILES ${DRAW_FILES})
source_group("libs\\Enet" FILES ${ENET_FILES})
source_group("libs\\Enet\\include" FILES ${ENET_INCLUDE})
source_group("Gui" FILES ${GUI_FILES})
source_group("Imports" FILES ${IMPORTS_FILES})
source_group("libs\\kiss_fft130" FILES ${KISS_FILES})
source_group("libs\\json" FILES ${JSON_FILES})
source_group("libs\\json\\include" FILES ${JSON_INCLUDE})
source_group("libs\\poly2tri" FILES ${POLY2TRI_FILES})
source_group("libs\\poly2tri\\common" FILES ${POLY2TRI_COMMON_FILES})
source_group("libs\\poly2tri\\sweep" FILES ${POLY2TRI_SWEEP_FILES})
source_group("ScriptBindings" FILES ${SCRIPTBINDING_FILES})
source_group("libs\\unzip" FILES ${UNZIP_FILES})

target_link_libraries(OpenSpades ${SDL2_LIBRARY} ${SDL2_IMAGE_LIBRARY} ${OPENGL_LIBRARIES} ${GLEW_LIBRARY} ${ZLIB_LIBRARIES} ${CURL_LIBRARY} ${CMAKE_DL_LIBS} ${ANGELSCRIPT_LIBS})

#todo: MACOSX_BUNDLE_ICON_FILE ?

if(OPENAL_FOUND)
	target_link_libraries(OpenSpades ${OPENAL_LIBRARY})
	include_directories(${OPENAL_INCLUDE_DIRS})
endif()

if(WIN32)
	target_link_libraries(OpenSpades ws2_32.lib winmm.lib)
else()
	target_link_libraries(OpenSpades Xext)
endif()

if(UNIX)
	target_link_libraries(OpenSpades rt)
endif()

#install(TARGETS OpenSpades DESTINATION bin)
