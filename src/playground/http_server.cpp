// SPDX-License-Identifier: MIT

#include "minitool/playground/http_server.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

#include "minitool/playground/session.hpp"
#include "page.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
// Keep the socket library dependency with the one file that needs it, so
// every target linking this object picks it up automatically -- this is
// what tools/build.ps1 relies on. MSVC-only: `#pragma comment` is not a
// standard pragma, and MinGW GCC treats an unrecognized one as an error
// under -Werror. MinGW gets the same dependency from CMake's
// `target_link_libraries(... ws2_32)` instead (src/CMakeLists.txt).
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace minitool::playground {
namespace {

// ------------------------------------------------------------- platform --

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void closeSocket(Socket socket) {
    closesocket(socket);
}
using SendSize = int;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void closeSocket(Socket socket) {
    close(socket);
}
using SendSize = std::size_t;
#endif

// ----------------------------------------------------------------- JSON --

/// Appends `text` as a JSON string literal, escaping what JSON requires and
/// replacing anything that is not well-formed UTF-8 with U+FFFD.
///
/// The replacement matters: a program's output is arbitrary bytes, and a lone
/// 0x80 in a JSON body makes the browser's JSON.parse throw, which would show
/// up as "the playground is broken" rather than "your program printed a byte
/// that is not text". Overlong encodings and surrogates are rejected too, so
/// the server can never emit a string the browser will refuse.
void appendJsonString(std::string& out, std::string_view text) {
    out.push_back('"');
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        switch (byte) {
            case '"':
                out += "\\\"";
                ++i;
                continue;
            case '\\':
                out += "\\\\";
                ++i;
                continue;
            case '\n':
                out += "\\n";
                ++i;
                continue;
            case '\r':
                out += "\\r";
                ++i;
                continue;
            case '\t':
                out += "\\t";
                ++i;
                continue;
            default:
                break;
        }
        if (byte < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(byte));
            ++i;
            continue;
        }
        if (byte < 0x80) {
            out.push_back(static_cast<char>(byte));
            ++i;
            continue;
        }

        // Multi-byte: work out the expected length and the legal range of the
        // second byte, which is what excludes overlong forms and surrogates.
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (byte >= 0xC2 && byte <= 0xDF) {
            length = 2;
        } else if (byte == 0xE0) {
            length = 3;
            low = 0xA0;
        } else if (byte >= 0xE1 && byte <= 0xEC) {
            length = 3;
        } else if (byte == 0xED) {
            length = 3;
            high = 0x9F;  // exclude UTF-16 surrogates
        } else if (byte >= 0xEE && byte <= 0xEF) {
            length = 3;
        } else if (byte == 0xF0) {
            length = 4;
            low = 0x90;
        } else if (byte >= 0xF1 && byte <= 0xF3) {
            length = 4;
        } else if (byte == 0xF4) {
            length = 4;
            high = 0x8F;  // largest code point is U+10FFFF
        }

        bool valid = length != 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const auto continuation = static_cast<unsigned char>(text[i + k]);
            const unsigned char min = (k == 1) ? low : 0x80;
            const unsigned char max = (k == 1) ? high : 0xBF;
            valid = continuation >= min && continuation <= max;
        }

        if (valid) {
            out.append(text.substr(i, length));
            i += length;
        } else {
            out += "\xEF\xBF\xBD";  // U+FFFD REPLACEMENT CHARACTER
            ++i;
        }
    }
    out.push_back('"');
}

void appendField(std::string& out, std::string_view name, std::string_view value, bool& first) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    appendJsonString(out, name);
    out.push_back(':');
    appendJsonString(out, value);
}

void appendRaw(std::string& out, std::string_view name, std::string_view value, bool& first) {
    if (!first) {
        out.push_back(',');
    }
    first = false;
    appendJsonString(out, name);
    out.push_back(':');
    out.append(value);
}

