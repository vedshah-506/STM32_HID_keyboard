# Project File Note

This bundle includes the complete application source, ESP32 sketch, diagrams, wiring, protocol, and setup documentation created for this project.

The original startup assembly file, linker script, CMSIS headers, CubeIDE metadata, and a CubeMX `.ioc` file were not included because they were not supplied in the uploaded files. To make the STM32 application buildable, add the startup file, linker script, vector table, and device support files from the original STM32 project that successfully built the USB HID firmware.

The application source is:

```text
firmware/stm32/main.c
```
