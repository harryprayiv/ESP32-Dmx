// Esp32Dmx - DMX512 transmitter for ESP32 on ESP-IDF.
// Port of the transmit half of TeensyDmx. License and credits: Esp32Dmx.h.
//
// TeensyDmx chains one byte per TX-complete interrupt and makes the break by
// dropping the baud rate. The ESP32 UART has a 128 byte FIFO that the ESP-IDF
// driver refills from its own interrupt, and a break generator that holds the
// line low for a set number of bit times once the FIFO has drained. So the
// state machine becomes a task that hands the driver one whole frame at a
// time, and the break is timed by the UART hardware.
//
// On the wire each cycle is: start code, 512 slots, break, mark until the
// next cycle. The break that ends one frame is the break that starts the
// next one.

#include "Esp32Dmx.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "Esp32Dmx";

constexpr int DMXSPEED = 250000;  // one bit is 4 us

// 44 bit times is 176 us. E1.11 requires at least 92 us from a transmitter.
constexpr int BREAK_BITS = 44;

// Shortest mark after break this driver will produce. E1.11 requires at
// least 12 us. The mark is normally far longer, because the task sleeps
// until the next frame is due.
constexpr uint32_t MIN_MARK_AFTER_BREAK_US = 20;

// 40 Hz. A full frame with its break takes 22.75 ms.
constexpr TickType_t FRAME_PERIOD = pdMS_TO_TICKS(25);

// The driver refuses to install without a receive buffer larger than the
// hardware FIFO, even though nothing here receives.
constexpr int UNUSED_RX_BUFFER = 256;

constexpr uint32_t TASK_STACK_BYTES = 4096;
constexpr UBaseType_t TASK_PRIORITY = 20;

// Wi-Fi runs on core 0. On dual core parts the transmit task and the UART
// interrupt, which is allocated on the core that installs the driver, go on
// core 1.
constexpr BaseType_t TASK_CORE = (configNUMBER_OF_CORES > 1) ? 1 : 0;

}  // namespace

Esp32Dmx::Esp32Dmx(uart_port_t uart, gpio_num_t txPin, gpio_num_t redePin) :
    Esp32Dmx(uart, txPin, std::optional<gpio_num_t>(redePin))
{
}

Esp32Dmx::Esp32Dmx(uart_port_t uart, gpio_num_t txPin) :
    Esp32Dmx(uart, txPin, std::optional<gpio_num_t>())
{
}

Esp32Dmx::Esp32Dmx(uart_port_t uart, gpio_num_t txPin, std::optional<gpio_num_t> redePin) :
    m_uart(uart),
    m_txPin(txPin),
    m_redePin(redePin),
    m_handshake(xSemaphoreCreateBinaryStatic(&m_handshakeStorage))
{
    if (m_redePin) {
        gpio_config_t config{};
        config.pin_bit_mask = 1ULL << *m_redePin;
        config.mode = GPIO_MODE_OUTPUT;
        config.pull_up_en = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        gpio_set_level(*m_redePin, 0);
        ESP_ERROR_CHECK(gpio_config(&config));
    }
}

Esp32Dmx::~Esp32Dmx()
{
    if (m_mode == Mode::Out) {
        stopTransmit();
    }
}

esp_err_t Esp32Dmx::setMode(Mode mode)
{
    if (mode == m_mode) {
        return ESP_OK;
    }

    // Stop what we were doing
    switch (m_mode) {
        case Mode::Out:
            stopTransmit();
            break;
        case Mode::Off:
            break;
    }
    m_mode = Mode::Off;

    switch (mode) {
        case Mode::Out: {
            const esp_err_t err = startTransmit();
            if (err != ESP_OK) {
                return err;
            }
            break;
        }
        case Mode::Off:
            break;
    }
    m_mode = mode;
    return ESP_OK;
}

void Esp32Dmx::setChannel(uint16_t address, uint8_t value)
{
    if (address < DMX_BUFFER_SIZE) {
        const std::lock_guard lock(m_bufferMutex);
        m_dmxBuffer[address] = value;
    }
}

