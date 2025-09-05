# D1 Mini - MAX7219 Dot Matrix Module Display <a href='https://ko-fi.com/christianjameswatkins' target='_blank'><img height='35' align='right' style='border:0px;height:46px;' src='https://storage.ko-fi.com/cdn/kofi1.png?v1' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>

This project creates a versatile WiFi-enabled clock and message display using a WEMOS D1 Mini and two 32x8 (cascaded 4x 8x8) MAX7219 dot matrix modules.

![GPIO Pin Out](D1%20Mini%20-%20MAX7219%20Dot%20Matrix%20Module%20Display.png)

## Features

*   Displays Time (12/24h format, with optional seconds), Day of the week, and Date.
*   Shows a custom user-defined message.
*   Features a countdown timer to a specific date and time.
*   Built-in Web Interface for configuration:
    *   Initial setup via WiFi Access Point mode.
    *   Connects to your local WiFi network.
    *   All display settings can be configured on-the-fly through the web UI.
*   Settings are saved to EEPROM, so they persist after power loss.
*   Factory reset option to clear all settings.

## Hardware Required

You will need the following components:

*   **1 x WEMOS D1 Mini (or compatible ESP8266 board):**
    *   Can be purchased here: [D1 Mini (USB-C/Micro)](https://www.aliexpress.com/item/1005007364969950.html)
*   **2 x MAX7219 Dot Matrix Module 32x8 (4-in-1):**
    *   Can be purchased here: [MAX7219 Dot Matrix Module](https://www.aliexpress.com/item/1005008005112441.html)
*   Connecting wires.
*   A 5V power supply (if you use the same board linked above a standard USB port should work).

## Wiring Instructions

The MAX7219 modules need to be connected to the D1 Mini. The modules are daisy-chained, meaning the output of the first module connects to the input of the second.

**1. Connect the D1 Mini to the first 32x8 module:**

| D1 Mini Pin | MAX7219 Pin |
| :---------- | :---------- |
| `D7` (MOSI) | `DIN`       |
| `D6` (CS)   | `CS`        |
| `D5` (SCK)  | `CLK`       |
| `5V`        | `VCC`       |
| `GND`       | `GND`       |

**2. Connect the first module to the second module:**

Connect the `DOUT`, `CS`, `CLK`, `VCC`, and `GND` pins on the output side of the first module to the corresponding input pins (`DIN`, `CS`, `CLK`, `VCC`, `GND`) of the second module.


## Display Hardware Configuration

The MAX7219 Dot Matrix modules come from various manufacturers and may have different internal wiring. To support this, the `MD_MAX72xx` library provides a `HARDWARE_TYPE` setting in the `RTC.ino` file.

### Choosing a `HARDWARE_TYPE`

In `RTC.ino`, you will find this line:
```c++
#define HARDWARE_TYPE MD_MAX72XX::DR1CR0RR1_HW
```

If your display appears scrambled or does not work correctly, you may need to change this value. The most common types for modules found on Amazon, AliExpress, etc., are:

*   `FC16_HW`: For modules with "FC-16" printed on the circuit board.
*   `GENERIC_HW`: For many other common modules.

**It is recommended to try `FC16_HW` first.** If that does not work, try `GENERIC_HW`.

Additional configurations can be found in the [library documentation](https://majicdesigns.github.io/MD_MAX72XX/page_hardware.html).

### Correcting Mirrored or Flipped Displays

After selecting the correct `HARDWARE_TYPE`, your display may still appear mirrored (backwards). This can be corrected in software.

In the `setup()` function of `RTC.ino`, you will find this line:
```c++
myDisplay.setZoneEffect(0, true, PA_FLIP_LR);
```
This line flips the display horizontally (`LR` = Left-Right).

*   If your text is mirrored, **keep this line**.
*   If your text is not mirrored, you can **remove or comment out this line** to prevent it from being flipped.

## Software & Programming

You will use the Arduino IDE to program the D1 Mini.

**1. Install Arduino IDE:**
Download and install the latest version from the [Arduino website](https://www.arduino.cc/en/software).

**2. Add ESP8266 Board Manager:**
*   In the Arduino IDE, go to `File` > `Preferences`.
*   In the "Additional Boards Manager URLs" field, add the following URL:
    ```
    http://arduino.esp8266.com/stable/package_esp8266com_index.json
    ```
*   Go to `Tools` > `Board` > `Boards Manager...`.
*   Search for `esp8266` and install the package.

**3. Install Required Libraries:**
Go to `Tools` > `Manage Libraries...` (or `Sketch` > `Include Library` > `Manage Libraries...`) and install the following libraries:
*   `ArduinoJson` by Benoit Blanchon
*   `MD_Parola` by MajicDesigns
*   `MD_MAX72xx` by MajicDesigns (this is a dependency for MD_Parola and should be installed with it)
*   `NTPClient` by Fabrice Weinberg

**4. Program the D1 Mini:**
*   Open the `RTC.ino` sketch file in the Arduino IDE.
*   Connect the D1 Mini to your computer via USB.
*   Go to `Tools` > `Board` and select "LOLIN(WEMOS) D1 R2 & mini".
*   Select the correct COM port under `Tools` > `Port`.
*   Click the "Upload" button (the arrow icon).

## Initial Setup & Usage

On its first boot (or after a factory reset), the display will start in Access Point (AP) mode.

1.  On your phone or computer, connect to the WiFi network named **"MatrixDisplay"**.
2.  Once connected, open a web browser and navigate to `http://192.168.4.1`.
3.  Click on "Setup WiFi", select your home WiFi network (SSID), and enter its password.
4.  The display will reboot and connect to your network. If the display is configured to show it, it will scroll its new IP address.
5.  You can now access the settings page by navigating to that IP address in your browser from any device on the same network.

## Customization

### Changing Default Behavior (Firmware)

If you perform a factory reset, the display reverts to its hardcoded default settings. You can change these defaults by editing the `RTC.ino` file before uploading. The `initializeDefaultDisplaySettings` function sets the default `flags`. The `flags` variable uses one bit for each display item. For example, to enable Time (bit 0) and Date (bit 2) by default, you would set:
```c++
// In initializeDefaultDisplaySettings():
g_displaySettings.flags = (1 << 0) | (1 << 2);
```

### Runtime Configuration (Web UI)

For most changes, you do not need to reprogram the device. Simply navigate to the device's IP address in a browser to access the settings page. Here you can:
*   Enable or disable display items (Time, Date, etc.).
*   Change display durations and animation speeds.
*   Set a custom message.
*   Configure the countdown timer.
*   Adjust the display brightness.
