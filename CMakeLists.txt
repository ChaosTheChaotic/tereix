cmake_minimum_required(VERSION 3.15)
project(tereix VERSION 1.0 LANGUAGES C)

option(USE_CCACHE "Try to use ccache for faster builds" OFF)
option(USE_LTO "Use linktime optimizations if available" ON)
option(MARCH_NATIVE "Build library optimized for native arch" OFF)
option(DO_INSTALL "Try to install library after it is build" OFF)
option(FETCH_STDLIB "Fetch stdlib and install it (via git)" ON)
set(USE_CACHE_COMPRESSION "AUTO" CACHE STRING "Compress AST cache using zstd (ON, OFF, AUTO)")
set(ALLOW_PARALLEL "AUTO" CACHE STRING "Compiles in the ability to use multiple threads for compilation (ON, OFF, AUTO)")

add_library(settings INTERFACE)
add_library(deps_public INTERFACE)
add_library(deps_private INTERFACE)

if(ALLOW_PARALLEL STREQUAL "ON" OR ALLOW_PARALLEL STREQUAL "AUTO")
    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads QUIET)

    if(Threads_FOUND)
        include(CheckCSourceCompiles)
        message(STATUS "Checking for stdatomic compat")
        set(ATOMIC_TEST_CODE "
        #include <stdatomic.h>
        #include <stddef.h>
        int main(void) {
            atomic_size_t counter;
            atomic_init(&counter, 0);
            atomic_fetch_add(&counter, 1);
            return 0;
        }
        ")
        
        check_c_source_compiles("${ATOMIC_TEST_CODE}" ATOMICS_NATIVE_SUPPORT)
        
        if(ATOMICS_NATIVE_SUPPORT)
            set(HAS_WORKING_ATOMICS TRUE)
        else()
            set(CMAKE_REQUIRED_LIBRARIES atomic)
            check_c_source_compiles("${ATOMIC_TEST_CODE}" ATOMICS_NEEDS_LIBATOMIC)
            unset(CMAKE_REQUIRED_LIBRARIES)

            if(ATOMICS_NEEDS_LIBATOMIC)
                set(HAS_WORKING_ATOMICS TRUE)
            endif()
        endif()

        if(HAS_WORKING_ATOMICS)
            message(STATUS "Threads and Atomics supported. Parallel compilation enabled.")
            
						target_compile_definitions(settings INTERFACE ENABLE_THREADS)
            
            target_link_libraries(settings INTERFACE Threads::Threads)
            
            if(ATOMICS_NEEDS_LIBATOMIC)
                target_link_libraries(settings INTERFACE atomic)
            endif()
        else()
            if(ALLOW_PARALLEL STREQUAL "ON")
                message(FATAL_ERROR "ALLOW_PARALLEL requested, but working <stdatomic.h> was not found.")
            else()
                message(STATUS "Atomics not supported. Disabling parallel features.")
            endif()
        endif()
    else()
        if(ALLOW_PARALLEL STREQUAL "ON")
            message(FATAL_ERROR "ALLOW_PARALLEL requested, but system thread library (e.g. pthreads) was not found.")
        else()
            message(STATUS "System threads not found. Disabling parallel features.")
        endif()
    endif()
endif()

find_package(yyjson CONFIG QUIET)

if(NOT yyjson_FOUND)
	find_path(YYJSON_INCLUDE_DIR NAMES yyjson.h)
	find_library(YYJSON_LIBRARY NAMES yyjson libyyjson)
	
	if(YYJSON_INCLUDE_DIR AND YYJSON_LIBRARY)
		set(yyjson_FOUND TRUE)
		
		add_library(yyjson::yyjson UNKNOWN IMPORTED)
		set_target_properties(yyjson::yyjson PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES "${YYJSON_INCLUDE_DIR}"
			IMPORTED_LOCATION "${YYJSON_LIBRARY}"
		)
	endif()
endif()

if(NOT yyjson_FOUND)
	if(USE_CACHE_COMPRESSION STREQUAL "ON")
		message(STATUS "yyjson compression requested but libyyjson headers/libraries were not found!")
	else()
		message(STATUS "yyjson not found")
	endif()
endif()

if(USE_CACHE_COMPRESSION STREQUAL "ON" OR USE_CACHE_COMPRESSION STREQUAL "AUTO")
	find_package(zstd CONFIG QUIET)

	if (zstd_FOUND AND TARGET zstd::libzstd)
		set(ZSTD_TARGET zstd::libzstd)
	elseif(TARGET zstd::libzstd_shared)
		set(ZSTD_TARGET zstd::libzstd_shared)
	elseif(TARGET zstd::libzstd_static)
		set(ZSTD_TARGET zstd::libzstd_static)
	endif()

	if (NOT ZSTD_TARGET)
			find_path(ZSTD_INCLUDE_DIR NAMES zstd.h)
			find_library(ZSTD_LIBRARY NAMES zstd libzstd)

			if (ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
				set(zstd_FOUND TRUE)
			else()
				set(zstd_FOUND FALSE)
			endif()
		endif()

		if (NOT zstd_FOUND)
			if (USE_CACHE_COMPRESSION STREQUAL "ON")
				message(FATAL_ERROR "Zstd compression requested by libzstd headers/libraries were not found")
			else()
				message(STATUS "Zstd not found. Disabling AST cache compression.")
				set(USE_CACHE_COMPRESSION "OFF")
			endif()
		endif()
endif()

if(USE_CACHE_COMPRESSION STREQUAL "ON" OR USE_CACHE_COMPRESSION STREQUAL "AUTO")
	target_compile_definitions(settings INTERFACE ENABLE_AST_COMPRESSION)
	message(STATUS "Zstd found, cache compression enabled")

	if(ZSTD_TARGET)
		target_link_libraries(settings INTERFACE ${ZSTD_TARGET})
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

if(yyjson_FOUND)
	message(STATUS "Using system yyjson. Excluding internal src/yyjson.c from compilation.")
	list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/yyjson.c")
else()
	message(STATUS "System yyjson not found. Compiling fallback internal src/yyjson.c.")
endif()

if (NOT HAS_WORKING_ATOMICS)
	message(STATUS "Removing thread types from compilation.")
	list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/data_structures/thread_pool/thread_pool.c")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(DO_INSTALL)
	include(GNUInstallDirs)
	target_compile_definitions(settings INTERFACE 
		TX_STDLIB_DIR=\"${CMAKE_INSTALL_PREFIX}/lib/tereix/std\"
	)
endif()

add_executable(tereix_bin ${SOURCES})

target_include_directories(settings INTERFACE 
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/private>
	$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
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

if(yyjson_FOUND)
	target_link_libraries(tereix_bin PRIVATE yyjson::yyjson)
endif()

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

if(FETCH_STDLIB)
    include(FetchContent)
    FetchContent_Declare(
        tereix_stdlib
        GIT_REPOSITORY https://github.com/ChaosTheChaotic/tereix-stdlib.git
        GIT_TAG        master
    )
    FetchContent_MakeAvailable(tereix_stdlib)

    if(DO_INSTALL)
        install(DIRECTORY ${tereix_stdlib_SOURCE_DIR}/
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/tereix/std)
    endif()
endif()
