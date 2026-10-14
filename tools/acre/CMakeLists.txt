# Unit tests of the acre pipeline (Python, stdlib only).
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(NAME acre_unit
         COMMAND ${Python3_EXECUTABLE} -m unittest discover -s acre/tests -t .
         WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tools)
set_tests_properties(acre_unit PROPERTIES LABELS acre)
