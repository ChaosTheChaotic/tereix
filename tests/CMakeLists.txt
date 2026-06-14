find_package(cmocka QUIET)
if(NOT cmocka_FOUND)
    message(STATUS "CMocka not found. Fetching locally for testing...")
    include(FetchContent)
    FetchContent_Declare(
        cmocka
        GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
        GIT_TAG        cmocka-2.0.2
    )
    set(WITH_STATIC_LIB ON CACHE BOOL "Build static library" FORCE)
    set(WITH_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
    set(UNIT_TESTING OFF CACHE BOOL "Build internal tests" FORCE)
    set(WITH_SHARED_LIB OFF CACHE BOOL "Build shared library" FORCE)
    FetchContent_MakeAvailable(cmocka)
endif()

file(GLOB_RECURSE TEREIX_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../src/*.c")
list(REMOVE_ITEM TEREIX_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../src/main.c")

if(yyjson_FOUND)
    list(REMOVE_ITEM TEREIX_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../src/yyjson.c")
endif()

add_library(tereix_core_for_tests OBJECT ${TEREIX_CORE_SOURCES})
target_link_libraries(tereix_core_for_tests PUBLIC deps_public PRIVATE settings deps_private)
if(yyjson_FOUND)
    target_link_libraries(tereix_core_for_tests PRIVATE yyjson::yyjson)
endif()

target_include_directories(tereix_core_for_tests PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/../src/private"
    "${CMAKE_CURRENT_BINARY_DIR}/.."
)

function(add_tereix_test TEST_NAME)
    add_executable(${TEST_NAME} ${ARGN})
    
    target_link_libraries(${TEST_NAME}
        PRIVATE
        cmocka
        tereix_core_for_tests
        deps_public
        settings
        deps_private
    )
    if(yyjson_FOUND)
        target_link_libraries(${TEST_NAME} PRIVATE yyjson::yyjson)
    endif()
    
    target_include_directories(${TEST_NAME} PRIVATE 
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/private"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
        "${CMAKE_CURRENT_BINARY_DIR}/.."
    )
    
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

add_subdirectory(data_structures)
add_subdirectory(core)
