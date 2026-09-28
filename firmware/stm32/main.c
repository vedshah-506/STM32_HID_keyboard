/*
 * Bare-metal USB HID keyboard for STM32F401RCT6 (USB OTG FS, device mode)
 *
 * Final behaviour:
 *   - LED blinks fast while the host has not enumerated us, slow once it has
 *     (and stays solid ON while the host's Caps Lock is on -> proves the
 *     host->device SET_REPORT path works too).
 *   - If the onboard button is pressed, run_demo() executes the built-in HID
 *     sequence and its continuous shell-command loop.
 *   - If the button is not pressed, USART2 receives an ESP32 message ending
 *     in LF (0x0A), and the STM32 types it through USB HID.
 *
 * Adjust the USER CONFIG block below to match your board.
 */
#include <stdint.h>
#include <stddef.h>

/* ========================================================================== */
/*                               USER CONFIG                                  */
/* ========================================================================== */
#define USE_HSE         1       /* 1 = use external crystal (recommended for USB) */
#define HSE_MHZ         25      /* crystal in MHz: 25 (Black Pill), 8, 12, 16 ... integer only */

#define LED_PORT        GPIOC_BASE
#define LED_PIN         13      /* Black Pill = PC13 */
#define LED_ACTIVE_LOW  1

#define BTN_ENABLE      1
#define BTN_PORT        GPIOA_BASE
#define BTN_PIN         0       /* Black Pill KEY = PA0 */
#define BTN_ACTIVE_LOW  1

#define SYSCLK_HZ       84000000UL

/* ========================================================================== */
/*                          REGISTER DEFINITIONS                              */
/* ========================================================================== */
#define REG32(a)            (*(volatile uint32_t *)(uintptr_t)(a))

/* RCC / FLASH */
#define RCC_BASE            0x40023800UL
#define RCC_CR              REG32(RCC_BASE + 0x00)
#define RCC_PLLCFGR         REG32(RCC_BASE + 0x04)
#define RCC_CFGR            REG32(RCC_BASE + 0x08)
#define RCC_AHB1ENR         REG32(RCC_BASE + 0x30)
#define RCC_AHB2ENR         REG32(RCC_BASE + 0x34)
#define RCC_APB1ENR         REG32(RCC_BASE + 0x40)
#define FLASH_ACR           REG32(0x40023C00UL)

/* GPIO */
#define GPIOA_BASE          0x40020000UL
#define GPIOB_BASE          0x40020400UL
#define GPIOC_BASE          0x40020800UL
#define GPIO_MODER(b)       REG32((b) + 0x00)
#define GPIO_OSPEEDR(b)     REG32((b) + 0x08)
#define GPIO_PUPDR(b)       REG32((b) + 0x0C)
#define GPIO_IDR(b)         REG32((b) + 0x10)
#define GPIO_BSRR(b)        REG32((b) + 0x18)
#define GPIO_AFRL(b)        REG32((b) + 0x20)
#define GPIO_AFRH(b)        REG32((b) + 0x24)

/* USART2: APB1 peripheral, asynchronous UART, 115200 8N1. */
#define USART2_BASE         0x40004400UL
#define USART2_SR           REG32(USART2_BASE + 0x00)
#define USART2_DR           REG32(USART2_BASE + 0x04)
#define USART2_BRR          REG32(USART2_BASE + 0x08)
#define USART2_CR1          REG32(USART2_BASE + 0x0C)
#define USART2_CR2          REG32(USART2_BASE + 0x10)
#define USART2_CR3          REG32(USART2_BASE + 0x14)

#define USART_SR_RXNE       (1u << 5)
#define USART_SR_ORE        (1u << 3)
#define USART_CR1_RE        (1u << 2)
#define USART_CR1_TE        (1u << 3)
#define USART_CR1_UE        (1u << 13)

/* Cortex-M */
#define SYST_CSR            REG32(0xE000E010UL)
#define SYST_RVR            REG32(0xE000E014UL)
#define SYST_CVR            REG32(0xE000E018UL)
#define NVIC_ISER2          REG32(0xE000E108UL)     /* IRQ 64..95 */
#define OTG_FS_IRQn         67

/* USB OTG FS core */
#define OTG_BASE            0x50000000UL
#define OTG(off)            REG32(OTG_BASE + (off))
#define GAHBCFG             OTG(0x008)
#define GUSBCFG             OTG(0x00C)
#define GRSTCTL             OTG(0x010)
#define GINTSTS             OTG(0x014)
#define GINTMSK             OTG(0x018)
#define GRXSTSP             OTG(0x020)      /* NOTE: 0x1C is GRXSTSR (peek, does not pop!) */
#define GRXFSIZ             OTG(0x024)
#define DIEPTXF0            OTG(0x028)
#define GCCFG               OTG(0x038)
#define DIEPTXF(n)          OTG(0x104 + (((n) - 1) * 4))
#define DCFG                OTG(0x800)
#define DCTL                OTG(0x804)      /* soft-disconnect lives HERE, not in DCFG */
#define DIEPMSK             OTG(0x810)
#define DOEPMSK             OTG(0x814)
#define DAINT               OTG(0x818)
#define DAINTMSK            OTG(0x81C)
#define DIEPCTL(n)          OTG(0x900 + (n) * 0x20)
#define DIEPINT(n)          OTG(0x908 + (n) * 0x20)
#define DIEPTSIZ(n)         OTG(0x910 + (n) * 0x20)
#define DOEPCTL(n)          OTG(0xB00 + (n) * 0x20)
#define DOEPINT(n)          OTG(0xB08 + (n) * 0x20)
#define DOEPTSIZ(n)         OTG(0xB10 + (n) * 0x20)
#define PCGCCTL             OTG(0xE00)
#define FIFO(n)             ((volatile uint32_t *)(OTG_BASE + 0x1000 + (n) * 0x1000))

