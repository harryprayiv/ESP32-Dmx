#pragma once

#include "Console.h"
#include "esp_err.h"

// Starts the HTTP server: the page at /, its script at /app.js and the
// console WebSocket at /ws. The console must outlive the server.
[[nodiscard]] esp_err_t startWebServer(Console& console);
