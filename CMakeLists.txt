cmake_minimum_required(VERSION 3.16)
project(mix-track-filter-plugin VERSION 0.3.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED OBS_PATH)
    list(APPEND CMAKE_PREFIX_PATH "${OBS_PATH}")
    list(APPEND CMAKE_PREFIX_PATH "${OBS_PATH}/frontend/api") # For obs-frontend-apiConfig.cmake
    # Add paths to locate internal dependencies in OBS build tree (e.g. w32-pthreads)
    list(APPEND CMAKE_PREFIX_PATH "${OBS_PATH}/deps/w32-pthreads")
    list(APPEND CMAKE_PREFIX_PATH "${OBS_PATH}/deps/jansson")
    list(APPEND CMAKE_PREFIX_PATH "${OBS_PATH}/deps/libcaption")
    include_directories("${OBS_PATH}/../deps/libcaption")
    # Add path to load internal finders (e.g. SIMDe) required by libobsConfig.cmake
    list(APPEND CMAKE_MODULE_PATH "${OBS_PATH}/../cmake/finders")
    # Auto-detect and append obs-deps folder required by finders
    file(GLOB OBS_DEPS_DIRS "${OBS_PATH}/../.deps/obs-deps-*-x64")
    list(APPEND CMAKE_PREFIX_PATH ${OBS_DEPS_DIRS})
endif()

if(DEFINED QT_DIR)
    list(APPEND CMAKE_PREFIX_PATH "${QT_DIR}")
endif()

# Find OBS libraries
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

# Find Qt (Qt6 or Qt5)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(SRC_FILES
    src/plugin-main.cpp
    src/audio-processor.cpp
    src/mix-track-dock.cpp
    src/settings-dialog.cpp
    src/filter-helper.c
)

set(HEADERS
    src/plugin-main.h
    src/audio-processor.hpp
    src/mix-track-dock.hpp
    src/settings-dialog.hpp
)

add_library(${PROJECT_NAME} MODULE ${SRC_FILES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /W4)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mix-track-filter"
)