/* GAHBCFG / GUSBCFG / GRSTCTL / GCCFG bits */
#define GAHBCFG_GINT        (1u << 0)
#define GUSBCFG_PHYSEL      (1u << 6)
#define GUSBCFG_FDMOD       (1u << 30)
#define GUSBCFG_FHMOD       (1u << 29)
#define GUSBCFG_TRDT_MASK   (0xFu << 10)
#define GUSBCFG_TRDT(x)     ((uint32_t)(x) << 10)
#define GRSTCTL_CSRST       (1u << 0)
#define GRSTCTL_RXFFLSH     (1u << 4)
#define GRSTCTL_TXFFLSH     (1u << 5)
#define GRSTCTL_AHBIDL      (1u << 31)
#define GCCFG_PWRDWN        (1u << 16)
#define GCCFG_NOVBUSSENS    (1u << 21)

/* GINTSTS / GINTMSK bits */
#define GINT_RXFLVL         (1u << 4)
#define GINT_USBSUSP        (1u << 11)
#define GINT_USBRST         (1u << 12)
#define GINT_ENUMDNE        (1u << 13)
#define GINT_IEPINT         (1u << 18)
#define GINT_OEPINT         (1u << 19)
#define GINT_IISOIXFR       (1u << 20)
#define GINT_IPXFR          (1u << 21)
#define GINT_WKUINT         (1u << 31)

/* DCTL bits */
#define DCTL_RWUSIG         (1u << 0)
#define DCTL_SDIS           (1u << 1)
#define DCTL_CGINAK         (1u << 8)

/* Endpoint control bits (DIEPCTL / DOEPCTL) */
#define EPCTL_USBAEP        (1u << 15)
#define EPCTL_STALL         (1u << 21)
#define EPCTL_CNAK          (1u << 26)
#define EPCTL_SNAK          (1u << 27)
#define EPCTL_SD0PID        (1u << 28)
#define EPCTL_EPDIS         (1u << 30)
#define EPCTL_EPENA         (1u << 31)

/* Endpoint interrupt bits */
#define DIEPINT_XFRC        (1u << 0)
#define DIEPINT_EPDISD      (1u << 1)
#define DIEPINT_TOC         (1u << 3)
#define DIEPINT_INEPNE      (1u << 6)
#define DOEPINT_XFRC        (1u << 0)
#define DOEPINT_STUP        (1u << 3)
#define DOEPINT_B2BSTUP     (1u << 6)

/* GRXSTSP packet status */
#define PKTSTS_OUT_DATA     2u
#define PKTSTS_SETUP_DATA   6u

#define EP0_MPS             64u
#define HID_EP_MPS          8u
#define NUM_EPS             4       /* F401 OTG_FS: EP0..EP3 */

/* ========================================================================== */
/*                           USB DESCRIPTORS                                  */
/* ========================================================================== */
static const uint8_t DeviceDescriptor[18] = {
    18, 0x01,           /* bLength, bDescriptorType = DEVICE */
    0x00, 0x02,         /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,   /* class/subclass/protocol: defined at interface level */
    EP0_MPS,            /* bMaxPacketSize0 */
    0x09, 0x12,         /* idVendor  0x1209 (pid.codes open VID) */
    0x01, 0x00,         /* idProduct 0x0001 (pid.codes TEST pid - hobby use only) */
    0x00, 0x01,         /* bcdDevice 1.00 */
    1, 2, 0,            /* iManufacturer, iProduct, iSerialNumber */
    1                   /* bNumConfigurations */
};

static const uint8_t HID_ReportDescriptor[63] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,         /* Generic Desktop / Keyboard / Application */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,         /* Key codes: modifiers E0..E7 */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,                                 /* Input: modifier byte */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,         /* Input: reserved byte */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08,         /* LEDs: 5 bits */
    0x19, 0x01, 0x29, 0x05, 0x91, 0x02,         /* Output: LED bits */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,         /* Output: padding */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, /* Input: 6 key array */
    0xC0
};

static const uint8_t ConfigDescriptor[34] = {
    /* Configuration */
    9, 0x02, 34, 0x00, 1, 1, 0,
    0x80,               /* bus powered, NO remote wakeup (not implemented) */
    50,                 /* 100 mA */
    /* Interface 0: HID, boot subclass, keyboard protocol */
    9, 0x04, 0, 0, 1, 0x03, 0x01, 0x01, 0,
    /* HID descriptor (offset 18) */
    9, 0x21, 0x11, 0x01, 0x00, 1, 0x22, sizeof(HID_ReportDescriptor), 0x00,
    /* Endpoint 1 IN, interrupt, 8 bytes, 10 ms */
    7, 0x05, 0x81, 0x03, HID_EP_MPS, 0x00, 10
};

_Static_assert(sizeof(ConfigDescriptor) == 34, "wTotalLength mismatch");
_Static_assert(sizeof(HID_ReportDescriptor) == 63, "report descriptor size");

static const char STR_MANUFACTURER[] = "DIY Bare-metal";
static const char STR_PRODUCT[]      = "STM32F401 Bare-metal Keyboard";

/* ========================================================================== */
/*                           GLOBAL STATE                                     */
/* ========================================================================== */
typedef enum { EP0_IDLE, EP0_DATA_IN, EP0_DATA_OUT, EP0_STATUS_IN } ep0_state_t;

static volatile ep0_state_t ep0_state;
static const uint8_t *ep0_ptr;
static uint16_t       ep0_rem;
static uint8_t        ep0_zlp;
static uint8_t        setup_buf[8];
static uint8_t        ep0_out_buf[EP0_MPS];
static uint16_t       ep0_out_len;
static uint8_t        ep0_tmp[8];
static uint8_t        str_buf[80];
static uint8_t        last_report[8];

static uint8_t        cfg_value;
static uint8_t        hid_idle;
static uint8_t        hid_protocol = 1;

volatile uint8_t  usb_configured;
volatile uint8_t  usb_suspended;
volatile uint8_t  ep1_busy;
volatile uint8_t  caps_lock;
volatile uint32_t ms_ticks;

