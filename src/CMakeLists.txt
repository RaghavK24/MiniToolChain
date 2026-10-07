# The whole toolchain is one library; the driver in cli/ is a thin shell over
# it, and the tests link the same library the driver does.
add_library(minitool_core
    common/source_manager.cpp
    common/checksum.cpp
    diagnostics/diagnostic.cpp
    diagnostics/diagnostic_engine.cpp
    isa/registers.cpp
    isa/opcode.cpp
    isa/encoding.cpp
    isa/semantics.cpp
    lexer/lexer.cpp
    ast/ast.cpp
    parser/parser.cpp
    assembler/sema.cpp
    assembler/assembler.cpp
    assembler/pipeline.cpp
    ir/ir.cpp
    ir/lower.cpp
    optimizer/optimizer.cpp
    object/object.cpp
    object/object_io.cpp
    object/relocation.cpp
    linker/linker.cpp
    executable/executable.cpp
    executable/executable_io.cpp
    vm/memory.cpp
    vm/syscall.cpp
    vm/vm.cpp
    disassembler/disassembler.cpp
    debugger/debugger.cpp
    playground/session.cpp
    playground/http_server.cpp)
add_library(minitool::core ALIAS minitool_core)

target_link_libraries(minitool_core PUBLIC minitool_options)
# `minitool serve` uses BSD sockets; on Windows those live in ws2_32. The
# source also carries a #pragma comment(lib) for MSVC, but naming it here keeps
# the dependency correct for clang-cl and MinGW too.
if(WIN32)
    target_link_libraries(minitool_core PUBLIC ws2_32)
endif()
target_include_directories(minitool_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

add_executable(minitool cli/main.cpp)
target_link_libraries(minitool PRIVATE minitool::core)
