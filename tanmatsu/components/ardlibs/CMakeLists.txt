# ALL vendored Arduino libraries compiled as ONE IDF component, so their cross-includes
# resolve without per-lib REQUIRES (e.g. RTClib -> Adafruit_I2CDevice.h from Adafruit BusIO).
# Payload under upstream/<lib>/ is gitignored (populated by fetch-deps.sh); this wrapper is committed.
file(GLOB _libs LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/upstream/*")
set(_inc "")
foreach(_l ${_libs})
  if(IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/${_l}")
    list(APPEND _inc "${_l}")
    if(IS_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/${_l}/src")
      list(APPEND _inc "${_l}/src")
    endif()
  endif()
endforeach()
file(GLOB_RECURSE _srcs RELATIVE "${CMAKE_CURRENT_LIST_DIR}"
     "${CMAKE_CURRENT_LIST_DIR}/upstream/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/upstream/*.c")
list(FILTER _srcs EXCLUDE REGEX "/(examples|test|tests|extras)/|catch|/test[-_]")   # drop bundled tests (e.g. base64's Catch)
idf_component_register(SRCS ${_srcs} INCLUDE_DIRS ${_inc} REQUIRES espressif__arduino-esp32 nvs_flash)
# Pure third-party Arduino libs — silence warnings (and never -Werror-fail) on code we don't own.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
