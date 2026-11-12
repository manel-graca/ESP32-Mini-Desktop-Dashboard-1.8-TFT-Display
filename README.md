# ESP32 Mini Desktop Dashboard

A small desktop dashboard for an ESP32 with a 1.8" ST7735 TFT (128x160), two buttons and an RGB LED.

## Pages

- **Dashboard** – clock, date, current weather and 3-day forecast
- **Sun** – sunrise / sunset arc and UV index
- **Calendar** – month view with Portuguese public holidays
- **Crypto** – BTC, ETH and SOL prices in EUR
- **PC** – CPU, GPU and RAM usage with a load graph
- **AI Usage** – Claude and Codex usage limits
- **System** – Wi-Fi, IP, uptime and memory

Hold a button to change page, click to interact with the current page.

## Firmware

1. Open `sketch_nov9a/sketch_nov9a.ino` in the Arduino IDE (ESP32 board package).
2. Install the libraries: `Adafruit GFX`, `Adafruit ST7735`, `ArduinoJson`.
3. Set `WIFI_SSID`, `WIFI_PASS` and your latitude / longitude at the top of the sketch.
4. Upload.

### Wiring

| Part         | GPIO |
|--------------|------|
| TFT CS       | 5    |
| TFT DC       | 16   |
| TFT RST      | 17   |
| TFT SCK      | 18   |
| TFT MOSI     | 23   |
| Left button  | 4    |
| Right button | 0    |
| LED red      | 13   |
| LED green    | 15   |
| LED blue     | 2    |

## PC bridge (optional)

The PC and AI usage pages need `pc/painel_bridge.py` running on your computer. It sends data over USB serial and Wi-Fi (UDP).

```
pip install requests pyserial
```

Then run `pc/run_painel.bat`.

PC stats come from [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) with its web server enabled on port 8085.

## Data sources

- Weather: [Open-Meteo](https://open-meteo.com/)
- Crypto: [CoinGecko](https://www.coingecko.com/)
