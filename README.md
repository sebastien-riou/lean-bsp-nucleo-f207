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
| Cycle timer (contract 1.1) | TIM5, one tick per CPU cycle at this clock configuration (APB1 undivided). Its interrupt enters the application's handler straight from a RAM copy of the vector table; SysTick is held off while it runs. PA0 (TIM5 channel 1) is high while it runs, a trigger for an oscilloscope. cm-trace traces with it. |
| `app_tick` | from `SysTick_Handler`, every millisecond |
| `app_on_fault` | from the HardFault, MemManage, BusFault and UsageFault handlers (`pc` is 0) |
| `app_on_exit` | default does nothing: `main()` then parks the CPU |
| Flash/NVM | not yet |

## Debugging in STM32CubeIDE

STM32CubeIDE debugs the firmware lean-bsp builds as it is: it imports the ELF and finds every source
through its debug information (absolute paths, built `-Og -g`), the application's, this BSP's and the
HAL's alike. The IDE debugs; lean-bsp keeps building. Written for STM32CubeIDE 2.0.0.

1. Build, from the application: `lbsp build --bsp nucleo-f207`. The firmware is
   `build/nucleo-f207/cortex-m3/<application name>.elf`.
2. In STM32CubeIDE: *File > Import > C/C++ > STM32 Cortex-M Executable*. Executable: that ELF; MCU:
   STM32F207ZGTx. *Finish* creates a project for it and opens its debug configuration.
3. Keep the ST-LINK GDB server probe and press *Debug*: the ELF is flashed and the core stops at
   `main()` in `Core/Src/main.c`. Step into `bsp_init()` or `app_main()`, or set breakpoints in any
   source.
4. After rebuilding with `lbsp build`, debug the same configuration again: it loads the new ELF from
   the same path.

The imported project holds only the ELF. To browse the sources and set breakpoints before execution
reaches them:

- Link the source trees into the project: right-click it, *New > Folder > Advanced > Link to alternate
  location*, once for the application and once for this BSP. They then show in the Project Explorer,
  and *Navigate > Open Resource* (Ctrl+Shift+R) finds any of their files by name. The indexer knows
  none of their include paths, so it may flag symbols as unresolved; this does not affect debugging.
- Or open a single file with *File > Open File...*: breakpoints set in it work as well.
- Or break on a function by name, with no file open: *Add Function Breakpoint (C/C++)* in the
  *Breakpoints* view's menu, e.g. `bsp_delay_ms`.

UART 0 is the ST-LINK virtual COM port, at 115200 baud. When lean-hwbridge holds that port, read it with
`hwbridge serial cat nucleo-f207`. lean-hwbridge uses the probe only while it flashes or resets, so
leave `lbsp load` and `lbsp run` alone during a debug session.

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

To change the board support itself in STM32CubeMX, open `nucleo-f207.ioc`; lean-bsp code survives
regeneration.
