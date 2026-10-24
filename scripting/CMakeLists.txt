# ---------------------------------------------------------------------------
#  C# scripting integration.
#
#  Adds a `neo_scripts` target that shells out to `dotnet publish` with
#  NativeAOT. If the .NET SDK is missing, the target is skipped with a warning
#  rather than failing the build — the engine itself does not depend on it.
# ---------------------------------------------------------------------------
find_program(DOTNET_EXECUTABLE dotnet)

option(NEO_BUILD_SCRIPTS "Compile the C# gameplay assembly with NativeAOT" ON)
set(NEO_SCRIPT_RID "linux-x64" CACHE STRING "Runtime identifier for the script assembly")

if(NEO_ENABLE_SCRIPTING AND NEO_BUILD_SCRIPTS AND DOTNET_EXECUTABLE)
  add_custom_target(neo_scripts ALL
    COMMAND ${DOTNET_EXECUTABLE} publish
            "${CMAKE_CURRENT_SOURCE_DIR}/NeoAsis.Scripting/NeoAsis.Scripting.csproj"
            -c Release
            -r ${NEO_SCRIPT_RID}
            --self-contained true
            -p:PublishAot=true
            -p:NativeLib=Shared
            -o "${CMAKE_BINARY_DIR}/scripts/${NEO_SCRIPT_RID}"
    COMMENT "Publishing C# gameplay assembly (NativeAOT, ${NEO_SCRIPT_RID})"
    VERBATIM)

  message(STATUS "Neo-Asis: C# scripting enabled (${NEO_SCRIPT_RID})")
elseif(NEO_ENABLE_SCRIPTING)
  message(WARNING
    "NEO_ENABLE_SCRIPTING is ON but the .NET SDK was not found. "
    "Install .NET 10 or set -DDOTNET_EXECUTABLE=<path>. "
    "The engine will build without managed scripts.")
endif()
