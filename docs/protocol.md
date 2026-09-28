# Communication Protocol

## Overview

```text
Browser → HTTP POST → ESP32 WebServer → UART2 → STM32 USART2 → USB HID → Computer
```

## UART settings

- Baud rate: `115200`
- Data bits: `8`
- Parity: none
- Stop bits: `1`
- Flow control: none
- Electrical level: 3.3 V

## Frame format

The protocol is a simple line-based ASCII protocol:

```text
[payload bytes][LF]
```

`LF` is the line-feed byte `0x0A`, sent by the ESP32 with:

```cpp
STM32Serial.print(message);
STM32Serial.print('\n');
```

The STM32 ignores carriage return (`0x0D`) and uses LF as the end-of-message delimiter.

## Example

```text
Hello STM32\n
```

The STM32 receives `Hello STM32`, converts the supported ASCII characters to HID key usages, types the message, and presses Enter.

## Current limitations

The effective STM32 line buffer is 127 characters including the null-termination constraint. The basic version has no acknowledgement, checksum, retransmission, queue, or authentication.
