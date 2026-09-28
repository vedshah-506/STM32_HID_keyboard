# Build and Flashing Guide

## STM32

1. Open the original bare-metal STM32F401 project in STM32CubeIDE or your ARM build environment.
2. Replace its application source with `firmware/stm32/main.c`.
3. Preserve the startup file, linker script, vector table, and build settings that already compile the USB HID project.
4. Verify the board crystal frequency. The source currently assumes `HSE_MHZ = 25`.
5. Verify PA0, PA2, PA3, PA11, and PA12 as documented in `hardware/wiring.md`.
6. Build for Cortex-M4 Thumb mode.
7. Flash using ST-LINK.
8. Connect the STM32 USB port to the target computer.
9. Confirm the USB HID keyboard enumerates.

The source uses direct register access. Do not add `MX_USART2_UART_Init()`, `HAL_UART_Receive()`, or `HAL_UART_Receive_IT()` to this version.

## ESP32

1. Open `firmware/esp32/esp32_web_uart.ino` in Arduino IDE.
2. Replace `CHANGE_THIS_PASSWORD` with a private password.
3. Select the correct ESP32 board and port.
4. Flash the sketch.
5. Open Serial Monitor at 115200 baud.
6. Wire the ESP32 and STM32 with crossed TX/RX lines and a common ground.
7. Connect to the ESP32 access point.
8. Open the printed IP address.
9. Send a short test message.

## First test

Use a focused text editor on the computer connected to the STM32. Send:

```text
abc123
```

The STM32 should type `abc123` and press Enter.

## Troubleshooting

- No USB device: check PA11/PA12, USB clock accuracy, startup/vector files, and the USB cable.
- No UART input: cross TX/RX, connect grounds, verify 115200 8N1, and check PA2/PA3.
- Garbled input: verify the source clock and APB1 frequency are 42 MHz.
- Web page unavailable: confirm the phone/laptop is connected to the ESP32 access point and use the IP printed by the ESP32.
- Button path starts unexpectedly: confirm the button polarity and pull resistor match the board.
- ESP32 input stops after pressing the button: this is expected because `run_demo()` enters an intentional infinite loop; reset the STM32.
