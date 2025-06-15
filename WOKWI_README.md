# Wokwi Simulation Setup

This project is configured for simulation using Wokwi for VS Code.

## Files Created

- `wokwi.toml` - Configuration file for Wokwi simulation
- `diagram.json` - Circuit diagram with ESP32 and ILI9341 display
- `.vscode/tasks.json` - Build task for PlatformIO

## Hardware Simulation

The simulation includes:
- ESP32 DevKit C v4
- ILI9341 240x320 TFT Display
- Pin connections matching the CYD (Cheap Yellow Display) configuration

### Pin Mapping
- TFT_CS: GPIO 15
- TFT_DC: GPIO 2  
- TFT_SCLK: GPIO 14
- TFT_MOSI: GPIO 13
- TFT_MISO: GPIO 12
- TFT_BL: GPIO 21 (backlight)
- TOUCH_CS: GPIO 33 (touch controller)

## Usage

1. **Build the project**: 
   - Press `Ctrl+Shift+P` and select "Tasks: Run Task"
   - Choose "Build for Wokwi"
   - Or run: `pio run -e cyd2usb`

2. **Start simulation**:
   - Press `F1` and select "Wokwi: Start Simulator"
   - Or press `Ctrl+Shift+P` and search for "Wokwi: Start Simulator"

3. **Serial monitoring**:
   - The simulation includes RFC2217 server on port 4000
   - Use Serial Monitor extension in TCP mode
   - Or connect with: `serial.serial_for_url('rfc2217://localhost:4000', baudrate=115200)`

## Notes

- Make sure to build the project before starting simulation
- The simulation uses the `cyd2usb` environment configuration
- Serial output will be available through the Wokwi interface
- Touch functionality may be limited in simulation (use mouse clicks)

## Troubleshooting

- If simulation fails to start, ensure the firmware files exist in `.pio/build/cyd2usb/`
- Check that the Wokwi extension is installed and up to date
- Verify the paths in `wokwi.toml` match your build output directory
