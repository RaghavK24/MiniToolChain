# The suite uses the bundled xUnit runner in tests/support (ADR-010), so it
# needs no network access and no third-party checkout. `test_main.cpp` provides
# main() for every binary, the way gtest_main does.
add_library(minitool_test_main STATIC support/test_main.cpp)
target_include_directories(minitool_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(minitool_test_main PUBLIC minitool::core)

# Tests run from the repository root so that the ones reading examples/ and
# tests/fixtures/ find them.
function(minitool_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE minitool_test_main)  # brings in minitool::core
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

# --- foundation, ISA, encoding -----------------------------------------------
minitool_add_test(test_common
    unit/test_byte_order.cpp
    unit/test_source_manager.cpp)
minitool_add_test(test_diagnostics unit/test_diagnostics.cpp)
minitool_add_test(test_isa
    unit/test_registers.cpp
    unit/test_opcode_table.cpp
    unit/test_encoding.cpp
    unit/test_semantics.cpp)

# --- the front end -----------------------------------------------------------
minitool_add_test(test_lexer unit/test_lexer.cpp)
minitool_add_test(test_parser unit/test_parser.cpp)
minitool_add_test(test_sema unit/test_sema.cpp)
minitool_add_test(test_ir unit/test_ir.cpp)
minitool_add_test(test_assembler unit/test_assembler.cpp)

# --- binary formats and linking ----------------------------------------------
minitool_add_test(test_object unit/test_object.cpp)
minitool_add_test(test_relocation unit/test_relocation.cpp)
minitool_add_test(test_linker unit/test_linker.cpp)
minitool_add_test(test_executable unit/test_executable.cpp)

# --- runtime and tools -------------------------------------------------------
minitool_add_test(test_memory unit/test_memory.cpp)
minitool_add_test(test_vm unit/test_vm.cpp)
minitool_add_test(test_disassembler unit/test_disassembler.cpp)
minitool_add_test(test_optimizer unit/test_optimizer.cpp)
minitool_add_test(test_debugger unit/test_debugger.cpp)

# --- the browser playground --------------------------------------------------
minitool_add_test(test_playground unit/test_playground.cpp)

# --- integration, properties, golden files, fuzzing, failures ----------------
minitool_add_test(test_pipeline integration/test_pipeline.cpp)
minitool_add_test(test_multi_file integration/test_multi_file.cpp)
minitool_add_test(test_golden_binaries golden/test_golden_binaries.cpp)
minitool_add_test(test_properties
    property/test_encoding_properties.cpp
    property/test_format_properties.cpp)
minitool_add_test(test_fuzz fuzz/test_fuzz.cpp)
minitool_add_test(test_failure_cases failure/test_failure_cases.cpp)
