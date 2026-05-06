#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <WinSock2.h>
#include <WS2tcpip.h>
using socket_handle = SOCKET;
constexpr socket_handle invalid_socket_handle = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
constexpr socket_handle invalid_socket_handle = -1;
#endif

namespace {

std::atomic_bool g_running{true};

void handle_signal(int) {
    g_running = false;
}

void close_socket(socket_handle socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

std::string socket_error_message() {
#ifdef _WIN32
    return "WSA error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

class SocketRuntime {
public:
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
#endif
    }

    ~SocketRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

int parse_port(int argc, char* argv[]) {
    int port = 8080;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " -p <port>\n";
            std::exit(0);
        }
        if (arg == "-p") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value after -p");
            }
            char* end = nullptr;
            const long parsed = std::strtol(argv[++i], &end, 10);
            if (*argv[i] == '\0' || *end != '\0' || parsed < 1 || parsed > 65535) {
                throw std::invalid_argument("port must be an integer from 1 to 65535");
            }
            port = static_cast<int>(parsed);
            continue;
        }
        throw std::invalid_argument("unknown argument: " + arg);
    }
    return port;
}

std::string clock_page() {
    return R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Green Diver Clock</title>
  <style>
    :root {
      color-scheme: dark;
      --bezel: #063f31;
      --bezel-dark: #021d17;
      --dial: #041713;
      --gold: #d5b76b;
      --lume: #dff9e9;
      --steel: #d9e2e1;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      display: grid;
      place-items: center;
      background:
        radial-gradient(circle at 50% 35%, rgba(22, 129, 96, 0.28), transparent 30rem),
        linear-gradient(135deg, #05120f 0%, #0a1f1a 45%, #010504 100%);
      font-family: "Segoe UI", Arial, sans-serif;
      color: var(--steel);
    }
    main {
      width: min(92vw, 680px);
      display: grid;
      gap: 22px;
      justify-items: center;
    }
    .watch {
      position: relative;
      width: min(86vw, 520px);
      aspect-ratio: 1;
      border-radius: 50%;
      background:
        repeating-conic-gradient(from -30deg, #083f31 0deg 3deg, #0f6b50 3deg 4deg, #083f31 4deg 6deg),
        radial-gradient(circle at 34% 28%, #168c69 0%, var(--bezel) 38%, var(--bezel-dark) 74%);
      box-shadow:
        0 24px 70px rgba(0, 0, 0, 0.52),
        inset 0 0 0 12px #b8c4c3,
        inset 0 0 0 20px #65716f,
        inset 0 0 44px rgba(0, 0, 0, 0.7);
    }
    .dial {
      position: absolute;
      inset: 14%;
      border-radius: 50%;
      background:
        radial-gradient(circle at 42% 34%, rgba(33, 119, 91, 0.78), transparent 24%),
        radial-gradient(circle, #073327 0%, var(--dial) 66%, #010605 100%);
      box-shadow: inset 0 0 0 4px #c8d3d0, inset 0 0 36px #000;
    }
    .marker {
      position: absolute;
      left: 50%;
      top: 50%;
      width: 4.2%;
      height: 12.5%;
      transform: translate(-50%, -100%) rotate(var(--angle)) translateY(-165%);
      transform-origin: 50% 100%;
      border: 2px solid var(--gold);
      border-radius: 999px;
      background: var(--lume);
      box-shadow: 0 0 12px rgba(223, 249, 233, 0.45);
    }
    .marker.triangle {
      width: 0;
      height: 0;
      border-left: 16px solid transparent;
      border-right: 16px solid transparent;
      border-bottom: 32px solid var(--lume);
      background: transparent;
      border-top: 0;
      border-radius: 0;
      filter: drop-shadow(0 0 8px rgba(223, 249, 233, 0.42));
    }
    .marker.wide {
      width: 11%;
      height: 4.2%;
      transform: translate(-50%, -50%) rotate(var(--angle)) translateX(268%);
    }
    .hand {
      position: absolute;
      left: 50%;
      top: 50%;
      transform: translate(-50%, -100%) rotate(var(--angle));
      transform-origin: 50% 100%;
      border-radius: 999px;
      box-shadow: 0 0 10px rgba(0, 0, 0, 0.55);
    }
    .hour {
      width: 3.5%;
      height: 25%;
      background: linear-gradient(var(--steel), #96a3a1);
    }
    .minute {
      width: 2.4%;
      height: 34%;
      background: linear-gradient(var(--steel), #8c9997);
    }
    .second {
      width: 1%;
      height: 39%;
      background: #e34242;
      box-shadow: 0 0 12px rgba(227, 66, 66, 0.4);
    }
    .cap {
      position: absolute;
      left: 50%;
      top: 50%;
      width: 7.5%;
      aspect-ratio: 1;
      transform: translate(-50%, -50%);
      border-radius: 50%;
      background: radial-gradient(circle, #f8f5df 0 28%, var(--gold) 30% 58%, #6b5520 62% 100%);
      box-shadow: 0 0 12px rgba(0, 0, 0, 0.6);
    }
    .brand {
      position: absolute;
      left: 50%;
      top: 30%;
      transform: translateX(-50%);
      color: var(--gold);
      font-size: clamp(14px, 3vw, 22px);
      font-weight: 700;
      letter-spacing: 0;
      text-align: center;
    }
    .date {
      position: absolute;
      right: 20%;
      top: 47%;
      min-width: 42px;
      padding: 5px 8px;
      border: 2px solid #c8d3d0;
      background: #f5f6ee;
      color: #06120f;
      font-size: clamp(16px, 4vw, 26px);
      font-weight: 700;
      text-align: center;
    }
    .digital {
      padding: 10px 18px;
      border: 1px solid rgba(213, 183, 107, 0.5);
      border-radius: 8px;
      background: rgba(2, 19, 15, 0.72);
      color: var(--lume);
      font-size: clamp(18px, 4vw, 28px);
      font-variant-numeric: tabular-nums;
    }
  </style>
</head>
<body>
  <main>
    <section class="watch" aria-label="green diver style analog clock">
      <div class="dial">
        <div class="brand">GREEN DIVER<br>C++17</div>
        <div id="date" class="date">--</div>
        <div class="marker triangle" style="--angle: 0deg"></div>
        <div class="marker" style="--angle: 30deg"></div>
        <div class="marker wide" style="--angle: 90deg"></div>
        <div class="marker" style="--angle: 60deg"></div>
        <div class="marker wide" style="--angle: 180deg"></div>
        <div class="marker" style="--angle: 120deg"></div>
        <div class="marker" style="--angle: 150deg"></div>
        <div class="marker" style="--angle: 210deg"></div>
        <div class="marker wide" style="--angle: 270deg"></div>
        <div class="marker" style="--angle: 240deg"></div>
        <div class="marker" style="--angle: 300deg"></div>
        <div class="marker" style="--angle: 330deg"></div>
        <div id="hour" class="hand hour"></div>
        <div id="minute" class="hand minute"></div>
        <div id="second" class="hand second"></div>
        <div class="cap"></div>
      </div>
    </section>
    <div id="digital" class="digital">--:--:--</div>
  </main>
  <script>
    const hour = document.getElementById("hour");
    const minute = document.getElementById("minute");
    const second = document.getElementById("second");
    const digital = document.getElementById("digital");
    const date = document.getElementById("date");

    function tick() {
      const now = new Date();
      const ms = now.getMilliseconds();
      const s = now.getSeconds() + ms / 1000;
      const m = now.getMinutes() + s / 60;
      const h = (now.getHours() % 12) + m / 60;
      hour.style.setProperty("--angle", `${h * 30}deg`);
      minute.style.setProperty("--angle", `${m * 6}deg`);
      second.style.setProperty("--angle", `${s * 6}deg`);
      digital.textContent = now.toLocaleTimeString("zh-CN", { hour12: false });
      date.textContent = String(now.getDate()).padStart(2, "0");
      requestAnimationFrame(tick);
    }
    tick();
  </script>
</body>
</html>)HTML";
}

std::string http_response(const std::string& body, const std::string& status, const std::string& content_type) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "Cache-Control: no-store\r\n"
             << "\r\n"
             << body;
    return response.str();
}

void send_all(socket_handle client, const std::string& data) {
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const int sent = send(client, cursor, static_cast<int>(remaining), 0);
        if (sent <= 0) {
            return;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
}

void handle_client(socket_handle client) {
    char buffer[2048]{};
    const int received = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        close_socket(client);
        return;
    }

    const std::string request(buffer, static_cast<std::size_t>(received));
    const std::size_t line_end = request.find("\r\n");
    const std::string request_line = request.substr(0, line_end == std::string::npos ? request.size() : line_end);
    const bool is_root = request_line == "GET / HTTP/1.1" || request_line == "GET / HTTP/1.0";
    const bool is_health = request_line == "GET /health HTTP/1.1" || request_line == "GET /health HTTP/1.0";

    if (is_root) {
        send_all(client, http_response(clock_page(), "200 OK", "text/html; charset=utf-8"));
    } else if (is_health) {
        send_all(client, http_response("ok\n", "200 OK", "text/plain; charset=utf-8"));
    } else {
        send_all(client, http_response("not found\n", "404 Not Found", "text/plain; charset=utf-8"));
    }
    close_socket(client);
}

socket_handle create_server_socket(int port) {
    const socket_handle server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == invalid_socket_handle) {
        throw std::runtime_error("socket failed: " + socket_error_message());
    }

    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port));

    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string message = socket_error_message();
        close_socket(server);
        throw std::runtime_error("bind failed: " + message);
    }
    if (listen(server, 16) != 0) {
        const std::string message = socket_error_message();
        close_socket(server);
        throw std::runtime_error("listen failed: " + message);
    }
    return server;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        const int port = parse_port(argc, argv);
        const SocketRuntime runtime;
        const socket_handle server = create_server_socket(port);

        std::cout << "Submariner-style clock server listening on http://127.0.0.1:" << port << "/\n";
        std::cout << "Press Ctrl+C to stop.\n";

        while (g_running) {
            sockaddr_in client_address{};
#ifdef _WIN32
            int address_length = sizeof(client_address);
#else
            socklen_t address_length = sizeof(client_address);
#endif
            const socket_handle client = accept(server, reinterpret_cast<sockaddr*>(&client_address), &address_length);
            if (client == invalid_socket_handle) {
                if (g_running) {
                    std::cerr << "accept failed: " << socket_error_message() << "\n";
                }
                continue;
            }
            handle_client(client);
        }

        close_socket(server);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        std::cerr << "Usage: " << argv[0] << " -p <port>\n";
        return 1;
    }
}
