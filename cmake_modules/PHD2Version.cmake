#.rst:
# .. command:: get_phd_version
#
#    Extract the current version from version.md and populate the variables
#    `VERSION_MAJOR`, `VERSION_MINOR`, `VERSION_PATCH`, and `VERSION_SUFFIX`.
#    Raises an error if the version cannot be extracted.
#
# Extracted from the former PHD2BuildDoc.cmake (which also held the GUI-help /
# i18n build machinery, removed for the headless, English-only build).
function(get_phd_version)
  set(filename_to_extract_from ${PHD_PROJECT_ROOT_DIR}/version.md)
  file(STRINGS ${filename_to_extract_from} file_content)

  foreach(SRC_LINE ${file_content})
    if("${SRC_LINE}" MATCHES "^[ \t]*([0-9]+)\\.([0-9]+)\\.([0-9]+)([A-Za-z0-9._-]*)[ \t]*$")
      set(VERSION_MAJOR ${CMAKE_MATCH_1} PARENT_SCOPE)
      set(VERSION_MINOR ${CMAKE_MATCH_2} PARENT_SCOPE)
      set(VERSION_PATCH ${CMAKE_MATCH_3} PARENT_SCOPE)
      set(VERSION_SUFFIX ${CMAKE_MATCH_4} PARENT_SCOPE)
      return()
    endif()
  endforeach()

  message(FATAL_ERROR "Cannot extract version from file '${filename_to_extract_from}'. Expected a line like '1.2.3' or '1.2.3rc1'")
endfunction()
