# ESP32 Flight Tracker ✈️

A real-time, live Flight Tracker built for the ESP32-S3. This project utilizes the OpenSky Network API to fetch live aircraft data and renders them on a beautiful dual-screen setup (TFT + OLED) with a dual-core architecture. 

It was originally built as a module for a custom handheld game console, but has been extracted here as a standalone project for anyone to build and enjoy!

## Features 🌟

- **Dual-Core Architecture (FreeRTOS):**
  - **Core 0:** Dedicated to networking. It handles downloading and parsing huge chunked JSON files from the OpenSky API without blocking the UI.
  - **Core 1:** Dedicated to rendering the map, airplanes, and UI at 15+ FPS for a buttery smooth panning experience.
- **Offline PSRAM Maps & Hardware Limits:**
  - Due to the ESP32-S3's hardware limits (8MB PSRAM), it is impossible to load a high-resolution offline map of the entire world into memory at once. 
  - To push the hardware to its absolute maximum limit, I engineered a **Hybrid Map System**: A high-resolution regional map (2048x1024) specifically for Turkey and its surroundings (which consumes ~6.5MB of PSRAM), and a low-resolution global fallback map (512x256) for the rest of the world.
  - Both maps are stored on an SD Card and loaded directly into the ESP32's PSRAM on boot, ensuring zero latency when moving the map via joystick!
- **Altitude Heatmap & Anti-Aliasing:**
  - Planes are drawn using 72 pre-rendered frames (with anti-aliasing) per color.
  - Aircraft colors dynamically change based on altitude:
    - **Yellow:** Low altitude (< 15,000 ft)
    - **Green:** Medium altitude (15,000 - 25,000 ft)
    - **Blue:** Cruising altitude (25,000 - 35,000 ft)
    - **Purple:** High cruising altitude (> 35,000 ft)
- **Z-Index Rendering:**
  - The currently selected aircraft is always drawn on top, ensuring you never lose your tracked flight in crowded airspaces.
- **Persistent Tracking:**
  - When the OpenSky API refetches data (every 120s), the tracker remembers your selected aircraft by its unique ICAO24 transponder code, re-selecting it seamlessly.

## Project Structure 📂

- `ESP32-Flight-Tracker.ino` - The main application entry point and FreeRTOS task manager.
- `flight_config.h` - Contains your WiFi credentials, API secrets, and polling intervals.
- `flight_control.cpp` - Handles joystick panning, button inputs, and zooming logic.
- `flight_internal.h` - Global shared state and data structures (e.g., `Aircraft` struct).
- `flight_map.cpp` - Map rendering, geographical coordinate projections, and country borders.
- `flight_net.cpp` - WiFi connection, chunked JSON downloading, and manual parsing of OpenSky data.
- `flight_plane_frames.h` - The 128KB pre-rendered, 4-color aircraft sprites for the altitude heatmap.
- `flight_render.cpp` - The heavy-lifting UI rendering engine (Double buffering, TFT, and OLED drawing).
- `flight_borders.h` - High-resolution GeoJSON vector borders for seamless zooming without pixelation.
- `hardware_config.h` - Defines all SPI/I2C pins, buttons, and joystick hardware connections.
- `partitions.csv` - The custom 16MB partition scheme tailored to fit the huge app firmware.
- `generate_airplane.py` - A python utility script (optional) to regenerate or modify aircraft sprites.
- `SD_Card_Files/` - Contains `regional.bin` (6.5MB) and `world.bin` (256KB) that MUST be copied to your MicroSD Card.

## Hardware Requirements 🛠️

- **Microcontroller:** ESP32-S3 (Requires minimum 8MB PSRAM)
- **Displays:**
  - 1.8" TFT Display (160x128) - Main Radar Screen
  - 0.96" OLED Display (128x64) I2C - Stats Screen
- **Input:** Analog Joystick (X/Y + Button) & 3 Push Buttons
- **Storage:** MicroSD Card Module (For loading map `.bin` files)
- **Audio (Optional):** Passive Buzzer for UI feedback

*Note: You can easily change the pin definitions in `hardware_config.h` to match your own wiring!*

## Arduino IDE Settings ⚙️

To successfully compile this project for the ESP32-S3, make sure your settings in **Tools** match the following (adjust PSRAM/Flash type based on your specific ESP32-S3 board):

- **Board:** ESP32S3 Dev Module
- **Flash Size:** **16MB (128Mb)** *(Required to fit the included custom partition table)*
- **Partition Scheme:** **Custom** *(CRITICAL: The project includes a custom `partitions.csv` which gives enough space for the app and OTA)*
- **PSRAM:** **OPI PSRAM** or **QSPI PSRAM** *(CRITICAL: Must be Enabled! Maps are loaded into PSRAM)*
- **USB CDC On Boot:** Enabled (Recommended for seeing Serial Monitor logs)

## Setup & Installation 🚀

1. Clone or download this repository.
2. Open `ESP32-Flight-Tracker.ino` in the Arduino IDE.
3. Install the required libraries via the Arduino Library Manager:
   - `TFT_eSPI` (Configure your User_Setup.h for your specific TFT screen)
   - `U8g2`
   - `ArduinoJson` (v7.x or later)
4. Update `flight_config.h` with your **WiFi SSID/Password** and your **OpenSky Network Credentials**.
5. Copy the map binary files (`regional.bin` and `world.bin`) from the `SD_Card_Files` folder into the root directory of your MicroSD card.
6. Compile and upload to your ESP32-S3!

## Controls 🎮

- **Joystick X/Y:** Pan the map (East/West, North/South)
- **Joystick Click:** Reset Viewport to default center
- **BTN A:** Zoom In
- **BTN B:** Zoom Out
- **BTN C:** Select nearest aircraft to center / Deselect