/* ESP32-to-STM32 messages are bytes terminated by LF (0x0A). */
#define UART_RX_BUFFER_SIZE 128u
static char uart_rx_buffer[UART_RX_BUFFER_SIZE];
static uint32_t uart_rx_index;
static volatile uint8_t uart_message_ready;
static volatile uint8_t uart_rx_overflow;

/* ========================================================================== */
/*                           SMALL HELPERS                                    */
/* ========================================================================== */
static inline uint32_t irq_save(void)
{
    uint32_t p;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(p) :: "memory");
    return p;
}
static inline void irq_restore(uint32_t p)
{
    __asm volatile("msr primask, %0" :: "r"(p) : "memory");
}

void SysTick_Handler(void) { ms_ticks++; }

static void delay_ms(uint32_t ms)
{
    uint32_t s = ms_ticks;
    while ((uint32_t)(ms_ticks - s) < ms) { }
}

/* ========================================================================== */
/*                       1. CLOCKS: 84 MHz SYSCLK, 48 MHz USB                 */
/* ========================================================================== */
static void clock_init(void)
{
    /* 84 MHz needs 2 wait states; also turn on prefetch + I/D cache */
    FLASH_ACR = 2u | (1u << 8) | (1u << 9) | (1u << 10);
    while ((FLASH_ACR & 7u) != 2u) { }

    uint32_t pllm = 16, pllsrc = 0;         /* default: HSI 16 MHz / 16 = 1 MHz */
#if USE_HSE
    RCC_CR |= (1u << 16);                   /* HSEON */
    for (uint32_t t = 500000; t && !(RCC_CR & (1u << 17)); t--) { }
    if (RCC_CR & (1u << 17)) {              /* HSERDY */
        pllm = HSE_MHZ;                     /* HSE / HSE_MHZ = 1 MHz */
        pllsrc = 1;
    } else {
        RCC_CR &= ~(1u << 16);              /* no crystal -> fall back to HSI */
    }
#endif
    /* VCO = 1 MHz * 336 = 336 MHz ; SYSCLK = /4 = 84 MHz ; USB = /7 = 48 MHz */
    RCC_PLLCFGR = (pllm << 0) | (336u << 6) | (1u << 16) /* PLLP=/4 */
                | (pllsrc << 22) | (7u << 24);

    RCC_CR |= (1u << 24);                   /* PLLON */
    while (!(RCC_CR & (1u << 25))) { }      /* PLLRDY */

    /* AHB /1, APB1 /2 (42 MHz max), APB2 /1 */
    RCC_CFGR = (RCC_CFGR & ~((0xFu << 4) | (7u << 10) | (7u << 13))) | (4u << 10);

    RCC_CFGR = (RCC_CFGR & ~3u) | 2u;       /* SW = PLL */
    while (((RCC_CFGR >> 2) & 3u) != 2u) { }
}

static void systick_init(void)
{
    SYST_RVR = (SYSCLK_HZ / 1000u) - 1u;
    SYST_CVR = 0;
    SYST_CSR = 7u;                          /* core clock, IRQ on, enable */
}

/* ========================================================================== */
/*                               2. GPIO                                      */
/* ========================================================================== */
static void gpio_mode(uint32_t base, uint32_t pin, uint32_t mode)
{
    GPIO_MODER(base) = (GPIO_MODER(base) & ~(3u << (pin * 2))) | (mode << (pin * 2));
}
static void gpio_pull(uint32_t base, uint32_t pin, uint32_t pull)
{
    GPIO_PUPDR(base) = (GPIO_PUPDR(base) & ~(3u << (pin * 2))) | (pull << (pin * 2));
}
static void led_set(int on)
{
    int level = LED_ACTIVE_LOW ? !on : on;
    GPIO_BSRR(LED_PORT) = level ? (1u << LED_PIN) : (1u << (LED_PIN + 16));
}
static int button_pressed(void)
{
#if BTN_ENABLE
    int level = (GPIO_IDR(BTN_PORT) >> BTN_PIN) & 1;
    return BTN_ACTIVE_LOW ? !level : level;
#else
    return 0;
#endif
}

static void gpio_init(void)
{
    RCC_AHB1ENR |= 7u;                      /* GPIOA, GPIOB, GPIOC clocks */

    /* PA2 = USART2_TX, PA3 = USART2_RX -> AF7. */
    gpio_mode(GPIOA_BASE, 2, 2);
    gpio_mode(GPIOA_BASE, 3, 2);
    GPIO_OSPEEDR(GPIOA_BASE) |= (3u << 4) | (3u << 6);
    gpio_pull(GPIOA_BASE, 2, 1);             /* idle-high UART lines */
    gpio_pull(GPIOA_BASE, 3, 1);
    GPIO_AFRL(GPIOA_BASE) = (GPIO_AFRL(GPIOA_BASE) & ~((0xFu << 8) | (0xFu << 12)))
                          | (7u << 8) | (7u << 12);

    /* PA11 = OTG_FS_DM, PA12 = OTG_FS_DP -> AF10 (read-modify-write keeps SWD pins!) */
    gpio_mode(GPIOA_BASE, 11, 2);
    gpio_mode(GPIOA_BASE, 12, 2);
    GPIO_OSPEEDR(GPIOA_BASE) |= (3u << 22) | (3u << 24);
    gpio_pull(GPIOA_BASE, 11, 0);
    gpio_pull(GPIOA_BASE, 12, 0);
    GPIO_AFRH(GPIOA_BASE) = (GPIO_AFRH(GPIOA_BASE) & ~((0xFu << 12) | (0xFu << 16)))
                          | (10u << 12) | (10u << 16);

    led_set(0);
    gpio_mode(LED_PORT, LED_PIN, 1);        /* push-pull output */
#if BTN_ENABLE
    gpio_mode(BTN_PORT, BTN_PIN, 0);
    gpio_pull(BTN_PORT, BTN_PIN, BTN_ACTIVE_LOW ? 1 : 2);
#endif
}

