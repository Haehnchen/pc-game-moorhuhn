add_executable(moorhuhn_contract_compile compile.cpp)
target_link_libraries(moorhuhn_contract_compile PRIVATE moorhuhn::core)
set_target_properties(moorhuhn_contract_compile PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME contracts.compile COMMAND moorhuhn_contract_compile)

if(UNIX)
    find_program(MOORHUHN_MAKE_EXECUTABLE make)
    if(Python3_Interpreter_FOUND AND MOORHUHN_MAKE_EXECUTABLE)
        add_test(NAME build.make_entrypoints COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/make_entrypoints.py")
    endif()
endif()

if(MOORHUHN_BUILD_SDL_ADAPTER)
    add_executable(moorhuhn_dependency_versions sdl_versions.cpp)
    target_link_libraries(moorhuhn_dependency_versions PRIVATE moorhuhn::sdl_adapter)
    set_target_properties(moorhuhn_dependency_versions PROPERTIES CXX_EXTENSIONS OFF
        BUILD_WITH_INSTALL_RPATH FALSE)
    add_test(NAME dependencies.versions COMMAND moorhuhn_dependency_versions)
endif()
