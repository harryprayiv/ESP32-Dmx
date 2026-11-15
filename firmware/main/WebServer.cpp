#include "WebServer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if !CONFIG_HTTPD_WS_SUPPORT
#error "Set CONFIG_HTTPD_WS_SUPPORT=y (it is in sdkconfig.defaults; delete sdkconfig and rebuild)"
#endif

// Files embedded by main/CMakeLists.txt.
extern const uint8_t indexHtmlStart[] asm("_binary_index_html_start");
extern const uint8_t indexHtmlEnd[] asm("_binary_index_html_end");
extern const uint8_t appJsGzStart[] asm("_binary_app_js_gz_start");
extern const uint8_t appJsGzEnd[] asm("_binary_app_js_gz_end");

namespace {

constexpr char TAG[] = "WebServer";

constexpr std::size_t MAX_SOCKETS = 7;

template <typename... Handlers>
struct Overloaded : Handlers...
{
    using Handlers::operator()...;
};
template <typename... Handlers>
Overloaded(Handlers...) -> Overloaded<Handlers...>;

esp_err_t sendEmbedded(httpd_req_t* request, const uint8_t* start, const uint8_t* end)
{
    // The page is small and changes with every flash, so never let the
    // browser keep an old copy.
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, reinterpret_cast<const char*>(start), end - start);
}

esp_err_t handleIndex(httpd_req_t* request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return sendEmbedded(request, indexHtmlStart, indexHtmlEnd);
}

esp_err_t handleAppJs(httpd_req_t* request)
{
    httpd_resp_set_type(request, "text/javascript; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    return sendEmbedded(request, appJsGzStart, appJsGzEnd);
}

esp_err_t sendState(httpd_handle_t server, int socket, const Console::StateMessage& state)
{
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = const_cast<uint8_t*>(state.data());  // the send only reads it
    frame.len = state.size();
    return httpd_ws_send_frame_async(server, socket, &frame);
}

// Sends the state to every WebSocket client except the one that caused the
// change. That client already shows what it sent.
void sendStateToOthers(httpd_handle_t server, int sender, const Console::StateMessage& state)
{
    std::array<int, MAX_SOCKETS> sockets{};
    std::size_t count = sockets.size();
    if (httpd_get_client_list(server, &count, sockets.data()) != ESP_OK) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const int socket = sockets[i];
        if (socket == sender || httpd_ws_get_fd_info(server, socket) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }
        if (sendState(server, socket, state) != ESP_OK) {
            ESP_LOGW(TAG, "dropping client on socket %d: send failed", socket);
            httpd_sess_trigger_close(server, socket);
        }
    }
}

// Returning an error from this handler makes the server close the socket.
// That is the response to every protocol violation.
esp_err_t handleSocket(httpd_req_t* request)
{
    if (request->method == HTTP_GET) {
        // The handshake has just completed. The client asks for the state
        // with RequestState when it is ready.
        return ESP_OK;
    }

    auto& console = *static_cast<Console*>(request->user_ctx);
    const httpd_handle_t server = request->handle;
    const int socket = httpd_req_to_sockfd(request);

    // With a zero length this only reads the frame header.
    httpd_ws_frame_t frame{};
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(request, &frame, 0), TAG, "reading frame header");

    // A message split over several frames arrives with final unset on its
    // first frame. No valid command is large enough for a browser to split.
    if (frame.type != HTTPD_WS_TYPE_BINARY || !frame.final || frame.len == 0 || frame.len > MAX_COMMAND_SIZE) {
        ESP_LOGW(TAG, "socket %d: rejected frame (type %d, %u bytes)", socket, static_cast<int>(frame.type),
                 static_cast<unsigned>(frame.len));
        return ESP_FAIL;
    }

    std::array<uint8_t, MAX_COMMAND_SIZE> buffer;
    frame.payload = buffer.data();
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(request, &frame, frame.len), TAG, "reading frame payload");

    const std::optional<Command> command = decodeCommand(std::span<const uint8_t>(buffer.data(), frame.len));
    if (!command) {
        ESP_LOGW(TAG, "socket %d: rejected message (opcode 0x%02x, %u bytes)", socket, buffer[0],
                 static_cast<unsigned>(frame.len));
        return ESP_FAIL;
    }

    return std::visit(
        Overloaded{
            [&](const SetSlots& slots) {
                console.setSlots(slots);
                sendStateToOthers(server, socket, console.stateMessage());
                return ESP_OK;
            },
            [&](const SetMaster& master) {
                console.setMaster(master.level);
                sendStateToOthers(server, socket, console.stateMessage());
                return ESP_OK;
            },
            [&](const RequestState&) { return sendState(server, socket, console.stateMessage()); },
        },
        *command);
}

}  // namespace

esp_err_t startWebServer(Console& console)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = MAX_SOCKETS;
    // The socket handler keeps a command buffer, a state message and a
    // rendered universe on the stack.
    config.stack_size = 8192;
    // A phone that walks out of range never closes its socket. Probe idle
    // connections and reuse the oldest one when the table is full.
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    // Sends run on the server task. A stalled client may hold it this many
    // seconds before it is dropped. DMX output does not depend on this task.
    config.send_wait_timeout = 2;

    httpd_handle_t server = nullptr;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "httpd_start");

    httpd_uri_t index{};
    index.uri = "/";
    index.method = HTTP_GET;
    index.handler = handleIndex;

    httpd_uri_t appJs{};
    appJs.uri = "/app.js";
    appJs.method = HTTP_GET;
    appJs.handler = handleAppJs;

    httpd_uri_t socket{};
    socket.uri = "/ws";
    socket.method = HTTP_GET;
    socket.handler = handleSocket;
    socket.user_ctx = &console;
    socket.is_websocket = true;

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &index), TAG, "registering /");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &appJs), TAG, "registering /app.js");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &socket), TAG, "registering /ws");
    return ESP_OK;
}
