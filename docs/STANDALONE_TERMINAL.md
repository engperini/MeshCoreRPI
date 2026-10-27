# Standalone MeshCore terminal (roadmap)

Goal: a Raspberry Pi with a LoRa radio (USB MeshStick or HAT) that works **on its own** as a
MeshCore device. Depending on the user's choice it is either:

- a **message terminal**: messages, channels, contacts and history on the Pi's own display
  (touch, kiosk), with the same UI on the LAN as a secondary access path; or
- a **repeater**, unattended.

It must work without a smartphone, Bluetooth companion app, second MCU, external computer,
or Internet (for basic MeshCore messaging).

This document collects the requirements of that plan so nothing is lost, and tracks what is
done in this repository.

## Boot flow (message terminal)

```text
Power on
  -> Raspberry Pi OS 64-bit (with Desktop for the display)
  -> raspberrypimc-companion.service   (MeshStick/HAT radio, TCP :5000)
  -> RemoteTerm container              (web UI, :8080, restart unless-stopped)
  -> Chromium kiosk at desktop login   (optional)
  -> terminal ready on the display, no keyboard/mouse needed
```

The repeater mode is just `raspberrypimc-repeater.service`; no UI is required on the Pi.

## Choosing the mode

| Mode | Install |
|---|---|
| Repeater | `install.sh repeater` |
| Companion for apps on the LAN | `install.sh` |
| Message terminal (web) | `install.sh`, then `RaspberryPiMC/terminal/setup-terminal.sh` |
| Message terminal on the Pi's display | `install.sh`, then `setup-terminal.sh --kiosk` |

One radio runs one role (companion **or** repeater).

## Status

### Done

- [x] MeshCore radio on the Pi without an MCU: native companion and repeater (this repo).
- [x] USB MeshStick (CH341 + SX1262) driver, tested on real RF: RX, TX, channels,
      acknowledged direct messages.
- [x] Region/radio params applied to the chip from the app or the env file.
- [x] systemd services that survive upgrades, one-line installer, prebuilt `.deb` releases.
- [x] Messages/channels/contacts/history UI: RemoteTerm over TCP, set up by `setup-terminal.sh`
      (Docker, restart policy, persistent data in `/opt/meshcorerpi-terminal/data`).
- [x] Kiosk launcher: Chromium `--kiosk`, dedicated profile, no first-run prompts, waits for the
      UI to answer before opening, XDG autostart at desktop login.

### To do

- [ ] **Validate the kiosk on hardware** (Raspberry Pi OS with Desktop + auto-login).
- [ ] **Display profile** for the chosen screen (planned: SPI display on the MeshStick Pi):
      driver/overlay, resolution, rotation, touch calibration, on-screen keyboard,
      control size, brightness, screen blanking, power draw.
- [ ] Every essential flow usable by touch only (send, reply, channels, contacts, adverts).
- [ ] Kiosk recovery: auto-reload if the browser or the UI dies; hide the cursor on touch-only
      screens; an admin way out of the kiosk; reboot/shutdown from the UI or a side panel.
- [ ] Reboot test: power-cycle goes straight to the terminal with no keyboard/mouse.
- [ ] Bots (RemoteTerm Python bots: echo, status, keyword, scheduled, diagnostic; MQTT/HTTP/
      Home Assistant/GPIO/sensors). Kept **disabled** until the UI has authentication/HTTPS,
      because bots run arbitrary Python. A failing bot must never take the radio down.
- [ ] Hardening/appliance: watchdog, health monitoring, log rotation, backup/restore of
      identity + contacts + RemoteTerm database, update/rollback, clean shutdown, recovery after
      power loss, storage/RAM/temperature diagnostics.

## Persistence

| Data | Location |
|---|---|
| Node identity, keys, contacts, radio prefs | `/var/lib/raspberrypimc/userdata` |
| Companion/repeater config | `/etc/raspberrypimc/*.env` |
| RemoteTerm history, channels, settings | `/opt/meshcorerpi-terminal/data/meshcore.db` |

Back up the first and the last to keep a node's identity and message history across reinstalls.

## Security

- Never expose the companion port (5000) or RemoteTerm (8080) to the Internet: neither has
  authentication. Keep them on the LAN.
- RemoteTerm bots stay disabled by default (`MESHCORE_DISABLE_BOTS=true`).
- No secrets in Git; avoid privileged containers (the radio runs on the host, not in Docker).

## Design notes

- Reuse, don't rewrite: RemoteTerm is the UI; this repo provides the radio and the glue.
- The radio runs natively on the host (systemd), not in a container: direct USB/SPI/GPIO access
  and fewer moving parts. The UI runs in Docker.
- No simulator or TCP router is needed any more: RemoteTerm talks straight to the companion.
