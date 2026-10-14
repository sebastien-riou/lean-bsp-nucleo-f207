/* NUCLEO-F207ZG: the lean-bsp contract on top of the STM32F2 HAL.
 *
 * The vendor-generated main() (Core/Src/main.c) brings the board up and initialises the LED and
 * button pins and USART3, the ST-LINK virtual COM port; its USER CODE 2 section then calls
 * bsp_init() and the application. Heap tracking lives in Core/Src/sysmem.c, next to _sbrk. */
#include "main.h"
#include "bsp_api.h"
#include "bsp_board.h"

extern UART_HandleTypeDef huart3;

static const bsp_capabilities_t capabilities = {
    .board_name = "nucleo-f207",
    .n_uarts = 1,
    .n_leds = 3,
    .n_buttons = 1,
    .has_cycle_counter = true,
    .n_nvm_regions = 0,
    .nvm_regions = NULL,
};

const bsp_capabilities_t *bsp_get_capabilities(void) { return &capabilities; }

/* ---- the application hooks' defaults ---- */

__attribute__((weak)) void app_init(void) {}

__attribute__((weak)) void app_tick(uint32_t tick_ms) { (void)tick_ms; }

__attribute__((weak)) void app_on_fault(uint32_t reason, uintptr_t pc) {
    (void)reason;
    (void)pc;
}

__attribute__((weak)) void app_on_exit(int code) { (void)code; }  /* main() then parks the CPU */

void bsp_init(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  /* the cycle counter */
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    bsp_heap_usage_reset();
}

/* ---- UART 0: USART3, the ST-LINK virtual COM port; stdin and stdout too ---- */

void bsp_uart_tx(unsigned port, const void *buf, size_t len) {
    if (port != 0) return;
    const uint8_t *p = buf;
    while (len) {
        uint16_t n = len > 0xFFFFu ? 0xFFFFu : (uint16_t)len;
        HAL_UART_Transmit(&huart3, (uint8_t *)p, n, HAL_MAX_DELAY);  /* bounded by the baud rate */
        p += n;
        len -= n;
    }
}

/* Polled: what arrives while nobody calls this can overrun the one-byte data register; an
 * interrupt-fed ring buffer is the obvious next step once an application needs continuous input. */
size_t bsp_uart_rx(unsigned port, void *buf, size_t len) {
    if (port != 0) return 0;
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_ORE)) __HAL_UART_CLEAR_OREFLAG(&huart3);
    uint8_t *p = buf;
    size_t n = 0;
    while (n < len && __HAL_UART_GET_FLAG(&huart3, UART_FLAG_RXNE)) p[n++] = (uint8_t)(huart3.Instance->DR & 0xFFu);
    return n;
}

/* newlib's stdout and stdin, overriding the weak ones of Core/Src/syscalls.c */
int _write(int file, char *ptr, int len) {
    (void)file;
    if (len > 0) bsp_uart_tx(0, ptr, (size_t)len);
    return len;
}

int _read(int file, char *ptr, int len) {
    (void)file;
    if (len <= 0) return 0;
    size_t n;
    while ((n = bsp_uart_rx(0, ptr, (size_t)len)) == 0) {
    }
    return (int)n;
}

/* ---- LEDs (LD1 green, LD2 blue, LD3 red) and the user button ---- */

static GPIO_TypeDef *const led_ports[] = {LD1_GPIO_Port, LD2_GPIO_Port, LD3_GPIO_Port};
static const uint16_t led_pins[] = {LD1_Pin, LD2_Pin, LD3_Pin};