/* ========================================================================== */
/*                         2a. USART2 UART                                   */
/* ========================================================================== */
static void usart2_init(void)
{
    /* APB1 = 42 MHz with the clock tree configured above. */
    const uint32_t pclk1_hz = SYSCLK_HZ / 2u;

    RCC_APB1ENR |= (1u << 17);               /* USART2EN */

    USART2_CR1 = 0;
    USART2_CR2 = 0;                          /* 1 stop bit */
    USART2_CR3 = 0;                          /* no flow control */
    USART2_BRR = (pclk1_hz + 57600u) / 115200u;
    USART2_CR1 = USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
}

/* ========================================================================== */
/*                     3. FIFO ACCESS (alignment-safe)                        */
/* ========================================================================== */
static void fifo_write(uint8_t ep, const uint8_t *buf, uint16_t len)
{
    volatile uint32_t *fifo = FIFO(ep);
    while (len) {
        uint32_t w = 0;
        uint16_t n = (len >= 4) ? 4 : len;
        for (uint16_t i = 0; i < n; i++) w |= (uint32_t)buf[i] << (8 * i);
        *fifo = w;
        buf += n;
        len -= n;
    }
}

/* Pops `len` bytes from RX FIFO; stores at most `cap` of them. Always drains fully. */
static uint16_t fifo_read(uint8_t *dst, uint16_t len, uint16_t cap)
{
    volatile uint32_t *fifo = FIFO(0);
    uint16_t stored = 0;
    while (len) {
        uint32_t w = *fifo;
        uint16_t n = (len >= 4) ? 4 : len;
        for (uint16_t i = 0; i < n; i++) {
            if (dst && stored < cap) dst[stored++] = (uint8_t)(w >> (8 * i));
        }
        len -= n;
    }
    return stored;
}

static void flush_txfifo(uint32_t num)          /* num = 0..3, or 0x10 for all */
{
    GRSTCTL = (num << 6) | GRSTCTL_TXFFLSH;
    for (uint32_t t = 100000; t && (GRSTCTL & GRSTCTL_TXFFLSH); t--) { }
}
static void flush_rxfifo(void)
{
    GRSTCTL = GRSTCTL_RXFFLSH;
    for (uint32_t t = 100000; t && (GRSTCTL & GRSTCTL_RXFFLSH); t--) { }
}

/* ========================================================================== */
/*                       4. ENDPOINT 0 (CONTROL) ENGINE                       */
/* ========================================================================== */
/* Arm EP0 OUT: ready for up to 3 back-to-back SETUP packets (or a ZLP status) */
static void ep0_out_arm(void)
{
    DOEPTSIZ(0) = (3u << 29) | (1u << 19) | (3u * 8u);
    DOEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
}

/* Arm EP0 OUT to receive a control-write DATA stage (max one 64-byte packet) */
static void ep0_out_arm_data(void)
{
    DOEPTSIZ(0) = (3u << 29) | (1u << 19) | EP0_MPS;
    DOEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
}

static void ep0_stall(void)
{
    DIEPCTL(0) |= EPCTL_STALL;
    ep0_out_arm();
    DOEPCTL(0) |= EPCTL_STALL;              /* core clears it on next SETUP */
    ep0_state = EP0_IDLE;
}

static void ep0_tx_chunk(void)
{
    uint16_t n = (ep0_rem > EP0_MPS) ? EP0_MPS : ep0_rem;
    DIEPTSIZ(0) = (1u << 19) | n;
    DIEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
    if (n) fifo_write(0, ep0_ptr, n);
    ep0_ptr += n;
    ep0_rem -= n;
}

/* Start a control-read data stage. Handles multi-packet + trailing ZLP. */
static void ep0_start_in(const uint8_t *p, uint16_t len, uint16_t wLength)
{
    ep0_zlp = (len < wLength) && (len != 0) && ((len % EP0_MPS) == 0);
    if (len > wLength) len = wLength;
    ep0_ptr = p;
    ep0_rem = len;
    ep0_state = EP0_DATA_IN;
    ep0_out_arm();                          /* for the host's status-stage ZLP */
    ep0_tx_chunk();
}

static void ep0_send_status(void)
{
    ep0_state = EP0_STATUS_IN;
    ep0_out_arm();
    DIEPTSIZ(0) = (1u << 19);
    DIEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
}

/* ---- endpoint 1 (HID IN) management ---- */
static void ep1_abort(void)                 /* stop any pending transfer + flush FIFO */
{
    uint32_t pm = irq_save();
    if (DIEPCTL(1) & EPCTL_EPENA) {
        DIEPCTL(1) |= EPCTL_SNAK;
        for (uint32_t t = 100000; t && !(DIEPINT(1) & DIEPINT_INEPNE); t--) { }
        DIEPCTL(1) |= EPCTL_EPDIS | EPCTL_SNAK;
        for (uint32_t t = 100000; t && !(DIEPINT(1) & DIEPINT_EPDISD); t--) { }
        DIEPINT(1) = DIEPINT_EPDISD | DIEPINT_INEPNE;
    }
    flush_txfifo(1);
    ep1_busy = 0;
    irq_restore(pm);
}

static void ep1_deactivate(void)
{
    usb_configured = 0;
    ep1_abort();
    DIEPCTL(1) &= ~EPCTL_USBAEP;
    DAINTMSK &= ~(1u << 1);
}

static void ep1_activate(void)
{
    ep1_deactivate();
    DIEPINT(1) = 0xFB7Fu;
    DIEPTSIZ(1) = 0;
    /* MPS=8, active, interrupt type, TX FIFO #1, start with DATA0 */
    DIEPCTL(1) = HID_EP_MPS | EPCTL_USBAEP | (3u << 18) | (1u << 22) | EPCTL_SD0PID;
    DAINTMSK |= (1u << 1);
    usb_configured = 1;
}

