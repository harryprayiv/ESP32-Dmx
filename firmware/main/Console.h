// Console state and the wire protocol of the /ws WebSocket.
//
// Every message is one binary WebSocket frame. The first byte is the opcode.
//
//   client to server
//     0x01 SetSlots      1 to 512 triplets: slot high byte, slot low byte, level.
//                        Slots are 0 to 511. All triplets take effect in the same DMX frame.
//     0x02 SetMaster     one byte, the grand master level.
//     0x03 RequestState  no payload.
//
//   server to client
//     0x81 State         the grand master byte, then 512 level bytes.
//
// A message that does not match this exactly is rejected whole.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <variant>

#include "Esp32Dmx.h"

// A SetSlots payload that has passed validation. It can only be made by
// parse, so holding one means every slot in it is in range.
class SetSlots
{
  public:
    static constexpr std::size_t TRIPLET = 3;
    static constexpr std::size_t MAX_PAYLOAD = TRIPLET * DMX_BUFFER_SIZE;

    static std::optional<SetSlots> parse(std::span<const uint8_t> payload)
    {
        if (payload.empty() || payload.size() > MAX_PAYLOAD || payload.size() % TRIPLET != 0) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < payload.size(); i += TRIPLET) {
            if (slotAt(payload, i) >= DMX_BUFFER_SIZE) {
                return std::nullopt;
            }
        }
        return SetSlots(payload);
    }

    void applyTo(std::array<uint8_t, DMX_BUFFER_SIZE>& levels) const
    {
        for (std::size_t i = 0; i < m_payload.size(); i += TRIPLET) {
            levels[slotAt(m_payload, i)] = m_payload[i + 2];
        }
    }

  private:
    explicit SetSlots(std::span<const uint8_t> payload) : m_payload(payload) { }

    static std::size_t slotAt(std::span<const uint8_t> payload, std::size_t i)
    {
        return (static_cast<std::size_t>(payload[i]) << 8) | payload[i + 1];
    }

    std::span<const uint8_t> m_payload;  // borrowed from the received frame
};

struct SetMaster
{
    uint8_t level;
};

struct RequestState
{
};

using Command = std::variant<SetSlots, SetMaster, RequestState>;

// The longest valid client message: the opcode and 512 triplets.
inline constexpr std::size_t MAX_COMMAND_SIZE = 1 + SetSlots::MAX_PAYLOAD;

inline std::optional<Command> decodeCommand(std::span<const uint8_t> message)
{
    if (message.empty()) {
        return std::nullopt;
    }
    const std::span<const uint8_t> payload = message.subspan(1);
    switch (message[0]) {
        case 0x01:
            if (const auto slots = SetSlots::parse(payload)) {
                return Command(*slots);
            }
            return std::nullopt;
        case 0x02:
            if (payload.size() == 1) {
                return Command(SetMaster{payload[0]});
            }
            return std::nullopt;
        case 0x03:
            if (payload.empty()) {
                return Command(RequestState{});
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

// Channel levels and the grand master. The DMX output is every level scaled
// by the master. Safe to call from any task.
class Console
{
  public:
    using StateMessage = std::array<uint8_t, 2 + DMX_BUFFER_SIZE>;

    // Pushes the all-zero universe to the driver.
    explicit Console(Esp32Dmx& dmx) : m_dmx(dmx) { render(); }

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    void setSlots(const SetSlots& slots)
    {
        const std::lock_guard lock(m_mutex);
        slots.applyTo(m_levels);
        render();
    }

    void setMaster(uint8_t level)
    {
        const std::lock_guard lock(m_mutex);
        m_master = level;
        render();
    }

    // The 0x81 State message for the current state.
    StateMessage stateMessage() const
    {
        const std::lock_guard lock(m_mutex);
        StateMessage message;
        message[0] = 0x81;
        message[1] = m_master;
        std::copy(m_levels.begin(), m_levels.end(), message.begin() + 2);
        return message;
    }

  private:
    // Call with m_mutex held, so outputs reach the driver in the order the
    // state changed.
    void render()
    {
        std::array<uint8_t, DMX_BUFFER_SIZE> output;
        for (std::size_t i = 0; i < DMX_BUFFER_SIZE; ++i) {
            output[i] = static_cast<uint8_t>((m_levels[i] * m_master + 127) / 255);
        }
        m_dmx.setChannels(0, output);
    }

    Esp32Dmx& m_dmx;
    mutable std::mutex m_mutex;
    std::array<uint8_t, DMX_BUFFER_SIZE> m_levels{};
    uint8_t m_master = 255;
};
