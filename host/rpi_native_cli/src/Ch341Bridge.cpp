#include "Ch341Bridge.h"

#ifdef __linux__
#include <dlfcn.h>
#endif

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
constexpr uint8_t EP_OUT = 0x02;
constexpr uint8_t EP_IN = 0x82;
constexpr unsigned int USB_TIMEOUT_MS = 1000;
constexpr size_t PACKET_LEN = 32;

constexpr uint8_t CMD_GET_INPUT = 0xA0;
constexpr uint8_t CMD_SPI_STREAM = 0xA8;
constexpr uint8_t CMD_UIO_STREAM = 0xAB;
constexpr uint8_t UIO_STM_OUT = 0x80;
constexpr uint8_t UIO_STM_DIR = 0x40;
constexpr uint8_t UIO_STM_END = 0x20;

constexpr int SCK_PIN = 3;
constexpr int MOSI_PIN = 5;

// Minimal libusb-1.0 ABI, resolved with dlsym().
struct LibUsb {
  int (*init)(void** ctx) = nullptr;
  void (*exit)(void* ctx) = nullptr;
  void* (*open_device_with_vid_pid)(void* ctx, uint16_t vid, uint16_t pid) = nullptr;
  void (*close)(void* handle) = nullptr;
  int (*set_auto_detach_kernel_driver)(void* handle, int enable) = nullptr;
  int (*claim_interface)(void* handle, int iface) = nullptr;
  int (*release_interface)(void* handle, int iface) = nullptr;
  int (*bulk_transfer)(void* handle, unsigned char ep, unsigned char* data, int len, int* transferred,
                       unsigned int timeout) = nullptr;
  const char* (*error_name)(int code) = nullptr;
};

LibUsb usb;

// The CH341 shifts SPI data LSB first; SX126x expects MSB first.
uint8_t reverseBits(uint8_t x) {
  x = static_cast<uint8_t>(((x >> 1) & 0x55) | ((x << 1) & 0xAA));
  x = static_cast<uint8_t>(((x >> 2) & 0x33) | ((x << 2) & 0xCC));
  return static_cast<uint8_t>((x >> 4) | (x << 4));
}

std::string usbError(const char* what, int rc) {
  std::string msg = std::string("CH341 ") + what + " failed";
  if (usb.error_name) msg += std::string(": ") + usb.error_name(rc);
  return msg;
}

#ifdef __linux__
template <typename T>
void resolve(void* lib, T& fn, const char* name) {
  fn = reinterpret_cast<T>(dlsym(lib, name));
  if (!fn) throw std::runtime_error(std::string("libusb symbol missing: ") + name);
}
#endif
}

Ch341Bridge::~Ch341Bridge() {
  close();
}

void Ch341Bridge::open(uint16_t vid, uint16_t pid) {
  if (handle) return;
#ifdef __linux__
  if (!lib) {
    lib = dlopen("libusb-1.0.so.0", RTLD_NOW);
    if (!lib) lib = dlopen("libusb-1.0.so", RTLD_NOW);
    if (!lib) {
      throw std::runtime_error("CH341: libusb-1.0 not found (install it with: sudo apt install libusb-1.0-0)");
    }
    resolve(lib, usb.init, "libusb_init");
    resolve(lib, usb.exit, "libusb_exit");
    resolve(lib, usb.open_device_with_vid_pid, "libusb_open_device_with_vid_pid");
    resolve(lib, usb.close, "libusb_close");
    resolve(lib, usb.set_auto_detach_kernel_driver, "libusb_set_auto_detach_kernel_driver");
    resolve(lib, usb.claim_interface, "libusb_claim_interface");
    resolve(lib, usb.release_interface, "libusb_release_interface");
    resolve(lib, usb.bulk_transfer, "libusb_bulk_transfer");
    resolve(lib, usb.error_name, "libusb_error_name");
  }

  int rc = usb.init(&ctx);
  if (rc < 0) throw std::runtime_error(usbError("libusb_init", rc));

  handle = usb.open_device_with_vid_pid(ctx, vid, pid);
  if (!handle) {
    close();
    throw std::runtime_error("CH341: USB device 1a86:5512 not found or not accessible "
                             "(check lsusb, and run as root or add a udev rule)");
  }
  usb.set_auto_detach_kernel_driver(handle, 1);
  rc = usb.claim_interface(handle, 0);
  if (rc < 0) {
    const std::string msg = usbError("claim interface 0", rc);
    close();
    throw std::runtime_error(msg + " (is another process, e.g. meshtasticd, using the dongle?)");
  }

  // SCK and MOSI must be driven as outputs for the SPI stream to reach the radio.
  pin_dir = static_cast<uint8_t>((1u << SCK_PIN) | (1u << MOSI_PIN));
  pin_state = 0;
  flushPins();
#else
  (void)vid; (void)pid;
  throw std::runtime_error("CH341 bridge requires Linux");
#endif
}

