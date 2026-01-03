cmake_minimum_required(VERSION 3.28.3)

project(FileWatcher VERSION 1.0 LANGUAGES CXX)

# Default to a Debug build for single-configuration generators when unset
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Use a modern C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# Create executable from sources
add_executable(FileWatcher
	FileWatcher.cpp
	ThreadPool.cpp
	main.cpp
)

# Public include directory (project root)
target_include_directories(FileWatcher
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
)

# Prefer modern CMake feature requirement
target_compile_features(FileWatcher PRIVATE cxx_std_23)

# Ensure debug-friendly flags for Debug configuration
target_compile_options(FileWatcher PRIVATE
	$<$<CONFIG:Debug>:-g -O0>
)

# Compiler warnings
if(MSVC)
	target_compile_options(FileWatcher PRIVATE /W4 /permissive-)
else()
	target_compile_options(FileWatcher PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Install rules
include(GNUInstallDirs)

install(TARGETS FileWatcher
	EXPORT FileWatcherTargets
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES FileWatcher.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Export for use by other projects
install(EXPORT FileWatcherTargets
	FILE FileWatcherTargets.cmake
	NAMESPACE FileWatcher::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FileWatcher
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/FileWatcherConfigVersion.cmake"
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/FileWatcherConfig.cmake.in"
	"${CMAKE_CURRENT_BINARY_DIR}/FileWatcherConfig.cmake"
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FileWatcher
)

install(FILES
	"${CMAKE_CURRENT_BINARY_DIR}/FileWatcherConfig.cmake"
	"${CMAKE_CURRENT_BINARY_DIR}/FileWatcherConfigVersion.cmake"
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FileWatcher
)