/* Esp32Dmx - DMX512 transmitter for ESP32 on ESP-IDF.

   Port of the transmit half of TeensyDmx (https://github.com/chrisstaite/TeensyDmx).
   The transmit API keeps the TeensyDmx names. DMX receive and RDM are not ported.

   Copyright (c) 2017-2018 Peter Newman, Dan Large, http://daniellarge.co.uk
   Copyright (c) 2017 Chris Staite
   Copyright (c) 2014 Jim Paris
   Copyright (c) 2014 Ward
   Copyright (c) 2008-2009 Peter Knight, Tinker.it! All rights reserved.
   Copyright (c) 2011-2013 by Matthias Hertel, http://www.mathertel.de

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

inline constexpr std::size_t DMX_BUFFER_SIZE = 512;

class Esp32Dmx
{
  public:
    // TeensyDmx has DMX_OFF, DMX_IN and DMX_OUT. Receive is not ported, so
    // there is no In mode to select.
    enum class Mode { Off, Out };

    // redePin drives the transceiver's RE and DE pins tied together. It is
    // held high while transmitting and low when off, which leaves the bus
    // undriven.
    Esp32Dmx(uart_port_t uart, gpio_num_t txPin, gpio_num_t redePin);

    // For hardware that enables the RS-485 driver without a GPIO.
    Esp32Dmx(uart_port_t uart, gpio_num_t txPin);

    ~Esp32Dmx();

    Esp32Dmx(const Esp32Dmx&) = delete;
    Esp32Dmx& operator=(const Esp32Dmx&) = delete;

    // Call from one task only. Out starts a task that sends the whole
    // universe 40 times a second. Off stops it and releases the UART.
    [[nodiscard]] esp_err_t setMode(Mode mode);
    Mode mode() const { return m_mode; }

    // The setters may be called from any task at any time, including while
    // Off. A frame never contains half of one call.

    // Use for transmit with addresses from 0-511
    // Will keep all other values as they were previously
    void setChannel(uint16_t address, uint8_t value);

    // Use for transmit with addresses from 1-512
    // Will keep all other values as they were previously
    void setDmxChannel(uint16_t address, uint8_t value)
    {
        setChannel(static_cast<uint16_t>(address - 1), value);
    }

    // Use for transmit with channels from 0-511
    // Will set all other channels to 0
    void setChannels(uint16_t startAddress, std::span<const uint8_t> values);

    // Use for transmit with channels from 1-512
    // Will set all other channels to 0
    void setDmxChannels(uint16_t startAddress, std::span<const uint8_t> values)
    {
        setChannels(static_cast<uint16_t>(startAddress - 1), values);
    }

    // Frames sent since boot. Read it twice to measure the refresh rate.
    uint32_t frameCount() const { return m_frameCount.load(std::memory_order_relaxed); }

  private:
    Esp32Dmx(uart_port_t uart, gpio_num_t txPin, std::optional<gpio_num_t> redePin);

    esp_err_t startTransmit();
    void stopTransmit();

    esp_err_t openUart();
    void closeUart();
    void setDirection(bool transmit);

    static void txTaskEntry(void* self);
    void txTask();

    const uart_port_t m_uart;
    const gpio_num_t m_txPin;
    const std::optional<gpio_num_t> m_redePin;

    Mode m_mode = Mode::Off;

    std::mutex m_bufferMutex;  // guards m_dmxBuffer
    std::array<uint8_t, DMX_BUFFER_SIZE> m_dmxBuffer{};

    // Given by the transmit task: once when it has opened the UART (or
    // failed to), and once when it has closed the UART on the way out.
    StaticSemaphore_t m_handshakeStorage;
    SemaphoreHandle_t m_handshake;
    esp_err_t m_startResult = ESP_OK;

    std::atomic<bool> m_stopRequested{false};
    std::atomic<uint32_t> m_frameCount{0};
};
