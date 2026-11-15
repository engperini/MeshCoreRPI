#include "XiaoBridge.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

// Protocol v1, see docs/PROTOCOL.md in xiao-sx1262-usb-bridge.
constexpr uint8_t SYNC = 0xA5;
constexpr size_t HEADER_LEN = 5;
constexpr size_t MAX_PAYLOAD = 600;
constexpr uint8_t RESPONSE_FLAG = 0x80;
constexpr uint8_t FIRST_EVENT_TYPE = 0xC0;
constexpr uint8_t EVENT_DIO1 = 0xC1;
constexpr uint8_t PROTOCOL_VERSION = 1;

constexpr uint8_t REQ_HELLO = 0x01;
constexpr uint8_t REQ_SPI_TRANSFER = 0x02;
constexpr uint8_t REQ_PIN_WRITE = 0x03;
constexpr uint8_t REQ_PIN_READ = 0x04;
constexpr uint8_t REQ_RADIO_RESET = 0x07;
constexpr uint8_t REQ_IRQ_CONFIG = 0x08;
constexpr uint8_t REQ_WAIT_BUSY = 0x09;

constexpr uint8_t ST_OK = 0x00;
constexpr uint8_t ST_BUSY_TIMEOUT = 0x04;

constexpr uint8_t XFER_AUTO_CS = 0x01;
constexpr uint8_t XFER_WAIT_BUSY = 0x02;

constexpr uint8_t CAP_DIO1_EVENTS = 0x01;
constexpr uint8_t CAP_RXEN = 0x02;
constexpr uint8_t CAP_TXEN = 0x04;
constexpr uint8_t RADIO_FLAG_DIO2_RF_SWITCH = 0x01;

const char* statusName(uint8_t status) {
  switch (status) {
    case 0x00: return "OK";
    case 0x01: return "unknown command";
    case 0x02: return "bad length";
    case 0x03: return "bad pin";
    case 0x04: return "BUSY timeout";
    case 0x05: return "bad argument";
    default: return "unknown status";
  }
}

std::string requestError(uint8_t type, uint8_t status) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "XIAO bridge request 0x%02X failed: %s (0x%02X)", type, statusName(status), status);
  return buf;
}

std::string defaultPort() {
#ifdef __linux__
  const char* dir_path = "/dev/serial/by-id";
  if (DIR* dir = opendir(dir_path)) {
    std::string found;
    while (const dirent* ent = readdir(dir)) {
      const std::string name(ent->d_name);
      if (name.rfind("usb-Seeed_XIAO_nRF52840", 0) == 0 && name.size() > 5 &&
          name.compare(name.size() - 5, 5, "-if00") == 0) {
        if (found.empty() || name < found) found = name;
      }
    }
    closedir(dir);
    if (!found.empty()) return std::string(dir_path) + "/" + found;
  }
#endif
  return "/dev/ttyACM0";
}

}  // namespace

XiaoBridge::~XiaoBridge() {
  close();
}

void XiaoBridge::open(const std::string& port) {
#ifdef __linux__
  if (fd >= 0) return;
  port_path = port.empty() ? defaultPort() : port;

  fd = ::open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("XIAO bridge: cannot open " + port_path + ": " + std::strerror(errno) +
                             " (set RPI_XIAO_PORT, check the USB cable)");
  }

  termios tio{};
  if (tcgetattr(fd, &tio) == 0) {
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, B115200);  // ignored by the bridge
    cfsetospeed(&tio, B115200);
    tcsetattr(fd, TCSANOW, &tio);
  }

  // The bridge only sends while DTR is asserted.
  int bits = TIOCM_DTR | TIOCM_RTS;
  if (ioctl(fd, TIOCMBIS, &bits) < 0) {
    const std::string err = std::strerror(errno);
    close();
    throw std::runtime_error("XIAO bridge: cannot assert DTR on " + port_path + ": " + err);
  }
  tcflush(fd, TCIOFLUSH);

  seq = 0;
  dio1_count = 0;
  irq_enabled = false;
  rx_buf.clear();

  try {
    hello();
    if (board.dio1_events) {
      const uint8_t on = 1;
      requestOk(REQ_IRQ_CONFIG, &on, 1);
      irq_enabled = true;
    }
  } catch (...) {
    close();
    throw;
  }
#else
  (void)port;
  throw std::runtime_error("XIAO bridge requires Linux");
#endif
}

void XiaoBridge::close() {
#ifdef __linux__
  if (fd < 0) return;
  // Dropping DTR makes the bridge park the radio in a safe state.
  int bits = TIOCM_DTR;
  ioctl(fd, TIOCMBIC, &bits);
  ::close(fd);
#endif
  fd = -1;
  irq_enabled = false;
}

// ---------------------------------------------------------------- serial I/O

