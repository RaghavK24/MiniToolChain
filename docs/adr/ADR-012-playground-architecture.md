# ADR-012: The playground runs the real toolchain over a local socket

**Status:** Accepted

## Context

The toolchain needed a place to type assembly and see what happens without
building a file, linking it and running the driver by hand. A browser is the
obvious surface: it already has a text editor, a scrollable output pane and a
way to lay both out.

That leaves the real question: where does the *compiling* happen?

Three options were considered.

**Compile in the browser.** Port the lexer, parser, assembler, linker and
virtual CPU to JavaScript. The page then works as a static file, with no
process to run and nothing to install. It is the standard answer for a
language playground, and for a project whose compiler already targets a
virtual machine it is not even much work.

**Compile in the browser via WebAssembly.** Build the existing C++ to WASM with
Emscripten. One implementation, no server. This is what most C++ playgrounds
do.

**Compile in a local process.** A small HTTP server in the existing binary; the
browser posts source and renders the report.

## Decision

A local process: `minitool serve`.

The JavaScript port was rejected on the project's own terms. Architectural rule
9 says there is one definition of what the machine does, and the codebase relies
on that in two places: the disassembler shares `isa::decode` with
the VM so it always shows what will actually run, and the optimizer's constant
folder shares `isa::evaluateBinary` with the VM's execute step so folding
always matches execution. A JavaScript assembler would be a third opinion
about what the ISA means, and it would be the one users actually saw, while
the tested C++ one sat behind it. Every bug fixed in one would have to be found
again in the other.

WebAssembly does not have that problem: it is the same code. It was rejected
for a smaller reason: it needs Emscripten, a toolchain that is not on this
machine, cannot be fetched on a machine with no network, and would become the
project's first build dependency after ADR-010 went to some trouble to remove
the last one. The playground is not worth reintroducing that.

So the browser only draws. Every number, message and caret on the page came out
of the same `assembleSource` -> `linker::link` -> `vm::run` sequence that
`minitool build` uses.

## Consequences

The playground needs a running local process, which a static page would not.
That is the price, and it is the right one to pay for a toolchain whose premise
is that there is one definition of the machine.

The server is small on purpose: two routes, one connection at a time, always
closes, no filesystem surface. The page is a string compiled into the binary.
Handling requests sequentially means there is no shared state and therefore no
concurrency to get wrong; a playground serves one person.

It binds to loopback by default, because the endpoint compiles and executes
submitted code. The VM sandbox already bounds what a submitted program can do,
and the source size, request size, output size and instruction budget bound
what a submitted *request* can do. Exposure to the network is available behind
`--host`, and is a decision the user makes explicitly.

Sockets are the only OS API the toolchain uses beyond the standard library, and
they are confined to one file. The dependency travels with that file (a
`#pragma comment(lib, "ws2_32.lib")` for MSVC and a `target_link_libraries` for
every other Windows compiler), so no other target has to know about it.

The split between `session.cpp` (no transport) and `http_server.cpp` (routing
and encoding) makes the behaviour testable: 24 tests, none of which
open a socket. The accept loop itself is not covered, which is an intentional
gap: testing it would mean binding a port, and everything worth asserting is
on the other side of `handleRequest`.

## Reversibility

If Emscripten ever becomes acceptable as a dependency, `session.cpp` is already
the right seam: it takes source text and returns a report, with no I/O of its
own. Compiling that one translation unit to WASM and calling it from the page
would replace the server without touching the UI or the pipeline.
