// SPDX-License-Identifier: MIT
#pragma once

/// A minimal HTTP/1.1 server that puts the playground in a browser.
///
/// It exists so the UI can drive the *real* toolchain. Compiling assembly in
/// JavaScript would mean a second implementation of the lexer, assembler and
/// VM, free to disagree with the C++ one, which is exactly what architectural
/// rule 9 forbids. So the browser only draws; every answer it shows came from
/// `playground::runSource`.
///
/// Deliberately small: it serves one page and one endpoint, handles one
/// connection at a time, and always closes. It is a development tool for one
/// person on one machine, not a production server, and it binds to the
/// loopback interface so it is not reachable from the network.

#include <string>
#include <string_view>

#include "minitool/common/types.hpp"

namespace minitool::playground {

/// Largest request this server will read, headers and body together. A
/// playground source file is far smaller; the cap is what stops a malformed or
/// hostile request from growing the buffer without bound.
inline constexpr std::size_t kMaxRequestBytes = 1024 * 1024;

struct ServeOptions {
    u16 port = 8080;
    /// Bind address. Loopback by default: this endpoint runs submitted code,
    /// so it must not be exposed to the network without a deliberate choice.
    std::string host = "127.0.0.1";
};

/// Runs the server until the process is interrupted.
///
/// Returns 0 on a clean shutdown, or non-zero if the socket could not be
/// opened. The usual cause is that the port is already in use.
[[nodiscard]] int serve(const ServeOptions& options);

/// Routes one request and produces the complete response body.
///
/// Exposed so the routing, argument handling and JSON encoding can be tested
/// without opening a socket. `method` and `target` are as they appear on the
/// request line; `body` is the raw request body.
struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
};

[[nodiscard]] HttpResponse handleRequest(std::string_view method, std::string_view target,
                                         std::string_view body);

}  // namespace minitool::playground
