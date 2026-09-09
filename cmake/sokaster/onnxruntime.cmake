# ONNX Runtime, fetched from the project's own GitHub releases.
#
# obs-deps does not carry it, so the plugin brings its own copy the way other
# OBS plugins that run models do. Version 1.22.0 is deliberate: it is the last
# release with a macOS universal archive, and it is the version the Silero
# model has been running against in production.

include_guard(GLOBAL)

include(FetchContent)

set(ONNXRUNTIME_VERSION "1.22.0")
set(ONNXRUNTIME_BASE_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}")

if(OS_WINDOWS)
  set(_onnxruntime_archive "onnxruntime-win-x64-${ONNXRUNTIME_VERSION}.zip")
  set(_onnxruntime_hash "SHA256=174c616efc0271194488642a72f1a514e01487da4dfe84c49296d66e40ebe0da")
elseif(OS_MACOS)
  set(_onnxruntime_archive "onnxruntime-osx-universal2-${ONNXRUNTIME_VERSION}.tgz")
  set(_onnxruntime_hash "SHA256=cfa6f6584d87555ed9f6e7e8a000d3947554d589efe3723b8bfa358cd263d03c")
else()
  set(_onnxruntime_archive "onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz")
  set(_onnxruntime_hash "SHA256=8344d55f93d5bc5021ce342db50f62079daf39aaafb5d311a451846228be49b3")
endif()

FetchContent_Declare(
  onnxruntime
  URL "${ONNXRUNTIME_BASE_URL}/${_onnxruntime_archive}"
  URL_HASH "${_onnxruntime_hash}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# The archive is a plain binary distribution with no CMakeLists of its own, so
# this only unpacks it.
FetchContent_MakeAvailable(onnxruntime)

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(
  onnxruntime::onnxruntime
  PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include"
)

if(OS_WINDOWS)
  set(ONNXRUNTIME_RUNTIME_LIBRARY "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll")
  set_target_properties(
    onnxruntime::onnxruntime
    PROPERTIES
      IMPORTED_LOCATION "${ONNXRUNTIME_RUNTIME_LIBRARY}"
      IMPORTED_IMPLIB "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib"
  )
elseif(OS_MACOS)
  set(ONNXRUNTIME_RUNTIME_LIBRARY "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.${ONNXRUNTIME_VERSION}.dylib")
  set_target_properties(onnxruntime::onnxruntime PROPERTIES IMPORTED_LOCATION "${ONNXRUNTIME_RUNTIME_LIBRARY}")
else()
  set(ONNXRUNTIME_SONAME "libonnxruntime.so.1")
  set(ONNXRUNTIME_RUNTIME_LIBRARY "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so.${ONNXRUNTIME_VERSION}")
  set_target_properties(
    onnxruntime::onnxruntime
    PROPERTIES IMPORTED_LOCATION "${ONNXRUNTIME_RUNTIME_LIBRARY}" IMPORTED_SONAME "${ONNXRUNTIME_SONAME}"
  )
  include(GNUInstallDirs)
endif()

# target_add_onnxruntime: link the runtime and make sure it travels with the plugin
#
# The runtime is not part of OBS, so wherever the plugin ends up the library has
# to end up beside it — otherwise the plugin loads on the build machine and
# nowhere else.
function(target_add_onnxruntime target)
  target_link_libraries(${target} PRIVATE onnxruntime::onnxruntime)

  if(OS_WINDOWS)
    # Next to the plugin's own DLL, both when installed and when run from the
    # build directory.
    install(FILES "${ONNXRUNTIME_RUNTIME_LIBRARY}" DESTINATION "${target}/bin/64bit")

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "${ONNXRUNTIME_RUNTIME_LIBRARY}"
        "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
      COMMENT "Copy ONNX Runtime to rundir"
      VERBATIM
    )
  elseif(OS_MACOS)
    # Inside the bundle, where a plugin's private libraries belong. The library
    # declares itself as @rpath/..., so one rpath entry is all the loader needs.
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "@loader_path/../Frameworks")
    set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "@loader_path/../Frameworks")

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "${ONNXRUNTIME_RUNTIME_LIBRARY}"
        "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks"
      COMMENT "Copy ONNX Runtime into ${target}.plugin"
      VERBATIM
    )
  else()
    # Beside the plugin in lib/obs-plugins. $ORIGIN keeps the lookup relative to
    # wherever the distribution puts that directory.
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")

    # The versioned file, plus the SONAME symlink the loader actually asks for.
    # Both are created explicitly: copying the symlink from the archive would
    # follow it and leave a second 21 MB file behind.
    install(FILES "${ONNXRUNTIME_RUNTIME_LIBRARY}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    install(
      CODE
        "
      file(
        CREATE_LINK
        \"libonnxruntime.so.${ONNXRUNTIME_VERSION}\"
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/obs-plugins/${ONNXRUNTIME_SONAME}\"
        SYMBOLIC
      )
      "
    )

    add_custom_command(
      TARGET ${target}
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "${ONNXRUNTIME_RUNTIME_LIBRARY}"
        "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
      COMMAND
        "${CMAKE_COMMAND}" -E create_symlink "libonnxruntime.so.${ONNXRUNTIME_VERSION}"
        "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/${ONNXRUNTIME_SONAME}"
      COMMENT "Copy ONNX Runtime to rundir"
      VERBATIM
    )
  endif()
endfunction()