void Ch341Bridge::close() {
#ifdef __linux__
  if (handle) {
    usb.release_interface(handle, 0);
    usb.close(handle);
    handle = nullptr;
  }
  if (ctx) {
    usb.exit(ctx);
    ctx = nullptr;
  }
#endif
}

void Ch341Bridge::bulkOut(const uint8_t* buf, int len) {
  if (!handle) throw std::runtime_error("CH341 not open");
  int done = 0;
  const int rc = usb.bulk_transfer(handle, EP_OUT, const_cast<unsigned char*>(buf), len, &done, USB_TIMEOUT_MS);
  if (rc < 0) throw std::runtime_error(usbError("bulk OUT", rc));
  if (done != len) throw std::runtime_error("CH341 short bulk OUT");
}

void Ch341Bridge::bulkIn(uint8_t* buf, int len) {
  if (!handle) throw std::runtime_error("CH341 not open");
  int done = 0;
  const int rc = usb.bulk_transfer(handle, EP_IN, buf, len, &done, USB_TIMEOUT_MS);
  if (rc < 0) throw std::runtime_error(usbError("bulk IN", rc));
  if (done != len) throw std::runtime_error("CH341 short bulk IN");
}

void Ch341Bridge::flushPins() {
  // Only D0..D5 are writable through the UIO stream.
  const uint8_t cmd[4] = {
    CMD_UIO_STREAM,
    static_cast<uint8_t>(UIO_STM_OUT | (pin_state & 0x3F)),
    static_cast<uint8_t>(UIO_STM_DIR | (pin_dir & 0x3F)),
    UIO_STM_END,
  };
  bulkOut(cmd, sizeof(cmd));
}

void Ch341Bridge::pinMode(int pin, bool output) {
  if (pin < 0 || pin > 5) return;
  if (output) pin_dir = static_cast<uint8_t>(pin_dir | (1u << pin));
  else pin_dir = static_cast<uint8_t>(pin_dir & ~(1u << pin));
  flushPins();
}

void Ch341Bridge::digitalWrite(int pin, bool high) {
  if (pin < 0 || pin > 5) return;
  if (high) pin_state = static_cast<uint8_t>(pin_state | (1u << pin));
  else pin_state = static_cast<uint8_t>(pin_state & ~(1u << pin));
  flushPins();
}

bool Ch341Bridge::digitalRead(int pin) {
  if (pin < 0 || pin > 7) return false;
  const uint8_t cmd = CMD_GET_INPUT;
  bulkOut(&cmd, 1);
  uint8_t reply[6] = {0};
  bulkIn(reply, sizeof(reply));
  return (reply[0] & (1u << pin)) != 0;
}

void Ch341Bridge::spiTransfer(const uint8_t* tx, uint8_t* rx, size_t len) {
  uint8_t out[PACKET_LEN];
  uint8_t in[PACKET_LEN];
  for (size_t pos = 0; pos < len;) {
    const size_t n = std::min(PACKET_LEN - 1, len - pos);
    out[0] = CMD_SPI_STREAM;
    for (size_t i = 0; i < n; ++i) out[i + 1] = reverseBits(tx ? tx[pos + i] : 0x00);
    bulkOut(out, static_cast<int>(n + 1));
    bulkIn(in, static_cast<int>(n));
    if (rx) {
      for (size_t i = 0; i < n; ++i) rx[pos + i] = reverseBits(in[i]);
    }
    pos += n;
  }
}