int XiaoBridge::readSome(int timeout_ms) {
#ifdef __linux__
  pollfd pfd{fd, POLLIN, 0};
  const int pr = ::poll(&pfd, 1, std::max(0, timeout_ms));
  if (pr < 0) {
    if (errno == EINTR) return 0;
    throw std::runtime_error(std::string("XIAO bridge: poll failed: ") + std::strerror(errno));
  }
  if (pr == 0) return 0;
  if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
    throw std::runtime_error("XIAO bridge: serial port closed (device unplugged?)");
  }
  uint8_t chunk[1024];
  const ssize_t n = ::read(fd, chunk, sizeof(chunk));
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR) return 0;
    throw std::runtime_error(std::string("XIAO bridge: read failed: ") + std::strerror(errno));
  }
  if (n == 0) {
    // poll() said readable but nothing came: the device went away.
    throw std::runtime_error("XIAO bridge: serial port closed (device unplugged?)");
  }
  rx_buf.insert(rx_buf.end(), chunk, chunk + n);
  return static_cast<int>(n);
#else
  (void)timeout_ms;
  return 0;
#endif
}

void XiaoBridge::writeAll(const uint8_t* buf, size_t len) {
#ifdef __linux__
  size_t off = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (off < len) {
    const ssize_t n = ::write(fd, buf + off, len - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) {
      throw std::runtime_error(std::string("XIAO bridge: write failed: ") + std::strerror(errno));
    }
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("XIAO bridge: write timed out");
    pollfd pfd{fd, POLLOUT, 0};
    ::poll(&pfd, 1, 20);
  }
#else
  (void)buf; (void)len;
#endif
}

void XiaoBridge::drainInput() {
  for (int i = 0; i < 64; ++i) {
    if (readSome(20) == 0) break;
  }
  rx_buf.clear();
}

bool XiaoBridge::parseFrames(uint8_t want_type, uint8_t want_seq, uint8_t* status) {
  bool found = false;
  while (!found) {
    auto start = std::find(rx_buf.begin(), rx_buf.end(), SYNC);
    rx_buf.erase(rx_buf.begin(), start);
    if (rx_buf.size() < HEADER_LEN) break;
    const size_t plen = static_cast<size_t>(rx_buf[3]) | (static_cast<size_t>(rx_buf[4]) << 8);
    if (plen > MAX_PAYLOAD + 1) {
      rx_buf.erase(rx_buf.begin());  // not a real header; resync on the next 0xA5
      continue;
    }
    if (rx_buf.size() < HEADER_LEN + plen) break;

    const uint8_t ftype = rx_buf[1];
    const uint8_t fseq = rx_buf[2];
    const uint8_t* p = rx_buf.data() + HEADER_LEN;

    if (ftype == EVENT_DIO1 && plen >= 4) {
      dio1_count = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    } else if (ftype >= FIRST_EVENT_TYPE) {
      // Unknown event type: ignore.
    } else if (want_type != 0 && ftype == want_type && fseq == want_seq && plen >= 1) {
      *status = p[0];
      reply.assign(p + 1, p + plen);
      found = true;
    }
    // Stale responses (e.g. from a request that timed out) are dropped.
    rx_buf.erase(rx_buf.begin(), rx_buf.begin() + HEADER_LEN + plen);
  }
  return found;
}

