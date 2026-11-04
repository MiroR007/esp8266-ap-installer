# ESP8266 Soft-AP Station Control

## One-click web installer

The repository's GitHub Actions workflow builds the firmware with Espressif's official ESP8266 RTOS SDK v3.4 and publishes an ESP Web Tools installer on GitHub Pages. The installer target is a NodeMCU ESP8266 with 4MB flash.

Firmware for an ESP8266/NodeMCU that creates its own test Wi-Fi access point and serves a mobile UI at `http://192.168.4.1`.

It can:

- list stations associated with the ESP8266's own Soft-AP (MAC and DHCP IP),
- disconnect one selected station once,
- locally block one selected MAC from reconnecting until **Stop / Restore** is pressed.

It deliberately does **not** scan, target, or inject frames into external networks. It only uses the Soft-AP management APIs provided by Espressif.

## Requirements

- ESP8266 NodeMCU development board
- Espressif **ESP8266 RTOS SDK v3.4** and its matching GCC 8.4 toolchain
- USB data cable and the board's serial driver
- Linux, macOS, or a Linux environment on Windows (WSL is usually the easiest route for this legacy SDK)

Official SDK setup instructions: <https://github.com/espressif/ESP8266_RTOS_SDK>

## Configure, build, and flash

1. Install the ESP8266 RTOS SDK v3.4 toolchain and export `IDF_PATH` as described by Espressif.
2. Open a shell in this project directory.
3. Configure the project:

   ```sh
   idf.py menuconfig
   ```

   Under **AP station control**, change the SSID and password. Use a unique password of 8–63 characters. Under **Serial flasher config**, choose the correct serial port and flash size for your NodeMCU board.

4. Build and flash:

   ```sh
   idf.py build
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

   In WSL, the serial device may differ. On native Windows it is typically a `COM` port. If the SDK checkout uses the older Make workflow, the equivalent commands are `make menuconfig` and `make flash monitor`.

5. Connect a phone to the configured test SSID and open `http://192.168.4.1` in a browser.

## Operation

- Select a connected station.
- **Disconnect once** calls `esp_wifi_deauth_sta(aid)` once. Most phones reconnect automatically.
- **Block locally and disconnect** remembers that MAC in RAM. When that MAC associates again, the firmware immediately calls the same supported Soft-AP API for its new AID.
- **Stop / Restore** clears the local block. The client must reconnect itself; the ESP8266 cannot force a phone to join.

If you select the phone currently displaying the UI, that phone loses the page as soon as it is disconnected. Use a second connected device to manage it, or reconnect and press Restore after reboot (the block is RAM-only and a reboot also clears it).

## SDK limitations

- `esp_wifi_deauth_sta()` selects a station by its transient association ID (AID), not by MAC. The firmware records the MAC↔AID mapping from Soft-AP connect events.
- The station-list API returns MAC/PHY data but not AID, so a client already associated before the application event handler started cannot be selectively disconnected. In this firmware the handler is installed before Wi-Fi starts, preventing that normal startup race.
- The local block list contains one MAC and lives only in RAM. It is intentionally cleared on reset and by Restore.
- MAC addresses can be randomized or spoofed, so this is access-control convenience, not strong identity or security.
- The ESP8266 is 2.4 GHz only. The Soft-AP uses one channel and does not affect other networks.
- The legacy ESP8266 RTOS SDK is maintenance-oriented and much older than current ESP-IDF. For new hardware or longer-lived projects, an ESP32 with current ESP-IDF is the better-supported choice.
- The HTTP UI is unencrypted on the isolated local AP. The WPA2 AP password is the access boundary; change the default before use.

## Safety boundary

This project contains no promiscuous-mode logic, raw 802.11 transmission, packet injection, external AP scanning, or third-party-network deauthentication code.

