# Consumer of an installed UnrealLib: configured against the install prefix by tests/Install/RunConsumerTest.cmake.
cmake_minimum_required(VERSION 3.25)
project(UnrealLibConsumer LANGUAGES CXX)

find_package(UnrealLib ${UNREALLIB_EXPECTED_VERSION} EXACT CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_20)
# One module on its own and the umbrella target.
target_link_libraries(consumer PRIVATE UnrealLib::T3D UnrealLib::UnrealLib)
target_compile_definitions(consumer PRIVATE UNREALLIB_EXPECTED_VERSION="${UNREALLIB_EXPECTED_VERSION}")