void bsp_led_set(unsigned id, bool on) {
    if (id >= sizeof led_pins / sizeof led_pins[0]) return;
    HAL_GPIO_WritePin(led_ports[id], led_pins[id], on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

bool bsp_button_get(unsigned id) {
    return id == 0 && HAL_GPIO_ReadPin(USER_Btn_GPIO_Port, USER_Btn_Pin) == GPIO_PIN_SET;
}

/* ---- time: the DWT cycle counter, at the core clock ---- */

uint32_t bsp_cycle_count(void) { return DWT->CYCCNT; }

uint32_t bsp_cpu_freq_hz(void) { return SystemCoreClock; }

/* On the cycle counter rather than HAL_Delay: works with interrupts masked, and is bounded. */
void bsp_delay_ms(uint32_t ms) {
    const uint32_t per_ms = SystemCoreClock / 1000u;
    while (ms--) {
        const uint32_t start = DWT->CYCCNT;
        while (DWT->CYCCNT - start < per_ms) {
        }
    }
}

void bsp_reset(void) { NVIC_SystemReset(); }

/* The flash accelerator's instruction and data caches and its prefetch buffer: the caches only
 * reset while disabled. */
void bsp_clear_caches(void) {
    __HAL_FLASH_PREFETCH_BUFFER_DISABLE();
    __HAL_FLASH_INSTRUCTION_CACHE_DISABLE();
    __HAL_FLASH_DATA_CACHE_DISABLE();
    __HAL_FLASH_INSTRUCTION_CACHE_RESET();
    __HAL_FLASH_DATA_CACHE_RESET();
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
    __HAL_FLASH_INSTRUCTION_CACHE_ENABLE();
    __HAL_FLASH_DATA_CACHE_ENABLE();
}

/* ---- cycle timer: TIM5, one tick per CPU cycle ----
 * TIM5 counts PCLK1 undivided, which is HCLK with this board's clock configuration (APB1 prescaler 1):
 * one tick per CPU cycle. The handler goes straight into a RAM copy of the vector table, so the
 * interrupt enters it with the exception-entry state. TIM5's channel 1 output, PA0, is high while
 * the timer runs: a trigger for an oscilloscope. */

#define N_VECTORS (16 + 81)  /* the core's exceptions, then this part's interrupts */
static bsp_cycle_timer_handler_t ram_vectors[N_VECTORS] __attribute__((aligned(512)));  /* VTOR: 512 >= 4 * N_VECTORS */
static bool tick_held;
static bool tick_was_on;

/* The tick (SysTick, at TIM5's priority) must not add cycles to what is being timed. */
static void hold_tick(void) {
    if (tick_held) return;
    tick_was_on = SysTick->CTRL & SysTick_CTRL_TICKINT_Msk;
    SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;  /* a tick already pending would fire inside the window */
    tick_held = true;
}

static void release_tick(void) {
    if (!tick_held) return;
    if (tick_was_on) SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
    tick_held = false;
}

void bsp_cycle_timer_init(bsp_cycle_timer_handler_t handler) {
    const bsp_cycle_timer_handler_t *flash_vectors = (const bsp_cycle_timer_handler_t *)SCB->VTOR;
    for (unsigned i = 0; i < N_VECTORS; i++) ram_vectors[i] = flash_vectors[i];
    ram_vectors[16 + TIM5_IRQn] = handler;
    __DSB();
    SCB->VTOR = (uint32_t)ram_vectors;
    __DSB();
    __ISB();
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM5_STOP;  /* halts with the core under a debugger */
    TIM5->CCER |= TIM_CCER_CC1E;                     /* PA0 */
}

void bsp_cycle_timer_start(uint32_t cycles) {
    hold_tick();
    TIM5->ARR = cycles;
    TIM5->CNT = 0;
    TIM5->SR = ~TIM_SR_UIF;
    TIM5->DIER |= TIM_DIER_UIE;
    TIM5->CR1 |= TIM_CR1_OPM;  /* one pulse: stops at the update */
    TIM5->CCR1 = cycles;
    TIM5->CR1 |= TIM_CR1_CEN;
}

void bsp_cycle_timer_stop(void) {
    TIM5->CR1 &= ~TIM_CR1_CEN;
    release_tick();
}

uint32_t bsp_cycle_timer_read(void) { return TIM5->CNT; }

void bsp_cycle_timer_ack(void) {
    bsp_cycle_timer_stop();
    TIM5->SR = ~TIM_SR_UIF;
    TIM5->DIER &= ~TIM_DIER_UIE;
}