/* ---- standard requests ---- */
static const uint8_t *make_string(const char *s, uint16_t *len)
{
    uint8_t n = 0;
    while (s[n] && n < 38) {
        str_buf[2 + 2 * n] = (uint8_t)s[n];
        str_buf[3 + 2 * n] = 0;
        n++;
    }
    str_buf[0] = (uint8_t)(2 + 2 * n);
    str_buf[1] = 0x03;
    *len = str_buf[0];
    return str_buf;
}

static void ep0_get_descriptor(uint16_t wValue, uint16_t wLength)
{
    uint8_t type = wValue >> 8, idx = wValue & 0xFF;
    const uint8_t *p = NULL;
    uint16_t len = 0;

    switch (type) {
    case 0x01: p = DeviceDescriptor; len = sizeof(DeviceDescriptor); break;
    case 0x02: if (idx == 0) { p = ConfigDescriptor; len = sizeof(ConfigDescriptor); } break;
    case 0x03:
        if (idx == 0) {                     /* LANGID: US English */
            str_buf[0] = 4; str_buf[1] = 3; str_buf[2] = 0x09; str_buf[3] = 0x04;
            p = str_buf; len = 4;
        } else if (idx == 1) p = make_string(STR_MANUFACTURER, &len);
        else if (idx == 2)   p = make_string(STR_PRODUCT, &len);
        break;
    case 0x21: p = &ConfigDescriptor[18]; len = 9; break;                       /* HID descriptor */
    case 0x22: p = HID_ReportDescriptor;  len = sizeof(HID_ReportDescriptor); break; /* Report descriptor */
    default: break;                         /* device_qualifier etc. -> STALL (correct for FS-only) */
    }

    if (p) ep0_start_in(p, len, wLength);
    else   ep0_stall();
}

static void ep0_std_request(uint8_t req, uint16_t wValue, uint16_t wLength)
{
    switch (req) {
    case 0x00:                              /* GET_STATUS: bus powered, no remote wakeup */
        ep0_tmp[0] = ep0_tmp[1] = 0;
        ep0_start_in(ep0_tmp, 2, wLength);
        break;
    case 0x01:                              /* CLEAR_FEATURE (e.g. ENDPOINT_HALT) -> accept */
        ep0_send_status();
        break;
    case 0x05:                              /* SET_ADDRESS: apply NOW, before the status stage */
        DCFG = (DCFG & ~(0x7Fu << 4)) | ((uint32_t)(wValue & 0x7F) << 4);
        ep0_send_status();
        break;
    case 0x06:
        ep0_get_descriptor(wValue, wLength);
        break;
    case 0x08:                              /* GET_CONFIGURATION */
        ep0_tmp[0] = cfg_value;
        ep0_start_in(ep0_tmp, 1, wLength);
        break;
    case 0x09:                              /* SET_CONFIGURATION */
        if ((wValue & 0xFF) == 1)      { cfg_value = 1; ep1_activate(); ep0_send_status(); }
        else if ((wValue & 0xFF) == 0) { cfg_value = 0; ep1_deactivate(); ep0_send_status(); }
        else ep0_stall();
        break;
    case 0x0A:                              /* GET_INTERFACE */
        ep0_tmp[0] = 0;
        ep0_start_in(ep0_tmp, 1, wLength);
        break;
    case 0x0B:                              /* SET_INTERFACE (only alt 0 exists) */
        if (wValue == 0) ep0_send_status(); else ep0_stall();
        break;
    default:
        ep0_stall();
        break;
    }
}

/* ---- HID class requests ---- */
static void ep0_class_request(uint8_t req, uint16_t wValue, uint16_t wLength)
{
    switch (req) {
    case 0x01: ep0_start_in(last_report, 8, wLength); break;                    /* GET_REPORT   */
    case 0x02: ep0_tmp[0] = hid_idle;     ep0_start_in(ep0_tmp, 1, wLength); break; /* GET_IDLE     */
    case 0x03: ep0_tmp[0] = hid_protocol; ep0_start_in(ep0_tmp, 1, wLength); break; /* GET_PROTOCOL */
    case 0x09:                                                                  /* SET_REPORT (LEDs) */
        if (wLength == 0 || wLength > EP0_MPS) { ep0_stall(); break; }
        ep0_state = EP0_DATA_OUT;
        ep0_out_len = 0;
        ep0_out_arm_data();
        break;
    case 0x0A: hid_idle = wValue >> 8;        ep0_send_status(); break;         /* SET_IDLE     */
    case 0x0B: hid_protocol = wValue & 0xFF;  ep0_send_status(); break;         /* SET_PROTOCOL */
    default:   ep0_stall(); break;
    }
}

static void ep0_handle_setup(void)
{
    uint8_t  bm      = setup_buf[0];
    uint8_t  req     = setup_buf[1];
    uint16_t wValue  = (uint16_t)(setup_buf[2] | (setup_buf[3] << 8));
    uint16_t wLength = (uint16_t)(setup_buf[6] | (setup_buf[7] << 8));

    switch (bm & 0x60) {
    case 0x00: ep0_std_request(req, wValue, wLength);   break;
    case 0x20: ep0_class_request(req, wValue, wLength); break;
    default:   ep0_stall();                             break;
    }
}

static void ep0_in_done(void)
{
    if (ep0_state == EP0_DATA_IN) {
        if (ep0_rem)      ep0_tx_chunk();
        else if (ep0_zlp) { ep0_zlp = 0; ep0_tx_chunk(); }   /* rem == 0 -> sends ZLP */
        else              ep0_state = EP0_IDLE;
    } else if (ep0_state == EP0_STATUS_IN) {
        ep0_state = EP0_IDLE;
    }
}

static void ep0_out_done(void)
{
    if (ep0_state == EP0_DATA_OUT) {
        if (ep0_out_len >= 1) caps_lock = (ep0_out_buf[0] >> 1) & 1u;  /* LED report: bit1 = Caps */
        ep0_send_status();
    } else {
        ep0_state = EP0_IDLE;
        ep0_out_arm();
    }
}