uint8_t XiaoBridge::request(uint8_t type, const uint8_t* payload, size_t len, int timeout_ms) {
  if (fd < 0) throw std::runtime_error("XIAO bridge not open");
  if (len > MAX_PAYLOAD) throw std::runtime_error("XIAO bridge: request too large");

  seq = static_cast<uint8_t>(seq + 1);
  frame.resize(HEADER_LEN + len);
  frame[0] = SYNC;
  frame[1] = type;
  frame[2] = seq;
  frame[3] = static_cast<uint8_t>(len & 0xFF);
  frame[4] = static_cast<uint8_t>(len >> 8);
  if (len > 0) std::memcpy(&frame[HEADER_LEN], payload, len);
  writeAll(frame.data(), frame.size());

  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
  const uint8_t want = static_cast<uint8_t>(type | RESPONSE_FLAG);
  uint8_t status = 0;
  while (true) {
    if (parseFrames(want, seq, &status)) return status;
    const auto now = clock::now();
    if (now >= deadline) {
      char buf[80];
      std::snprintf(buf, sizeof(buf), "XIAO bridge did not answer request 0x%02X", type);
      throw std::runtime_error(buf);
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    readSome(static_cast<int>(left) + 1);
  }
}

void XiaoBridge::requestOk(uint8_t type, const uint8_t* payload, size_t len, int timeout_ms) {
  const uint8_t status = request(type, payload, len, timeout_ms);
  if (status != ST_OK) throw std::runtime_error(requestError(type, status));
}

void XiaoBridge::hello() {
  // A previous host may have left a partial frame behind; the bridge drops it
  // after 200 ms of silence, so retry a few times.
  std::string last_error;
  for (int attempt = 0; attempt < 3; ++attempt) {
    try {
      drainInput();
      requestOk(REQ_HELLO, nullptr, 0, 500);
      last_error.clear();
      break;
    } catch (const std::exception& ex) {
      last_error = ex.what();
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }
  if (!last_error.empty()) {
    throw std::runtime_error("XIAO on " + port_path + " did not answer as a bridge (" + last_error +
                             "); it needs the xiao-sx1262-usb-bridge firmware");
  }
  if (reply.size() < 12) throw std::runtime_error("XIAO bridge: short HELLO reply");
  if (reply[0] != PROTOCOL_VERSION) {
    throw std::runtime_error("XIAO bridge speaks protocol v" + std::to_string(reply[0]) + "; this driver needs v" +
                             std::to_string(PROTOCOL_VERSION));
  }
  board = BoardInfo{};
  board.protocol = reply[0];
  board.board_id = reply[1];
  board.fw_major = reply[2];
  board.fw_minor = reply[3];
  board.fw_patch = reply[4];
  const uint8_t caps = reply[5];
  board.dio1_events = (caps & CAP_DIO1_EVENTS) != 0;
  board.has_rxen = (caps & CAP_RXEN) != 0;
  board.has_txen = (caps & CAP_TXEN) != 0;
  board.dio2_rf_switch = (reply[6] & RADIO_FLAG_DIO2_RF_SWITCH) != 0;
  board.tcxo_mv = reply[7] | (reply[8] << 8);
  board.max_transfer = static_cast<size_t>(reply[9] | (reply[10] << 8));
  if (board.max_transfer == 0 || board.max_transfer > MAX_PAYLOAD - 1) board.max_transfer = MAX_PAYLOAD - 1;
  const size_t name_len = reply[11];
  if (reply.size() >= 12 + name_len) board.name.assign(reply.begin() + 12, reply.begin() + 12 + name_len);
}

// ---------------------------------------------------------------- bus calls

void XiaoBridge::digitalWrite(int pin, bool high) {
  if (pin < 0) return;
  const uint8_t p[2] = {static_cast<uint8_t>(pin), static_cast<uint8_t>(high ? 1 : 0)};
  requestOk(REQ_PIN_WRITE, p, sizeof(p));
}

bool XiaoBridge::digitalRead(int pin) {
  if (pin < 0) return false;
  const uint8_t p = static_cast<uint8_t>(pin);
  requestOk(REQ_PIN_READ, &p, 1);
  return !reply.empty() && reply[0] != 0;
}

void XiaoBridge::radioReset() {
  const uint8_t p[2] = {200, 0};  // BUSY timeout 200 ms
  requestOk(REQ_RADIO_RESET, p, sizeof(p), 1000);
}

uint32_t XiaoBridge::pollDio1() {
  if (fd < 0) return dio1_count;
  while (readSome(0) > 0) {
  }
  uint8_t dummy = 0;
  parseFrames(0, 0, &dummy);
  return dio1_count;
}

void XiaoBridge::xfer(uint8_t flags, const uint8_t* tx, uint8_t* rx, size_t len) {
  std::vector<uint8_t> payload(1 + len, 0);
  payload[0] = flags;
  if (tx && len > 0) std::memcpy(&payload[1], tx, len);

  uint8_t status = request(REQ_SPI_TRANSFER, payload.data(), payload.size(), 500);
  if (status == ST_BUSY_TIMEOUT && (flags & XFER_WAIT_BUSY)) {
    // Same as the spidev/CH341 paths, which give up waiting and send anyway.
    payload[0] = static_cast<uint8_t>(flags & ~XFER_WAIT_BUSY);
    status = request(REQ_SPI_TRANSFER, payload.data(), payload.size(), 500);
  }
  if (status != ST_OK) throw std::runtime_error(requestError(REQ_SPI_TRANSFER, status));
  if (reply.size() != len) throw std::runtime_error("XIAO bridge: SPI reply has the wrong length");
  if (rx && len > 0) std::memcpy(rx, reply.data(), len);
}

void XiaoBridge::spiCommand(const uint8_t* tx, uint8_t* rx, size_t len) {
  if (len == 0) return;
  if (len <= board.max_transfer) {
    // Chip select, BUSY wait and transfer in one USB round trip.
    xfer(XFER_AUTO_CS | XFER_WAIT_BUSY, tx, rx, len);
    return;
  }
  // Too long for one request: hold NSS low across several transfers.
  const uint8_t wait[2] = {100, 0};
  request(REQ_WAIT_BUSY, wait, sizeof(wait), 500);
  digitalWrite(PIN_NSS, false);
  try {
    for (size_t pos = 0; pos < len;) {
      const size_t n = std::min(board.max_transfer, len - pos);
      xfer(0, tx ? tx + pos : nullptr, rx ? rx + pos : nullptr, n);
      pos += n;
    }
  } catch (...) {
    digitalWrite(PIN_NSS, true);
    throw;
  }
  digitalWrite(PIN_NSS, true);
}
