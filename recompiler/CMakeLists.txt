# Generator that turns one module's dumped segments and roots into a C project.
# Header-only on the lifter side, so it builds without the runtime libraries.
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_executable(export_module export_module.cpp)
target_include_directories(export_module PRIVATE ${PROJECT_SOURCE_DIR}/runtime)
target_compile_features(export_module PRIVATE cxx_std_20)
target_link_libraries(export_module PRIVATE OpenSSL::Crypto)