/* ========================================================================== */
/*                      5. INTERRUPT SERVICE ROUTINE                          */
/* ========================================================================== */
static void usb_on_reset(void)
{
    DCTL &= ~DCTL_RWUSIG;
    if (DIEPCTL(1) & EPCTL_USBAEP) ep1_deactivate();
    usb_configured = 0;
    cfg_value = 0;
    ep0_state = EP0_IDLE;

    flush_txfifo(0x10);
    flush_rxfifo();

    for (uint32_t i = 0; i < NUM_EPS; i++) {
        DIEPINT(i) = 0xFB7Fu;
        DIEPCTL(i) &= ~EPCTL_STALL;
        DIEPCTL(i) |= EPCTL_SNAK;
        DOEPINT(i) = 0xFB7Fu;
        DOEPCTL(i) &= ~EPCTL_STALL;
        DOEPCTL(i) |= EPCTL_SNAK;
    }

    DAINTMSK = (1u << 0) | (1u << 16);              /* EP0 IN + EP0 OUT */
    DOEPMSK  = DOEPINT_STUP | DOEPINT_XFRC;
    DIEPMSK  = DIEPINT_XFRC | DIEPINT_TOC;
    DCFG &= ~(0x7Fu << 4);                          /* address 0 */
    ep0_out_arm();                                  /* ready for first SETUP */
}

static void usb_on_rxflvl(void)
{
    GINTMSK &= ~GINT_RXFLVL;
    uint32_t r     = GRXSTSP;                       /* pop status */
    uint8_t  ep    = r & 0xF;
    uint16_t bcnt  = (r >> 4) & 0x7FF;
    uint8_t  pkt   = (r >> 17) & 0xF;

    if (ep == 0 && pkt == PKTSTS_SETUP_DATA && bcnt == 8) {
        fifo_read(setup_buf, 8, 8);
    } else if (ep == 0 && pkt == PKTSTS_OUT_DATA && bcnt) {
        ep0_out_len = fifo_read(ep0_out_buf, bcnt, sizeof(ep0_out_buf));
    } else if (bcnt) {
        fifo_read(NULL, bcnt, 0);                   /* drain anything unexpected */
    }
    GINTMSK |= GINT_RXFLVL;
}

static void usb_on_iep(void)
{
    uint32_t daint = DAINT & DAINTMSK;
    if (daint & (1u << 0)) {
        uint32_t f = DIEPINT(0);
        if (f & DIEPINT_XFRC) { DIEPINT(0) = DIEPINT_XFRC; ep0_in_done(); }
        if (f & DIEPINT_TOC)  { DIEPINT(0) = DIEPINT_TOC; }
    }
    if (daint & (1u << 1)) {
        uint32_t f = DIEPINT(1);
        if (f & DIEPINT_XFRC) { DIEPINT(1) = DIEPINT_XFRC; ep1_busy = 0; }
        if (f & DIEPINT_TOC)  { DIEPINT(1) = DIEPINT_TOC; }
    }
}

static void usb_on_oep(void)
{
    uint32_t daint = DAINT & DAINTMSK;
    if (daint & (1u << 16)) {
        uint32_t f = DOEPINT(0);                    /* one snapshot: XFRC first, then STUP */
        DOEPINT(0) = f & (DOEPINT_XFRC | DOEPINT_STUP | DOEPINT_B2BSTUP);
        if (f & DOEPINT_XFRC) ep0_out_done();
        if (f & DOEPINT_STUP) ep0_handle_setup();
    }
}

void OTG_FS_IRQHandler(void)
{
    uint32_t st = GINTSTS & GINTMSK;

    if (st & GINT_RXFLVL)  usb_on_rxflvl();
    if (st & GINT_USBRST)  { GINTSTS = GINT_USBRST; usb_on_reset(); }
    if (st & GINT_ENUMDNE) {
        GINTSTS = GINT_ENUMDNE;
        DIEPCTL(0) &= ~3u;                          /* EP0 MPS = 64 */
        DCTL |= DCTL_CGINAK;
    }
    if (st & GINT_OEPINT)  usb_on_oep();
    if (st & GINT_IEPINT)  usb_on_iep();
    if (st & GINT_USBSUSP) { GINTSTS = GINT_USBSUSP; usb_suspended = 1; }
    if (st & GINT_WKUINT)  { GINTSTS = GINT_WKUINT;  usb_suspended = 0; }
    if (st & (GINT_IISOIXFR | GINT_IPXFR)) GINTSTS = GINT_IISOIXFR | GINT_IPXFR;
}

/* ========================================================================== */
/*                         6. USB CORE INITIALISATION                         */
/* ========================================================================== */
static void usb_core_reset(void)
{
    for (uint32_t t = 200000; t && !(GRSTCTL & GRSTCTL_AHBIDL); t--) { }
    GRSTCTL |= GRSTCTL_CSRST;
    for (uint32_t t = 200000; t && (GRSTCTL & GRSTCTL_CSRST); t--) { }
}

