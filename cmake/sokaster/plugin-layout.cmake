# Install layout that loads in OBS 32 and OBS 33+ alike (Windows).
#
# OBS 32 looks for third-party plugins only in
#   %ProgramData%\obs-studio\plugins\<name>\bin\64bit\<name>.dll
# OBS 33 moved them to
#   %ProgramData%\obs-studio\plugins\<name>\<name>.dll
# and still reads the old place as "Legacy" until OBS 34 drops it. Both share
# <name>\data. So the DLL goes to both places: OBS 32 sees only bin\64bit,
# OBS 33+ loads the new one first and skips the legacy copy as already loaded.
# Once OBS 32 is no longer supported, bin\64bit can go.

include_guard(GLOBAL)

function(target_install_both_layouts target)
  if(OS_WINDOWS)
    install(FILES "$<TARGET_FILE:${target}>" DESTINATION "${target}")
  endif()
endfunction()
