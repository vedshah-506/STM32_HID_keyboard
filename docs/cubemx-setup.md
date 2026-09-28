# CubeMX Setup for the Modified Bare-Metal ESP32-to-STM32 HID Source

## Important distinction

The modified `main.c` is a **standalone register-level bare-metal program**. It does not use the STM32 HAL UART driver and it does not call `MX_USART2_UART_Init()`.

The UART is configured directly in `main.c` by:

```c
usart2_init();
```

and received by polling:

```c
usart2_poll();
```

Therefore, CubeMX is used for pin and clock planning or verification. It must not generate a second HAL-based UART initialization that conflicts with this source.

## 1. Create or open the STM32F401RCT6 project

In STM32CubeMX:

1. Select the exact MCU or board part number: `STM32F401RCT6`.
2. Set the board's actual external crystal frequency. The uploaded source currently assumes:

   ```text
   HSE = 25 MHz
   ```

3. Ensure the USB Full-Speed peripheral is assigned to:

   ```text
   PA11 = USB OTG FS DM
   PA12 = USB OTG FS DP
   ```

The provided bare-metal source configures USB registers directly, so the final build must use this source's USB initialization rather than CubeMX-generated USB middleware.

## 2. Configure the UART pins

Assign:

| STM32 pin | Function | Connection |
|---|---|---|
| PA2 | `USART2_TX` / Alternate Function 7 | ESP32 GPIO16 / RX2 |
| PA3 | `USART2_RX` / Alternate Function 7 | ESP32 GPIO17 / TX2 |
| GND | Ground | ESP32 GND |

The wires are crossed:

```text
ESP32 TX2, GPIO17  →  STM32 PA3, USART2_RX
ESP32 RX2, GPIO16  ←  STM32 PA2, USART2_TX
ESP32 GND          →  STM32 GND
```

In CubeMX pinout view, select:

```text
PA2 → USART2_TX
PA3 → USART2_RX
```

CubeMX should show Alternate Function 7 for these pins.

Do not use PA11 or PA12 for UART because they are USB D− and USB D+ in this design.

## 3. UART peripheral settings

For documentation and pinout verification, configure USART2 as:

| CubeMX setting | Value |
|---|---|
| Mode | Asynchronous |
| Baud rate | 115200 Bits/s |
| Word length | 8 Bits |
| Parity | None |
| Stop bits | 1 |
| Hardware flow control | None |
| Oversampling | 16 |

This is the format:

```text
115200 8N1
```

The edited bare-metal code implements the same configuration directly:

```c
USART2_CR1 = 0;
USART2_CR2 = 0;
USART2_CR3 = 0;
USART2_BRR = (42000000u + 57600u) / 115200u;
USART2_CR1 = USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
```

The APB1 clock is 42 MHz because the source configures an 84 MHz system clock with APB1 divided by two.

## 4. Interrupt settings

Do **not** enable the USART2 global interrupt for this version.

The modified source uses polling:

```c
while (USART2_SR & USART_SR_RXNE) {
    uint8_t c = (uint8_t)USART2_DR;
    // Store the received byte.
}
```

USB still uses its own OTG FS interrupt. Do not disable the USB interrupt.

## 5. Clock configuration

The edited source's clock code expects:

```text
External crystal: 25 MHz
SYSCLK:           84 MHz
APB1:             42 MHz
APB2:             84 MHz
USB clock:        48 MHz
```

Its PLL calculation is:

```text
25 MHz / 25 = 1 MHz
1 MHz × 336 = 336 MHz VCO
336 MHz / 4 = 84 MHz SYSCLK
336 MHz / 7 = 48 MHz USB clock
```

If the board does not have a 25 MHz crystal, change the source's `HSE_MHZ` value to the actual crystal frequency. The PLL code is written to divide the HSE to 1 MHz before multiplying by 336.

If the board uses the internal HSI instead of an external crystal, set:

```c
#define USE_HSE 0
```

However, verify the USB clock carefully. USB Full-Speed requires an accurate 48 MHz clock. The external crystal configuration is preferred.

## 6. Do not mix CubeMX-generated HAL initialization with this file

There are two valid project styles, but they should not be mixed.

### Style A: The uploaded bare-metal source

Use:

- The supplied register definitions.
- `clock_init()`.
- `gpio_init()`.
- `usart2_init()`.
- `usb_init()`.
- The supplied OTG FS interrupt handler.

In this style, CubeMX does not need to generate `usart.c`, `usart.h`, `stm32f4xx_hal_uart.c`, or USB middleware for the final build.

### Style B: A CubeMX HAL project

Use:

- `HAL_Init()`.
- CubeMX-generated `SystemClock_Config()`.
- `MX_GPIO_Init()`.
- `MX_USART2_UART_Init()`.
- `HAL_UART_Receive_IT()` or `HAL_UART_Receive()`.
- CubeMX-generated USB middleware.

