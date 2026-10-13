# lean-bsp-nucleo-f207

The [lean-bsp](https://github.com/sebastien-riou/lean-bsp) BSP for the ST NUCLEO-F207ZG board
(STM32F207ZG, Cortex-M3). Any lean-bsp application builds and runs on it:

````
lbsp build-load-run --bsp nucleo-f207 --until 'hello-world done' --timeout 60
````

## What it needs

- The arm-none-eabi toolchain. `CROSS_COMPILE` selects it, e.g.
  `CROSS_COMPILE=/opt/toolchains/arm-none-eabi-gcc/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin/arm-none-eabi-`;
  without it, `arm-none-eabi-gcc` is taken from `PATH`.
- For `load` and `run`, [lean-hwbridge](https://github.com/sebastien-riou/lean-hwbridge) serving the
  board: `hwbridge` on `PATH`, or the command in `HWBRIDGE` (e.g. `HWBRIDGE="python3 -m hwbridge"`).
  The board is its usbman nickname, `HWBRIDGE_BOARD`, by default `nucleo-f207`. Without a bridge,
  `load` prints the STM32CubeProgrammer command to flash the board by hand.

## How the contract maps to the board

| Primitive | On NUCLEO-F207ZG |
|---|---|
| UART 0 (and `printf`, `stdin`) | USART3, the ST-LINK virtual COM port, 115200 8N1. Reception is polled: bytes arriving while nobody reads can overrun. |
| LEDs | 0 green (LD1), 1 blue (LD2), 2 red (LD3) |
| Button | 0 is the blue user button |
| CPU clock | 8 MHz, as cm-trace-stm32f207 configures it: the ST-LINK's clock output (HSE bypass), PLL off, zero flash wait states |
| Cycle counter, CPU frequency | DWT `CYCCNT`, at `SystemCoreClock` |
| Delay | busy loop on the cycle counter: works with interrupts masked |
| Cache clear | the flash accelerator's instruction and data caches reset, prefetch buffer toggled |
| Heap usage | growth of the heap end since the last reset, tracked in `_sbrk` (`Core/Src/sysmem.c`) |
| Reset | `NVIC_SystemReset()` |
| `app_tick` | from `SysTick_Handler`, every millisecond |
| `app_on_fault` | from the HardFault, MemManage, BusFault and UsageFault handlers (`pc` is 0) |
| `app_on_exit` | default does nothing: `main()` then parks the CPU |
| Flash/NVM | not yet |

## Layout

The board support is an STM32CubeMX project (`nucleo-f207.ioc`), taken from
[cm-trace-stm32f207](https://github.com/sebastien-riou/cm-trace-stm32f207) and kept regeneration-safe:
lean-bsp code is only in `USER CODE` sections of the generated files (the glue in `main()`,
`app_tick` and `app_on_fault` in `stm32f2xx_it.c`) and in files CubeMX does not generate. The project
still configures TIM5 as cm-trace did; nothing starts it.

- `Core/`, `Drivers/`, `startup_stm32f207xx.s`: the vendor-generated bring-up (HAL and CMSIS; device
  headers for this part only).
- `src/bsp_nucleo_f207.c`: the primitives, `bsp_init()` and the default application hooks.
- `include/bsp_api.h`: the contract header, identical to lean-bsp's canonical copy (`lbsp check` verifies it).
- `linker/`: `memory.ld` (this board's memory map), and the Cortex-M `sections_head/data/tail.ld` and
  `default.ld`, identical to the other Cortex-M lean-bsp BSPs'. An application needing its own
  sections copies `default.ld`.
- `bsp.toml`, `bsp_cli.py`: what lean-bsp reads and runs. `bsp_cli.py` is this board's data on top of lean-bsp's
  board helpers (`leanbsp.backends`), which lean-bsp runs it with.

To open it in STM32CubeIDE, regenerate an IDE project from `nucleo-f207.ioc` with STM32CubeMX.
