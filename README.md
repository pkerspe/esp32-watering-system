<p align="center">
<img src="https://raw.githubusercontent.com/pkerspe/esp32-watering-system/main/data/esp_watersystem_logo.svg" alt="ESP32-Watering-System" height="150">
</p>

# esp32-watering-system

A watering system controller based on an ESP32 MCU to schedule triggering of low voltage Pumps and Valves (current layout is focussing on 12V Systems) and control via a webinterface.

This project is currently in an early stage, the final list of features is as follows:
- ESP 32 based controller for scheduled and intelligent watering of plants
- Webserver with User Interface for Status and configuration (1)
- Capacitive, Analog Soil Moisture Sensor support (2)
- 5 Channels for controlling a Water Pump (low voltage e.g. 12V) and 4 (or 6 if wired/connected in a matrix style setup) Solenoid valves for controlling water flow (1)
- Flow Meter support to measure the amount of water dispensed (2)
- Support inclusion of weather forecast data from openWeatherMap API to postpone watering runs (e.g. when rain is predicted)
- Simple, switch based water Level sensor for the water reservoir implemented as a resistor ladder to support multiple switches (2)
- support for SPI TFT Touch Screen display for status display and configuration (3)

(1) = first iteration, currently in progress
(2) = second iteration (not yet started)
(3) = third iteration (not yet started)

A matching PCB Layout (Work in progress, use at own risk) for this code can be found here: https://oshwlab.com/paulk/display-mosfet-control-board_copy
The PCB is designed with 
- footprint for a ESP32 Dev Kit with 30 Pins
- 5 MOSFET controlled Low Side switching channels
- room for a I2C connected ADC IC with 8 channels (for the moisture sensors, water flow sensor and water level sensing)
- foot print for a SPI connected 2.8 inch Touch Screen module with SD Card reader (should also work with certain 2.4 and 3.2 inch SPI TFT Touch Screen types if you do not need SDCARD support)
- three I2C pin headers for other modules (I2C Pull Up resistors on board)
- UART pin header

<img src="https://github.com/pkerspe/esp32-watering-system/raw/main/doc/watering_system_pcb_v2.JPG" height="400">