/// Renders a report as the JSON the page expects.
///
/// Every 64-bit quantity goes out as a *string*: JSON numbers are doubles, and
/// a register holding more than 2^53 would silently lose its low bits on the
/// way to the browser. The page reads them back with BigInt.
std::string toJson(const RunReport& report) {
    std::string out;
    out.push_back('{');
    bool first = true;

    appendRaw(out, "ok", report.ok ? "true" : "false", first);
    appendField(out, "stage", stageName(report.stage), first);
    appendField(out, "diagnostics", report.diagnostics, first);
    appendField(out, "error", report.error, first);
    appendField(out, "output", report.output, first);
    appendRaw(out, "output_truncated", report.output_truncated ? "true" : "false", first);
    appendField(out, "disassembly", report.disassembly, first);

    const bool ran = report.stage == Stage::Execute || report.stage == Stage::Finished;
    appendRaw(out, "ran", ran ? "true" : "false", first);
    appendField(out, "exit_code", std::format("{}", report.exit_code), first);
    appendField(out, "instructions", std::format("{}", report.instructions), first);
    appendField(out, "pc", std::format("{}", report.pc), first);
    appendField(out, "sp", std::format("{}", report.sp), first);
    appendField(out, "flags", std::format("{}", report.flags), first);

    std::string registers = "[";
    for (std::size_t i = 0; i < report.registers.size(); ++i) {
        if (i != 0) {
            registers.push_back(',');
        }
        appendJsonString(registers, std::format("{}", report.registers[i]));
    }
    registers.push_back(']');
    appendRaw(out, "registers", registers, first);

    std::string stats = "{";
    stats += std::format("\"total\":{},", report.stats.total());
    stats += "\"summary\":";
    appendJsonString(stats, report.stats.summary());
    stats.push_back('}');
    appendRaw(out, "stats", stats, first);

    out.push_back('}');
    return out;
}

// ------------------------------------------------------------------ URL --

int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// Decodes `%XX` escapes and `+`. A malformed escape is kept verbatim rather
/// than dropped, so nothing a user typed disappears silently.
std::string urlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hexDigit(text[i + 1]);
            const int low = hexDigit(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                out.push_back('%');
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string_view pathOf(std::string_view target) noexcept {
    const std::size_t question = target.find('?');
    return question == std::string_view::npos ? target : target.substr(0, question);
}

std::string_view queryOf(std::string_view target) noexcept {
    const std::size_t question = target.find('?');
    return question == std::string_view::npos ? std::string_view{} : target.substr(question + 1);
}

/// Returns the decoded value of `key`, or an empty string if it is absent.
std::string queryValue(std::string_view query, std::string_view key) {
    std::size_t position = 0;
    while (position <= query.size()) {
        std::size_t end = query.find('&', position);
        if (end == std::string_view::npos) {
            end = query.size();
        }
        const std::string_view pair = query.substr(position, end - position);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos && pair.substr(0, equals) == key) {
            return urlDecode(pair.substr(equals + 1));
        }
        if (end == query.size()) {
            break;
        }
        position = end + 1;
    }
    return {};
}

// --------------------------------------------------------------- server --

std::string_view statusText(int status) noexcept {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        default:
            return "Internal Server Error";
    }
}

