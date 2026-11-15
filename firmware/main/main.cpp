#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "Console.h"
#include "Esp32Dmx.h"
#include "WebServer.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

namespace {

constexpr char TAG[] = "main";

constexpr char AP_SSID[] = CONFIG_DMX_AP_SSID;
constexpr char AP_PASSWORD[] = CONFIG_DMX_AP_PASSWORD;

static_assert(sizeof(AP_SSID) - 1 >= 1 && sizeof(AP_SSID) - 1 <= 32, "DMX_AP_SSID must be 1 to 32 characters");
static_assert(sizeof(AP_PASSWORD) - 1 >= 8 && sizeof(AP_PASSWORD) - 1 <= 63,
              "DMX_AP_PASSWORD must be 8 to 63 characters (WPA2)");
static_assert(CONFIG_DMX_UART_NUM < SOC_UART_HP_NUM, "DMX_UART_NUM does not exist on this chip");

void startNvs()
{
    // The Wi-Fi driver keeps its calibration data in NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void startAccessPoint()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t config{};
    std::memcpy(config.ap.ssid, AP_SSID, sizeof(AP_SSID) - 1);
    config.ap.ssid_len = sizeof(AP_SSID) - 1;
    std::memcpy(config.ap.password, AP_PASSWORD, sizeof(AP_PASSWORD) - 1);
    config.ap.channel = CONFIG_DMX_AP_CHANNEL;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

}  // namespace

extern "C" void app_main()
{
    const auto uart = static_cast<uart_port_t>(CONFIG_DMX_UART_NUM);
    const auto txPin = static_cast<gpio_num_t>(CONFIG_DMX_TX_GPIO);
#if CONFIG_DMX_REDE_GPIO >= 0
    static Esp32Dmx dmx(uart, txPin, static_cast<gpio_num_t>(CONFIG_DMX_REDE_GPIO));
#else
    static Esp32Dmx dmx(uart, txPin);
#endif
    static Console console(dmx);

    // DMX first: fixtures get a valid all-zero universe before Wi-Fi is up.
    ESP_ERROR_CHECK(dmx.setMode(Esp32Dmx::Mode::Out));

    startNvs();
    startAccessPoint();
    ESP_ERROR_CHECK(startWebServer(console));

    ESP_LOGI(TAG, "join Wi-Fi \"%s\" and open http://192.168.4.1/", AP_SSID);

    // Health line for the serial monitor. 40 Hz gives 400 frames per line.
    uint32_t before = dmx.frameCount();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const uint32_t now = dmx.frameCount();
        ESP_LOGI(TAG, "dmx: %" PRIu32 " frames in the last 10 s", now - before);
        before = now;
    }
}
