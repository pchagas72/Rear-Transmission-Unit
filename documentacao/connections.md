# Rear Transmission Unit Connections

Main telemetry and datalogging unit.

> **Firmware ativo:** `board_pins.h` segue este documento.  
> Lacunas ESP32-WROOM-32 (validação bancada): A7670 `RXD`→GPIO35 é *input-only*
> (ESP não TX). SD CS = **GPIO18** (bancada). Detalhes: `relatorio_hw_connections.md`.

## Components and connections
LoRa ebyte E32 module for radio telemetry.
GPRS A7670SA module for celular telemetry through MQTT.
SD module for datalogging.
- LoRa (Ebyte E32)
    - M0  => ESP32 (GPIO_15) 
    - M1  => ESP32 (GPIO_17) 
    - AUX => ESP32 (GPIO_05)
    - RX  => ESP32 (GPIO_16)
    - TX  => ESP32 (GPIO_04)
    - VCC => ESP32 (ESP_VIN)
    - GND => ESP32 (ESP_GND)
- GPRS (A7670SA)
    - 1 (VCC)   => ?
    - 2 (GND)   => ESP32 (ESP_GND)
    - 3 (TXD)   => LLC (TX_A1 -> TX_B1) => ESP32 (GPIO_34)
    - 4 (RXD)   => LLC (RX_A2 -> RX_B2) => ESP32 (GPIO_35)
    - 5 (3.8EN) => ESP32 (None)
    - 6 (NET)   => ESP32 (None)
    - 7 (DTR)   => ESP32 (None)
- SD (Generic 3.3v SPI-SD module)
    - MISO => ESP32 (GPIO_19)
    - MOSI => ESP32 (GPIO_22)
    - CLK  => ESP32 (GPIO_21)
    - CS   => ESP32 (GPIO_18)
    - VCC  => ESP32 (ESP_3v3)
    - GND  => ESP32 (ESP_GND)
- CAN transceptor (SN65HVD230)
    - RX  => ESP32 (GPIO_14)
    - TX  => ESP32 (GPIO_27)
    - GND => ESP32 (ESP_GND)
    - VCC => ESP32 (ESP_3v3)

## Main components and bridges
This is where the connections go to.
- Logic Level Converter (WCMCU-0108)
    - VA => 1.8v font
    - VB => 3.3 font
- ESP32 (Wroom-32)