static void usb_init(void)
{
    RCC_AHB2ENR |= (1u << 7);                       /* OTGFSEN */

    GUSBCFG |= GUSBCFG_PHYSEL;                      /* embedded full-speed PHY */
    usb_core_reset();

    GCCFG = GCCFG_PWRDWN | GCCFG_NOVBUSSENS;        /* PHY on; ignore VBUS pin (PA9), also clears VBUSA/BSEN */

    GUSBCFG = (GUSBCFG & ~(GUSBCFG_FHMOD | GUSBCFG_TRDT_MASK))
            | GUSBCFG_FDMOD | GUSBCFG_TRDT(6);      /* force device mode; turnaround time 6 for HCLK >= 32 MHz */
    delay_ms(50);                                   /* core needs >= 25 ms to switch mode */

    PCGCCTL = 0;                                    /* un-gate PHY/HCLK */
    DCFG = (DCFG & ~3u) | 3u;                       /* full speed, embedded PHY */

    /* FIFO layout (32-bit words, 320 total): RX 64 | TX0 64 | TX1 64 */
    GRXFSIZ  = 64;
    DIEPTXF0 = (64u << 16) | 64u;
    DIEPTXF(1) = (64u << 16) | 128u;
    flush_txfifo(0x10);
    flush_rxfifo();

    DIEPMSK = DOEPMSK = DAINTMSK = 0;
    for (uint32_t i = 0; i < NUM_EPS; i++) {
        DIEPCTL(i) = (DIEPCTL(i) & EPCTL_EPENA) ? (EPCTL_EPDIS | EPCTL_SNAK) : 0;
        DIEPTSIZ(i) = 0;
        DIEPINT(i) = 0xFB7Fu;
        DOEPCTL(i) = (DOEPCTL(i) & EPCTL_EPENA) ? (EPCTL_EPDIS | EPCTL_SNAK) : 0;
        DOEPTSIZ(i) = 0;
        DOEPINT(i) = 0xFB7Fu;
    }

    GINTMSK = 0;
    GINTSTS = 0xBFFFFFFFu;
    GINTMSK = GINT_RXFLVL | GINT_USBSUSP | GINT_USBRST | GINT_ENUMDNE |
              GINT_IEPINT | GINT_OEPINT | GINT_IISOIXFR | GINT_IPXFR | GINT_WKUINT;

    NVIC_ISER2 = 1u << (OTG_FS_IRQn - 64);          /* enable IRQ 67 in the NVIC */
    GAHBCFG = GAHBCFG_GINT;                         /* global interrupt enable */

    DCTL &= ~DCTL_SDIS;                             /* clear SOFT DISCONNECT -> D+ pull-up on, host sees us */
    delay_ms(3);
}

/* ========================================================================== */
/*                         7. HID KEYBOARD (application)                      */
/* ========================================================================== */

/*
 * Poll USART2 for one newline-terminated ESP32 message. A second message
 * arriving while the first is being typed is discarded in this basic version.
 */
static void usart2_poll(void)
{
    while (USART2_SR & USART_SR_RXNE) {
        uint8_t c = (uint8_t)USART2_DR;

        if (uart_message_ready) continue;

        if (c == '\n') {
            uart_rx_buffer[uart_rx_index] = '\0';
            uart_message_ready = 1;
        } else if (c != '\r') {
            if (uart_rx_index < (UART_RX_BUFFER_SIZE - 1u)) {
                uart_rx_buffer[uart_rx_index++] = (char)c;
            } else {
                uart_rx_overflow = 1;
                uart_rx_index = 0;
            }
        }
    }

    /* Reading SR followed by DR clears an STM32F4 overrun condition. */
    if (USART2_SR & USART_SR_ORE) {
        (void)USART2_DR;
    }
}

/* Human-visible typing speed for the demonstration. */
#define KEY_HOLD_MS         60u
#define TYPE_CHAR_DELAY_MS  100u

static int hid_try_send(const uint8_t *r)
{
    int ok = 0;
    uint32_t pm = irq_save();
    if (usb_configured && !ep1_busy && !(DIEPCTL(1) & EPCTL_EPENA)) {
        ep1_busy = 1;
        DIEPTSIZ(1) = (1u << 19) | 8u;              /* 1 packet, 8 bytes */
        DIEPCTL(1) |= EPCTL_EPENA | EPCTL_CNAK;
        fifo_write(1, r, 8);
        for (int i = 0; i < 8; i++) last_report[i] = r[i];
        ok = 1;
    }
    irq_restore(pm);
    return ok;
}

static int hid_send_blocking(const uint8_t *r)
{
    uint32_t t0 = ms_ticks;
    while (usb_configured) {
        if (hid_try_send(r)) return 0;
        if ((uint32_t)(ms_ticks - t0) > 200) {
            ep1_abort();
            return -1;                            /* host not polling */
        }
    }
    return -1;
}

/* US-layout ASCII -> HID usage. Returns 1 if the character is typeable. */
static int ascii_to_hid(char c, uint8_t *mod, uint8_t *key)
{
    *mod = 0;

    if (c >= 'a' && c <= 'z') {
        *key = 0x04 + (c - 'a');
        return 1;
    }

    if (c >= 'A' && c <= 'Z') {
        *mod = 0x02;                                /* Left Shift */
        *key = 0x04 + (c - 'A');
        return 1;
    }

    if (c >= '1' && c <= '9') {
        *key = 0x1E + (c - '1');
        return 1;
    }

    if (c == '0') {
        *key = 0x27;
        return 1;
    }

    /* Shift + number-row symbols, including '#'. */
    static const char shifted_nums[] = ")!@#$%^&*(";
    for (int i = 0; i < 10; i++) {
        if (c == shifted_nums[i]) {
            *mod = 0x02;                            /* Left Shift */
            *key = (i == 0) ? 0x27 : 0x1E + (i - 1);
            return 1;
        }
    }

    switch (c) {
    case '\n': *key = 0x28; return 1;              /* Enter */
    case '\t': *key = 0x2B; return 1;              /* Tab */
    case ' ':  *key = 0x2C; return 1;              /* Space */

    case '-':  *key = 0x2D; return 1;
    case '_':  *mod = 0x02; *key = 0x2D; return 1;
    case '=':  *key = 0x2E; return 1;
    case '+':  *mod = 0x02; *key = 0x2E; return 1;
    case '[':  *key = 0x2F; return 1;
    case '{':  *mod = 0x02; *key = 0x2F; return 1;
    case ']':  *key = 0x30; return 1;
    case '}':  *mod = 0x02; *key = 0x30; return 1;
    case '\\': *key = 0x31; return 1;
    case '|':  *mod = 0x02; *key = 0x31; return 1;
    case ';':  *key = 0x33; return 1;
    case ':':  *mod = 0x02; *key = 0x33; return 1;
    case '\'': *key = 0x34; return 1;
    case '"': *mod = 0x02; *key = 0x34; return 1;
    case '`':  *key = 0x35; return 1;
    case '~':  *mod = 0x02; *key = 0x35; return 1;
    case ',':  *key = 0x36; return 1;
    case '<':  *mod = 0x02; *key = 0x36; return 1;
    case '.':  *key = 0x37; return 1;
    case '>':  *mod = 0x02; *key = 0x37; return 1;
    case '/':  *key = 0x38; return 1;
    case '?':  *mod = 0x02; *key = 0x38; return 1;

    default:
        return 0;
    }
}

