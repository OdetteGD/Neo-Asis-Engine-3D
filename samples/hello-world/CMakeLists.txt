add_executable(neo_player main.cpp)
target_link_libraries(neo_player PRIVATE neo_engine)

# GLFW comes from cmake/Dependencies.cmake (linked into neo_engine).


# Ship a minimal asset tree next to the binary so shaders resolve in dev.
add_custom_command(TARGET neo_player POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/shaders"
          "$<TARGET_FILE_DIR:neo_player>/shaders"
  COMMENT "Copying shader sources beside the player")

set_target_properties(neo_player PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
