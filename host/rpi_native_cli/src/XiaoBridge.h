#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Host side of the xiao-sx1262-usb-bridge firmware
// (https://github.com/engperini/xiao-sx1262-usb-bridge): a Seeed XIAO nRF52840
// with a Wio-SX1262 board that shows up as a USB CDC-ACM serial port
// (2886:8044) and moves SPI bytes and the radio pins over a small framed
// protocol (docs/PROTOCOL.md in that repository, version 1).
//
// Frames are `A5 type seq len(u16 LE) payload`. Responses use type|0x80 and
// the request's seq, with a status byte first. DIO1 rising edges arrive as
// unsolicited 0xC1 events, possibly between a request and its response; they
// are counted wherever frames are read, so the radio driver can skip polling
// the SX1262 IRQ register while nothing happened.
//
// Pins are the bridge's logical ids, not MCU pins.
class XiaoBridge {
public:
  static constexpr int PIN_NSS = 0;
  static constexpr int PIN_RESET = 1;
  static constexpr int PIN_BUSY = 2;
  static constexpr int PIN_DIO1 = 3;
  static constexpr int PIN_RXEN = 4;
  static constexpr int PIN_TXEN = 5;

  // Radio wiring reported by HELLO.
  struct BoardInfo {
    uint8_t protocol = 0;
    uint8_t board_id = 0;
    uint8_t fw_major = 0, fw_minor = 0, fw_patch = 0;
    bool dio1_events = false;
    bool has_rxen = false;
    bool has_txen = false;
    bool dio2_rf_switch = false;
    int tcxo_mv = 0;  // 0 = crystal
    size_t max_transfer = 0;
    std::string name;
  };

  XiaoBridge() = default;
  ~XiaoBridge();

  XiaoBridge(const XiaoBridge&) = delete;
  XiaoBridge& operator=(const XiaoBridge&) = delete;

  // Opens the serial port, asserts DTR, runs HELLO and enables DIO1 events.
  // An empty path picks the first /dev/serial/by-id/usb-Seeed_XIAO_nRF52840_*
  // entry, then /dev/ttyACM0. Throws std::runtime_error on failure.
  void open(const std::string& port);
  void close();
  bool isOpen() const { return fd >= 0; }
  const std::string& portPath() const { return port_path; }
  const BoardInfo& info() const { return board; }

  void digitalWrite(int pin, bool high);
  bool digitalRead(int pin);

  // One SX1262 command: NSS low, wait for BUSY low, transfer, NSS high, in a
  // single USB round trip (split into NSS-held chunks if it is too long).
  void spiCommand(const uint8_t* tx, uint8_t* rx, size_t len);

  // Pulses RESET and waits for BUSY low.
  void radioReset();

  // Reads any frames already waiting, without blocking, and returns the DIO1
  // edge counter reported by the bridge (0 until the first edge).
  uint32_t pollDio1();
  bool dio1EventsEnabled() const { return irq_enabled; }

private:
  int fd = -1;
  std::string port_path;
  uint8_t seq = 0;
  bool irq_enabled = false;
  uint32_t dio1_count = 0;
  BoardInfo board;
  std::vector<uint8_t> rx_buf;
  std::vector<uint8_t> frame;
  std::vector<uint8_t> reply;

  uint8_t request(uint8_t type, const uint8_t* payload, size_t len, int timeout_ms);
  void requestOk(uint8_t type, const uint8_t* payload, size_t len, int timeout_ms = 500);
  // Parses buffered frames; returns true when the response (type, seq) was found.
  bool parseFrames(uint8_t want_type, uint8_t want_seq, uint8_t* status);
  int readSome(int timeout_ms);
  void writeAll(const uint8_t* buf, size_t len);
  void drainInput();
  void hello();
  void xfer(uint8_t flags, const uint8_t* tx, uint8_t* rx, size_t len);
};