In this style, do not use the supplied register-level USB implementation at the same time.

For your uploaded file, use **Style A**. If CubeMX generated a HAL project first, copy only the pin and clock choices into the bare-metal project rather than combining both initialization systems.

## 7. Build and startup requirements

The project must contain:

1. A correct STM32F401 startup file.
2. A linker script for the STM32F401RCT6 memory sizes.
3. A vector-table entry for `OTG_FS_IRQHandler`.
4. The CMSIS device header or startup definitions required by the build.
5. The edited `main.c`.

The edited file uses inline Cortex-M assembly for interrupt masking and direct peripheral addresses. Compile it with an ARM Cortex-M4 target, normally:

```text
-mcpu=cortex-m4 -mthumb
```

Use the same startup and linker files that successfully built the original bare-metal USB HID project.

## 8. ESP32 requirement

The ESP32 must send the message followed by a line-feed byte:

```cpp
STM32Serial.print(message);
STM32Serial.print('\n');
```

Use UART2 on the ESP32:

```cpp
HardwareSerial STM32Serial(2);
STM32Serial.begin(115200, SERIAL_8N1, 16, 17);
```

The ESP32 pins are:

```text
GPIO16 = UART2 RX
GPIO17 = UART2 TX
```

The web-page handler should limit the text to 127 or fewer characters. The STM32 buffer is 128 bytes, but one byte is needed for the null terminator:

```cpp
if (message.length() > 127)
{
    message = message.substring(0, 127);
}
```

The current STM32 code ignores carriage return and uses line feed as the delimiter, so both LF and CRLF-style senders are tolerated.

## 9. Flashing and test sequence

### Test A: Confirm USB HID alone

1. Flash the modified STM32 firmware.
2. Connect the STM32 USB port to the target computer.
3. Confirm that the computer enumerates it as a USB keyboard.
4. Confirm that the old automatic demo no longer types repeatedly.

### Test B: Confirm UART wiring

1. Power the STM32 and ESP32.
2. Connect ESP32 GPIO17 to STM32 PA3.
3. Connect ESP32 GPIO16 to STM32 PA2.
4. Connect both grounds.
5. Do not connect either board's 5 V UART signal to the STM32.

### Test C: Send a fixed ESP32 message

Temporarily place this in the ESP32 `setup()` after `STM32Serial.begin()`:

```cpp
STM32Serial.print("abc123");
STM32Serial.print('\n');
```

After the STM32 has enumerated on the target computer, it should type:

```text
abc123
```

followed by Enter.

### Test D: Test the web page

1. Flash the ESP32 sketch with the HTTP page.
2. Connect the input phone or laptop to the ESP32 access point.
3. Open `http://192.168.4.1` or the IP printed by the ESP32.
4. Submit:

   ```text
   Hello STM32 123
   ```

5. The computer attached to the STM32 should receive the text.

## 10. What this first version does not include

This version intentionally does not include:

- UART acknowledgements.
- CRC or checksums.
- A ring buffer or double buffer.
- WebSocket input.
- Authentication.
- Per-keystroke forwarding.

It uses one simple protocol:

```text
printable text bytes + LF
```

The next message is ignored while the STM32 is typing the previous message. This is acceptable for the first prototype and can be improved later.

## 11. Troubleshooting

### No characters appear

Check that the USB computer has enumerated the STM32 and that `usb_configured` becomes nonzero. The modified code waits for this before typing.

### The web page works but the STM32 does nothing

Check TX/RX crossing:

```text
ESP32 GPIO17 TX → STM32 PA3 RX
```

Check common ground and confirm both sides use 115200 8N1.

### Garbled characters appear

The APB1 clock must be 42 MHz. If the clock configuration was changed, update the BRR calculation. For a 42 MHz APB1 clock and 115200 baud, the BRR value is approximately:

```text
365 decimal = 0x016D
```

### The STM32 types the old message

The old automatic and button-triggered calls were removed from the modified main loop. Confirm that the correct modified file was compiled and flashed.

### The STM32 stops receiving after a long message

The first version has a 127-character effective line limit. The overflowed line is discarded. Send shorter messages until a queue or double buffer is added.

---

The final rule is:

> Configure PA2/PA3 and the 115200 8N1 UART in the design, but let the modified bare-metal `main.c` perform the actual USART2 initialization and polling.

[1]: https://www.st.com/resource/en/datasheet/stm32f401re.pdf "STM32F401xD and STM32F401xE datasheet"
[2]: https://docs.espressif.com/projects/arduino-esp32/en/latest/api/serial.html "Arduino-ESP32 serial communication API"

---

End of guide.