void Esp32Dmx::setChannels(uint16_t startAddress, std::span<const uint8_t> values)
{
    const std::lock_guard lock(m_bufferMutex);
    m_dmxBuffer.fill(0);
    if (startAddress < DMX_BUFFER_SIZE) {
        const std::size_t count = std::min(values.size(), DMX_BUFFER_SIZE - startAddress);
        std::copy_n(values.begin(), count, m_dmxBuffer.begin() + startAddress);
    }
}

esp_err_t Esp32Dmx::startTransmit()
{
    m_stopRequested.store(false);
    const BaseType_t created = xTaskCreatePinnedToCore(
        txTaskEntry, "dmx_tx", TASK_STACK_BYTES, this, TASK_PRIORITY, nullptr, TASK_CORE);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(m_handshake, portMAX_DELAY);
    return m_startResult;
}

void Esp32Dmx::stopTransmit()
{
    m_stopRequested.store(true);
    xSemaphoreTake(m_handshake, portMAX_DELAY);
}

esp_err_t Esp32Dmx::openUart()
{
    uart_config_t config{};
    config.baud_rate = DMXSPEED;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_2;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    // No transmit ring buffer. With a zero size, uart_write_bytes_with_break
    // blocks until the data and the break have both left the pin.
    esp_err_t err = uart_driver_install(m_uart, UNUSED_RX_BUFFER, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(m_uart, &config);
    if (err == ESP_OK) {
        err = uart_set_pin(m_uart, m_txPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        // The receiver has no pin assigned. Keep whatever its input floats
        // at from raising interrupts.
        err = uart_disable_rx_intr(m_uart);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart setup: %s", esp_err_to_name(err));
        uart_driver_delete(m_uart);
        return err;
    }

    setDirection(true);
    return ESP_OK;
}

void Esp32Dmx::closeUart()
{
    setDirection(false);
    uart_driver_delete(m_uart);
}

void Esp32Dmx::setDirection(bool transmit)
{
    if (m_redePin) {
        gpio_set_level(*m_redePin, transmit ? 1 : 0);
    }
}

void Esp32Dmx::txTaskEntry(void* self)
{
    static_cast<Esp32Dmx*>(self)->txTask();
    vTaskDelete(nullptr);
}

void Esp32Dmx::txTask()
{
    m_startResult = openUart();
    const bool opened = (m_startResult == ESP_OK);
    xSemaphoreGive(m_handshake);  // startTransmit is waiting for the result
    if (!opened) {
        return;
    }

    // frame[0] is the NULL start code and stays zero.
    std::array<uint8_t, 1 + DMX_BUFFER_SIZE> frame{};
    TickType_t lastWake = xTaskGetTickCount();

    while (!m_stopRequested.load()) {
        {
            const std::lock_guard lock(m_bufferMutex);
            std::memcpy(frame.data() + 1, m_dmxBuffer.data(), DMX_BUFFER_SIZE);
        }

        // Returns once the break has finished and the line is back at mark.
        // It can only fail on a bad port, a missing driver or a break length
        // outside 1 to 255, none of which can happen here.
        const int written = uart_write_bytes_with_break(m_uart, frame.data(), frame.size(), BREAK_BITS);
        assert(written == static_cast<int>(frame.size()));
        (void)written;

        // Guarantees the minimum mark after break even when this task is
        // already late for the next frame and does not sleep below.
        esp_rom_delay_us(MIN_MARK_AFTER_BREAK_US);

        m_frameCount.fetch_add(1, std::memory_order_relaxed);

        if (xTaskDelayUntil(&lastWake, FRAME_PERIOD) == pdFALSE) {
            // The frame overran its period. Start timing again from now.
            lastWake = xTaskGetTickCount();
        }
    }

    closeUart();
    xSemaphoreGive(m_handshake);  // stopTransmit is waiting
}
