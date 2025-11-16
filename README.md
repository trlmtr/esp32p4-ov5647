# ESP32-P4 OV5647 Camera Application

This project captures images from an OV5647 CSI camera connected to an ESP32-P4 and streams them via a web server. The board uses an ESP32-C6 co-processor with esp-hosted for WiFi connectivity.

## Features

- OV5647 CSI camera support via MIPI interface
- WiFi connectivity through ESP32-C6 co-processor using esp-hosted
- HTTP web server with:
  - Live MJPEG video streaming at `/stream`
  - Single image capture at `/capture`
  - Simple web interface at `/`
- mDNS support for easy access (http://esp-camera.local)

## Hardware Requirements

- Waveshare ESP32-P4-Module-DEV-KIT
- OV5647 camera module (connected via CSI/MIPI interface)
- ESP32-C6 co-processor (on-board for WiFi/BT)

## Pin Configuration

The default pin configuration for the camera I2C interface:
- I2C Port: 0
- SCL Pin: GPIO 8
- SDA Pin: GPIO 7
- XCLK Frequency: 24 MHz

## Software Requirements

- ESP-IDF v5.3 or later
- Components (included in support_folder):
  - esp-video-components
  - esp-hosted
  - esp-bsp

## Building and Flashing

1. **Set the target:**
   ```bash
   idf.py set-target esp32p4
   ```

2. **Configure the project:**
   ```bash
   idf.py menuconfig
   ```
   
   Navigate to `Example Connection Configuration` and set:
   - WiFi SSID
   - WiFi Password

3. **Build the project:**
   ```bash
   idf.py build
   ```

4. **Flash to the device:**
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

## Usage

1. After flashing, the device will connect to your WiFi network
2. The serial monitor will show the IP address
3. Open a web browser and navigate to:
   - `http://esp-camera.local` (using mDNS)
   - Or use the IP address shown in the serial monitor

### Web Interface

- **Main page (/)**: Simple HTML interface with embedded video stream
- **Stream endpoint (/stream)**: MJPEG stream for viewing live video
- **Capture endpoint (/capture)**: Captures and downloads a single JPEG image

## Project Structure

```
ov5647/
├── CMakeLists.txt              # Root CMake configuration
├── sdkconfig.defaults          # Default configuration
├── main/
│   ├── CMakeLists.txt          # Main component configuration
│   ├── idf_component.yml       # Component dependencies
│   ├── Kconfig.projbuild       # Configuration options
│   ├── app_main.c              # Main application entry point
│   ├── camera_init.c/h         # Camera initialization module
│   └── camera_server.c/h       # Web server implementation
└── support_folder/             # External components
    ├── esp-video-components/   # Camera drivers
    ├── esp-hosted/             # WiFi co-processor support
    └── esp-bsp/                # Board support packages
```

## Configuration Options

You can customize the application through `idf.py menuconfig`:

### Camera Application Configuration
- `MDNS_HOSTNAME`: mDNS hostname (default: esp-camera)
- `MDNS_INSTANCE`: mDNS instance name (default: ESP32-P4 Camera)

### Example Connection Configuration
- WiFi SSID and Password
- Connection retry settings

## Troubleshooting

### Camera Not Detected
- Check camera connections (especially I2C pins)
- Verify camera is properly powered
- Check I2C bus configuration in menuconfig

### WiFi Not Connecting
- Verify WiFi credentials in menuconfig
- Ensure ESP32-C6 co-processor is properly configured
- Check esp-hosted configuration

### Web Server Issues
- Verify IP address is correct
- Try using IP address instead of mDNS hostname
- Check firewall settings on your computer

### Build Errors
- Ensure ESP-IDF version is 5.3 or later
- Verify all components in support_folder are present
- Clean build: `idf.py fullclean`

## Next Steps

This is the first step in your project. Future enhancements can include:
1. Face detection using ESP-DL
2. MQTT messaging when faces are detected
3. Image processing and analytics
4. Multi-camera support

## License

This project is provided as-is for educational and development purposes.
