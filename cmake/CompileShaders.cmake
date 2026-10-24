# ---------------------------------------------------------------------------
#  GLSL -> SPIR-V at build time.
#
#  Bakes every *.glsl under SOURCE_DIR into OUTPUT_DIR/<relative path>.spv and
#  attaches the whole directory to the target as a definition so the runtime
#  can locate it (NEO_SHADER_DIR) and, on Android, pack it into assets.
# ---------------------------------------------------------------------------
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang
             HINTS $ENV{VULKAN_SDK}/bin ENV{PATH})

function(neo_compile_shaders TARGET)
  cmake_parse_arguments(ARG "" "SOURCE_DIR;OUTPUT_DIR" "" ${ARGN})

  if(NOT GLSLANG_VALIDATOR)
    message(WARNING
      "glslangValidator not found - shaders will NOT be compiled.\n"
      "Install the Vulkan SDK (or glslang-tools) and re-configure, or point "
      "GLSLANG_VALIDATOR at the binary.")
    target_compile_definitions(${TARGET} PUBLIC NEO_SHADER_DIR="shaders")
    return()
  endif()

  file(GLOB_RECURSE SHADER_FILES CONFIGURE_DEPENDS
       "${ARG_SOURCE_DIR}/*.glsl" "${ARG_SOURCE_DIR}/*.vert"
       "${ARG_SOURCE_DIR}/*.frag" "${ARG_SOURCE_DIR}/*.comp")

  set(SPV_OUTPUTS)
  foreach(SHADER ${SHADER_FILES})
    file(RELATIVE_PATH REL ${ARG_SOURCE_DIR} ${SHADER})
    set(SPV "${ARG_OUTPUT_DIR}/${REL}.spv")
    get_filename_component(SPV_DIR "${SPV}" DIRECTORY)
    file(MAKE_DIRECTORY "${SPV_DIR}")

    add_custom_command(
      OUTPUT  "${SPV}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${SPV_DIR}"
      COMMAND ${GLSLANG_VALIDATOR}
              --target-env vulkan1.2 -Os
              -I"${ARG_SOURCE_DIR}"
              -o "${SPV}" "${SHADER}"
      DEPENDS "${SHADER}"
      COMMENT "GLSL  ${REL}"
      VERBATIM)
    list(APPEND SPV_OUTPUTS "${SPV}")
  endforeach()

  add_custom_target(${TARGET}_shaders DEPENDS ${SPV_OUTPUTS})
  add_dependencies(${TARGET} ${TARGET}_shaders)
  target_compile_definitions(${TARGET} PUBLIC
    NEO_SHADER_DIR="${ARG_OUTPUT_DIR}")
  set_property(TARGET ${TARGET}_shaders PROPERTY SPV_OUTPUTS ${SPV_OUTPUTS})
  set_property(GLOBAL PROPERTY NEO_SPV_OUTPUTS ${SPV_OUTPUTS})

  list(LENGTH SPV_OUTPUTS N)
  message(STATUS "Neo-Asis: ${N} shaders will be compiled to SPIR-V")
endfunction()
