function(add_tereix_test TEST_NAME)
  add_executable(${TEST_NAME} ${ARGN})
  target_link_libraries(${TEST_NAME}
    deps_public
    settings
    deps_private
  )
  target_include_directories(${TEST_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
  add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()