/// Reads one complete request: headers, then exactly Content-Length bytes.
/// Returns false if the peer disconnects, the request is malformed, or it
/// exceeds `kMaxRequestBytes`.
bool readRequest(Socket socket, std::string& request) {
    std::array<char, 8192> buffer{};
    std::size_t header_end = std::string::npos;

    while (true) {
        if (header_end == std::string::npos) {
            header_end = request.find("\r\n\r\n");
        }
        if (header_end != std::string::npos) {
            // Headers are complete; work out whether a body is still coming.
            std::size_t content_length = 0;
            const std::string_view headers{request.data(), header_end};
            std::size_t at = 0;
            while (at < headers.size()) {
                std::size_t line_end = headers.find("\r\n", at);
                if (line_end == std::string_view::npos) {
                    line_end = headers.size();
                }
                const std::string_view line = headers.substr(at, line_end - at);
                const std::size_t colon = line.find(':');
                if (colon != std::string_view::npos) {
                    std::string name{line.substr(0, colon)};
                    std::transform(name.begin(), name.end(), name.begin(),
                                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
                    if (name == "content-length") {
                        std::string_view value = line.substr(colon + 1);
                        while (!value.empty() && value.front() == ' ') {
                            value.remove_prefix(1);
                        }
                        std::from_chars(value.data(), value.data() + value.size(), content_length);
                    }
                }
                if (line_end == headers.size()) {
                    break;
                }
                at = line_end + 2;
            }
            if (content_length > kMaxRequestBytes) {
                return false;
            }
            if (request.size() >= header_end + 4 + content_length) {
                return true;
            }
        }

        if (request.size() > kMaxRequestBytes) {
            return false;
        }
        const auto received = recv(socket, buffer.data(), static_cast<SendSize>(buffer.size()), 0);
        if (received <= 0) {
            return false;
        }
        request.append(buffer.data(), static_cast<std::size_t>(received));
    }
}

void sendAll(Socket socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto written =
            send(socket, data.data() + sent, static_cast<SendSize>(data.size() - sent), 0);
        if (written <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(written);
    }
}

void sendResponse(Socket socket, const HttpResponse& response) {
    std::string head = std::format(
        "HTTP/1.1 {} {}\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        response.status, statusText(response.status), response.content_type, response.body.size());
    sendAll(socket, head);
    sendAll(socket, response.body);
}

}  // namespace

HttpResponse handleRequest(std::string_view method, std::string_view target,
                           std::string_view body) {
    const std::string_view path = pathOf(target);

    if (path == "/" || path == "/index.html") {
        if (method != "GET") {
            return {405, "text/plain; charset=utf-8", "method not allowed\n"};
        }
        return {200, "text/html; charset=utf-8", std::string{kIndexHtml}};
    }

    if (path == "/api/run") {
        if (method != "POST") {
            return {405, "text/plain; charset=utf-8", "method not allowed\n"};
        }
        if (body.size() > kMaxSourceBytes) {
            return {413, "text/plain; charset=utf-8", "source too large\n"};
        }

        const std::string_view query = queryOf(target);
        RunRequest request;
        request.source = std::string{body};
        request.input = queryValue(query, "stdin");
        request.opt_level =
            queryValue(query, "opt") == "1" ? optimizer::OptLevel::O1 : optimizer::OptLevel::O0;

        const RunReport report = runSource(request);
        return {200, "application/json; charset=utf-8", toJson(report)};
    }

    return {404, "text/plain; charset=utf-8", "not found\n"};
}

int serve(const ServeOptions& options) {
#ifdef _WIN32
    WSADATA winsock_data{};
    if (::WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        std::fputs("error: could not initialise Winsock\n", stderr);
        return 1;
    }
#endif

    const Socket listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) {
        std::fputs("error: could not create a socket\n", stderr);
        return 1;
    }

    // Without SO_REUSEADDR, restarting the server after a request fails for as
    // long as the previous socket sits in TIME_WAIT.
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1) {
        std::fprintf(stderr, "error: '%s' is not an IPv4 address\n", options.host.c_str());
        closeSocket(listener);
        return 1;
    }

    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "error: could not bind %s:%u (is it already in use?)\n",
                     options.host.c_str(), static_cast<unsigned>(options.port));
        closeSocket(listener);
        return 1;
    }
    if (listen(listener, 16) != 0) {
        std::fputs("error: could not listen on the socket\n", stderr);
        closeSocket(listener);
        return 1;
    }

    std::printf("minitool playground on http://%s:%u/  (Ctrl+C to stop)\n", options.host.c_str(),
                static_cast<unsigned>(options.port));
    std::fflush(stdout);

    // One connection at a time. A playground serves one person; handling
    // requests sequentially means there is no shared state to get wrong, and a
    // run that hits the instruction budget delays only the next request.
    while (true) {
        const Socket client = accept(listener, nullptr, nullptr);
        if (client == kInvalidSocket) {
            continue;
        }

        std::string request;
        if (!readRequest(client, request)) {
            closeSocket(client);
            continue;
        }

        const std::size_t line_end = request.find("\r\n");
        const std::size_t header_end = request.find("\r\n\r\n");
        if (line_end == std::string::npos || header_end == std::string::npos) {
            sendResponse(client, {400, "text/plain; charset=utf-8", "bad request\n"});
            closeSocket(client);
            continue;
        }

        const std::string_view line{request.data(), line_end};
        const std::size_t first_space = line.find(' ');
        const std::size_t second_space = first_space == std::string_view::npos
                                             ? std::string_view::npos
                                             : line.find(' ', first_space + 1);
        if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
            sendResponse(client, {400, "text/plain; charset=utf-8", "bad request line\n"});
            closeSocket(client);
            continue;
        }

        const std::string_view method = line.substr(0, first_space);
        const std::string_view target =
            line.substr(first_space + 1, second_space - first_space - 1);
        const std::string_view body{request.data() + header_end + 4,
                                    request.size() - header_end - 4};

        sendResponse(client, handleRequest(method, target, body));
        closeSocket(client);
    }
}

}  // namespace minitool::playground
