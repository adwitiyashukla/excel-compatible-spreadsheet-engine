cmake_minimum_required(VERSION 3.20)
project(excel_compatible_spreadsheet_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SHEET_WERROR "Treat warnings as errors" OFF)
option(SHEET_ASAN "Build with address and undefined behavior sanitizers" OFF)
option(SHEET_TSAN "Build with thread sanitizer" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /bigobj /wd4100 /wd4244 /wd4267 /wd4324)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  if(SHEET_WERROR)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(SHEET_WERROR)
    add_compile_options(-Werror)
  endif()
  if(SHEET_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
  endif()
  if(SHEET_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  endif()
endif()

find_package(Threads REQUIRED)

add_library(sheetcore STATIC
  src/core.cpp
  src/format.cpp
  src/parser.cpp
  src/printer.cpp
  src/vm.cpp
  src/fn_math.cpp
  src/fn_stats.cpp
  src/fn_text.cpp
  src/fn_lookup.cpp
  src/fn_date.cpp
  src/fn_finance.cpp
  src/depgraph.cpp
  src/workbook.cpp
  src/recalc.cpp
  src/structure.cpp
  src/deflate.cpp
  src/zip.cpp
  src/xml.cpp
  src/xlsx.cpp
)
target_include_directories(sheetcore PUBLIC src)
target_link_libraries(sheetcore PUBLIC Threads::Threads)

add_executable(sheet src/main.cpp src/bench.cpp)
target_link_libraries(sheet PRIVATE sheetcore)
if(MINGW)
  target_link_options(sheet PRIVATE -static)
endif()

enable_testing()
foreach(name core formula functions engine io difftest)
  add_executable(${name}_test tests/${name}_test.cpp)
  target_link_libraries(${name}_test PRIVATE sheetcore)
  if(MINGW)
    target_link_options(${name}_test PRIVATE -static)
  endif()
  add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

set(POI_DATA "${CMAKE_SOURCE_DIR}/data/poi/test-data/spreadsheet")
set(LO_DATA "${CMAKE_SOURCE_DIR}/data/libreoffice/sc/qa/unit/data/functions")
if(EXISTS "${POI_DATA}")
  add_test(NAME poi_conformance COMMAND sheet check "${POI_DATA}" --min 99.9)
endif()
if(EXISTS "${LO_DATA}")
  add_test(NAME libreoffice_conformance COMMAND sheet check "${LO_DATA}" --min 93)
endif()