/* Send one normal key press and then release it. */
static void hid_key_press(uint8_t modifier, uint8_t key)
{
    static const uint8_t release[8] = {0};

    uint8_t press[8] = {
        modifier,
        0x00,
        key,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00
    };

    if (hid_send_blocking(press) < 0)
        return;

    delay_ms(KEY_HOLD_MS);

    if (hid_send_blocking(release) < 0)
        return;

    delay_ms(TYPE_CHAR_DELAY_MS);
}

/* Send a key combination such as Ctrl+Alt+T. */
static void hid_combo(uint8_t modifier, uint8_t key)
{
    static const uint8_t release[8] = {0};

    uint8_t press[8] = {
        modifier,
        0x00,
        key,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00
    };

    if (hid_send_blocking(press) < 0)
        return;

    delay_ms(KEY_HOLD_MS);

    if (hid_send_blocking(release) < 0)
        return;

    delay_ms(TYPE_CHAR_DELAY_MS);
}

static void hid_enter(void)
{
    hid_key_press(0x00, 0x28);                     /* Enter */
}

static void hid_ctrl_alt_t(void)
{
    hid_combo(0x05, 0x17);                         /* Ctrl + Alt + T */
}

static void hid_ctrl_tab(void)
{
    hid_combo(0x01, 0x2B);                         /* Ctrl + Tab */
}

static void hid_ctrl_c(void)
{
    hid_combo(0x01, 0x06);                         /* Ctrl + C */
}

/* Type a complete string slowly enough to be visible during the demo. */
static void type_string(const char *s)
{
    static const uint8_t release[8] = {0};

    /* Ensure no key is left pressed before starting a string. */
    if (hid_send_blocking(release) < 0)
        return;

    for (; *s; s++) {
        uint8_t mod, key;

        if (!ascii_to_hid(*s, &mod, &key))
            continue;

        uint8_t press[8] = {
            mod,
            0x00,
            key,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00
        };

        if (hid_send_blocking(press) < 0)
            return;

        delay_ms(KEY_HOLD_MS);

        if (hid_send_blocking(release) < 0)
            return;

        delay_ms(TYPE_CHAR_DELAY_MS);
    }
}

/* Append a non-negative 64-bit integer as decimal text. */
static char *append_uint64(char *p, uint64_t v)
{
    char tmp[20];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);

    while (n)
        *p++ = tmp[--n];

    return p;
}

/*
 * Demonstration sequence triggered by the board's KEY button.
 * After the one-time sequence, continuously types incrementing echo commands.
 */
static void run_demo(void)
{
    uint64_t n = 1;

    /* 1. Press Enter, wait 2 seconds. */
    hid_enter();
    delay_ms(2000);

    /* 2. Type 7410 and press Enter. */
    type_string("7410");
    hid_enter();
    delay_ms(500);

    /* 3. Open terminal using Ctrl + Alt + T. */
    hid_ctrl_alt_t();

    /* 4. Wait 1 second, then type firefox and press Enter. */
    delay_ms(1000);
    type_string("firefox");
    hid_enter();

    /* 5. Wait 2 seconds, then type google.com and press Enter. */
    delay_ms(2000);
    type_string("google.com");
    hid_enter();

    /* 6. Wait 5 seconds for the page to load. */
    delay_ms(5000);

    /* 7. Type the search query. */
    type_string("what is USB HID");

    /* 8. Leave it on screen for 10 seconds. */
    delay_ms(10000);

    /* 9. Ctrl + Tab, then Ctrl + C. */
    hid_ctrl_tab();
    delay_ms(500);
    hid_ctrl_c();

    /*
     * 10. Continuously type shell commands with an incrementing counter.
     *     #1, #2, #3, ...
     */
    for (;;) {
        char cmd[100];
        char *p = cmd;
        static const char prefix[] =
            "echo Hello from STM32 Baremetal USB Keyboard #";

        for (size_t i = 0; i < sizeof(prefix) - 1u; i++)
            *p++ = prefix[i];

        p = append_uint64(p, n);
        n++;
        *p = '\0';

        type_string(cmd);
        hid_enter();

        /* Give the computer time to execute the command and display it. */
        delay_ms(1000);
    }
}

/* ========================================================================== */
/*                                   MAIN                                     */
/* ========================================================================== */
int main(void)
{
    clock_init();
    systick_init();
    gpio_init();
    usart2_init();
    usb_init();

    uint32_t last_led = 0;
    uint8_t  btn_prev = 0;
    uint8_t  led_state = 0;

    for (;;) {
        uint32_t now = ms_ticks;

        /* Always poll UART so a message can wait until USB enumerates. */
        usart2_poll();

        /* ---- LED status ---- */
        uint32_t period = usb_configured ? 500u : 100u;

        if (usb_configured && caps_lock) {
            led_set(1);
        } else if ((uint32_t)(now - last_led) >= period) {
            last_led = now;
            led_state ^= 1u;
            led_set(led_state);
        }

        if (usb_configured) {
            /* Button path has priority over the ESP32 path. */
            uint8_t btn = (uint8_t)button_pressed();

            if (btn && !btn_prev) {
                delay_ms(20);

                if (button_pressed()) {
                    /* run_demo() intentionally never returns. */
                    run_demo();
                }
            }

            btn_prev = btn;

            /* If the button is not pressed, process one ESP32 message. */
            if (!btn && uart_message_ready) {
                if (!uart_rx_overflow) {
                    type_string(uart_rx_buffer);
                    hid_enter();
                }

                uint32_t pm = irq_save();
                uart_rx_index = 0;
                uart_rx_overflow = 0;
                uart_message_ready = 0;
                irq_restore(pm);
            }
        }
    }
}
