cmake_minimum_required(VERSION 3.12)
project(tereix VERSION 1.0 LANGUAGES C)

option(USE_CCACHE "Try to use ccache for faster builds" OFF)
option(USE_LTO "Use linktime optimizations if available" ON)
option(MARCH_NATIVE "Build library optimized for native arch" OFF)
option(DO_INSTALL "Try to install library after it is build" OFF)
set(USE_CACHE_COMPRESSION "AUTO" CACHE STRING "Compress AST cache using zstd (ON, OFF, AUTO)")

add_library(settings INTERFACE)
add_library(deps_public INTERFACE)
add_library(deps_private INTERFACE)

target_include_directories(settings INTERFACE 
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/private>
)

if(USE_CACHE_COMPRESSION STREQUAL "ON" OR USE_CACHE_COMPRESSION STREQUAL "AUTO")
	find_package(zstd CONFIG QUIET)

	if(NOT zstd_FOUND)
		find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
		find_library(ZSTD_LIBRARY NAMES zstd libzstd)
		if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
			set(zstd_FOUND TRUE)
		endif()
	endif()

	if(NOT zstd_FOUND)
		if(USE_CACHE_COMPRESSION STREQUAL "ON")
			message(FATAL_ERROR "Zstd compression requested but libzstd headers were not found!")
		else()
			message(STATUS "Zstd not found. Disabling AST cache compression.")
			set(USE_CACHE_COMPRESSION "OFF")
		endif()
	endif()
endif()

if(USE_CACHE_COMPRESSION STREQUAL "ON" OR USE_CACHE_COMPRESSION STREQUAL "AUTO")
	target_compile_definitions(settings INTERFACE ENABLE_AST_COMPRESSION)
	message(STATUS "Zstd found, cache compression enabled")

	if(TARGET zstd::libzstd)
		target_link_libraries(settings INTERFACE zstd::libzstd)
	else()
		target_include_directories(settings INTERFACE ${ZSTD_INCLUDE_DIR})
		target_link_libraries(settings INTERFACE ${ZSTD_LIBRARY})
	endif()
endif()

# Dependencies are added by the configuration script as separate
# target_link_libraries(deps_public ...) and target_link_libraries(deps_private ...)
# lines. Do not remove them manually.

find_program(CCACHE_PROGRAM ccache)
if(USE_CCACHE AND CCACHE_PROGRAM)
	set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
	set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
	message(STATUS "ccache is found and is being used")
elseif(USE_CCACHE)
	message(WARNING "ccache is enabled but not found.")
endif()

if(MSVC)
	target_compile_options(settings INTERFACE /W4)
else()
	target_compile_options(settings INTERFACE -Wall -Wextra -Wpedantic -Wno-strict-prototypes)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
	if(MSVC)
		target_compile_options(settings INTERFACE /O2 /Ob2 /Oi /DNDEBUG)
	else()
		target_compile_options(settings INTERFACE -O3 -DNDEBUG)
	endif()
endif()

if(MARCH_NATIVE)
	if(MSVC)
		message(WARNING "MARCH_NATIVE is not supported by MSVC logic directly. Ignoring to prevent errors.")
	else()
		target_compile_options(settings INTERFACE -march=native -mtune=native)
		message(STATUS "Native architecture optimization enabled")
	endif()
endif()

set(ENABLE_LTO_ON_TARGETS FALSE)
if(USE_LTO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT ltos OUTPUT ltoerr)
	if(ltos)
		set(ENABLE_LTO_ON_TARGETS TRUE)
		message(STATUS "LTO enabled and supported")
	else()
		message(STATUS "LTO enabled but not supported: ${ltoerr}")
	endif()
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS  "src/*.c")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(DO_INSTALL)
	include(GNUInstallDirs)
endif()

add_executable(tereix_bin ${SOURCES})

target_include_directories(settings INTERFACE 
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/private>
	$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}> # <-- ADD THIS
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/compute_hash.cmake" "
# Glob all sources and sort them to guarantee a deterministic hash order
file(GLOB_RECURSE SRC_FILES LIST_DIRECTORIES false \"\${SRC_DIR}/src/*.c\" \"\${SRC_DIR}/src/*.h\")
list(SORT SRC_FILES)

set(HASH_STR \"\")
foreach(F IN LISTS SRC_FILES)
    file(SHA256 \"\${F}\" OUT)
    string(APPEND HASH_STR \"\${OUT}\")
endforeach()

# Create one final hash representing the compiler's code state
string(SHA256 FINAL_HASH \"\${HASH_STR}\")
set(HEADER_CONTENT \"#pragma once\\n#define TEREIX_BUILD_HASH \\\"\${FINAL_HASH}\\\"\\n\")

# Only overwrite the header if the hash actually changed (prevents false positive rebuilds)
if(EXISTS \"\${BIN_DIR}/tereix_version.h\")
    file(READ \"\${BIN_DIR}/tereix_version.h\" OLD_CONTENT)
    if(NOT OLD_CONTENT STREQUAL HEADER_CONTENT)
        file(WRITE \"\${BIN_DIR}/tereix_version.h\" \"\${HEADER_CONTENT}\")
    endif()
else()
    file(WRITE \"\${BIN_DIR}/tereix_version.h\" \"\${HEADER_CONTENT}\")
endif()
")

add_custom_target(generate_version_header ALL
    COMMAND ${CMAKE_COMMAND} 
        -D SRC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\" 
        -D BIN_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\" 
        -P \"${CMAKE_CURRENT_BINARY_DIR}/compute_hash.cmake\"
    BYPRODUCTS \"${CMAKE_CURRENT_BINARY_DIR}/tereix_version.h\"
    COMMENT \"Evaluating compiler source state for caching...\"
)

add_dependencies(tereix_bin generate_version_header)

set_target_properties(tereix_bin PROPERTIES OUTPUT_NAME "tereix")
target_link_libraries(tereix_bin 
	PUBLIC deps_public 
	PRIVATE settings deps_private
)
if(ENABLE_LTO_ON_TARGETS)
	set_target_properties(tereix_bin PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
if(DO_INSTALL)
	install(TARGETS tereix_bin
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	)
endif()

enable_testing()
include(CTest)
if(BUILD_TESTING)
	add_subdirectory(tests)
endif()

