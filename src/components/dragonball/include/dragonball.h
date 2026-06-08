#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * MC68EZ328 "DragonBall EZ" peripheral register map.
 * All offsets relative to $FFFFF000 (base of 4KB register block).
 * Register names use the Motorola convention from the EZ datasheet.
 */

/* System Control */
#define REG_SCR         0x000

/* Chip Selects */
#define REG_GRPBASEA    0x100
#define REG_GRPBASEB    0x102
#define REG_GRPBASEC    0x104
#define REG_GRPBASED    0x106
#define REG_CSA0        0x110
#define REG_CSA1        0x114
#define REG_CSB0        0x118
#define REG_CSB1        0x11C
#define REG_CSC0        0x120
#define REG_CSC1        0x124
#define REG_CSD0        0x128
#define REG_CSD1        0x12C

/* PLL and Clock */
#define REG_PLLCR       0x200
#define REG_PLLFSR      0x202
#define REG_PCTLR       0x207
#define PLLCR_DISPLL    0x0008  /* PLL/clock disabled: deep sleep, display off */

/* Interrupt Controller */
#define REG_IVR         0x300
#define REG_ICR         0x302
#define REG_IMR         0x304
#define REG_IWR         0x308
#define REG_ISR         0x30C
#define REG_IPR         0x310

/* GPIO Ports */
#define REG_PADIR       0x400
#define REG_PADATA      0x401
#define REG_PASEL       0x403
#define REG_PBDIR       0x408
#define REG_PBDATA      0x409
#define REG_PBSEL       0x40B
#define REG_PCDIR       0x410
#define REG_PCDATA      0x411
#define REG_PCSEL       0x413
#define REG_PDDIR       0x418
#define REG_PDDATA      0x419
#define REG_PDPUEN      0x41A
#define REG_PDSEL       0x41B
#define REG_PDPOL       0x41C
#define REG_PDIRQEN     0x41D
#define REG_PDIRQEDGE   0x41F
#define REG_PEDIR       0x420
#define REG_PEDATA      0x421
#define REG_PEPUEN      0x422
#define REG_PESEL       0x423
#define REG_PFDIR       0x428
#define REG_PFDATA      0x429
#define REG_PFPUEN      0x42A
#define REG_PFSEL       0x42B

/* Port D / Port F data-line meaning on the IIIx (scanned keyboard + status). */
#define PD_KBD_COLS     0x0F  /* Port D bits 0-3: scanned keyboard columns        */
#define PD_DOCK_IRQ1    0x10  /* Port D bit 4: Dock button / IRQ1 (active-low)    */
#define PD_POWER_FAIL   0x80  /* Port D bit 7: Power Fail input  (active-low)     */
#define PF_PENIRQ       0x02  /* Port F bit 1: PENIRQ from the ADS7846 digitizer  */
#define PF_KBD_ROW0     0x10  /* Port F bit 4: keyboard Row0 select (Row1=<<1, …) */

#define REG_PGDIR       0x430
#define REG_PGDATA      0x431
#define REG_PGPUEN      0x432
#define REG_PGSEL       0x433
#define REG_PJDIR       0x438
#define REG_PJDATA      0x439
#define REG_PJSEL       0x43B
#define REG_PKDIR       0x440
#define REG_PKDATA      0x441
#define REG_PKPUEN      0x442
#define REG_PKSEL       0x443

/* PWM */
#define REG_PWMC        0x500
#define REG_PWMP        0x502
#define REG_PWMW        0x504

/* Timer 1 */
#define REG_TCTL        0x600
#define REG_TPRER       0x602
#define REG_TCMP        0x604
#define REG_TCR         0x606
#define REG_TCN         0x608
#define REG_TSTAT       0x60A

/* TCTL (control) */
#define TCTL_TEN        0x0001  /* timer enable                              */
#define TCTL_CLKSRC_SHIFT 1
#define TCTL_CLKSRC_MASK  0x0007  /* clock-source field, after >> SHIFT       */
#define TCTL_CLK_SYSCLK 1       /* SYSCLK direct (÷1)                        */
#define TCTL_CLK_SYSCLK16 2     /* SYSCLK ÷16 (the ROM's 100 Hz tick source) */
#define TCTL_IRQEN      0x0010  /* compare-match interrupt enable            */
/* TSTAT (status) */
#define TSTAT_COMP      0x0001  /* compare-match occurred                    */

/* Watchdog */
#define REG_WCTLR       0x618
#define REG_WCMPR       0x61A
#define REG_WCNTR       0x61C

/* SPI Master */
#define REG_SPIMDATA    0x800
#define REG_SPIMCONT    0x802

/* SPIMCONT ($802) bits (MC68EZ328). The master interrupt request (IPR[SPIM]) is
 * a LEVEL: it is asserted only while SPMIRQ (transfer complete) AND IRQEN (the
 * master's own interrupt enable) are both set — so clearing IRQEN de-asserts it. */
#define SPIM_ENABLE     0x0200   /* SPI master enable                 */
#define SPIM_XCH        0x0100   /* Exchange: start a transfer        */
#define SPIM_SPMIRQ     0x0080   /* Transfer-complete status (polled) */
#define SPIM_IRQEN      0x0040   /* Interrupt enable                  */

/* UART */
#define REG_USTCNT      0x900
#define REG_UBAUD       0x902
#define REG_URX         0x904
#define REG_UTX         0x906
#define REG_UMISC       0x908
#define REG_UNIPR       0x90A

/* MC68EZ328 UART register bits (Motorola M68EZ328 User's Manual). */
/* USTCNT (control) */
#define USTCNT_UARTEN   0x8000  /* UART enable          */
#define USTCNT_RXEN     0x4000  /* receiver enable      */
#define USTCNT_TXEN     0x2000  /* transmitter enable   */
#define USTCNT_PARITYEN 0x0800
#define USTCNT_PARITYODD 0x0400
#define USTCNT_STOP2    0x0200
#define USTCNT_DATA8    0x0100
#define USTCNT_RXFULLEN 0x0020  /* RX-FIFO-full   int enable */
#define USTCNT_RXHALFEN 0x0010  /* RX-FIFO-half   int enable */
#define USTCNT_RXRDYEN  0x0008  /* RX-data-ready  int enable */
#define USTCNT_TXEMPTYEN 0x0004 /* TX-FIFO-empty  int enable */
#define USTCNT_TXHALFEN 0x0002  /* TX-FIFO-half   int enable */
#define USTCNT_TXAVAILEN 0x0001 /* TX-FIFO-avail  int enable */
/* URX (receiver) */
#define URX_FIFOFULL    0x8000
#define URX_FIFOHALF    0x4000
#define URX_DATARDY     0x2000
#define URX_OVRUN       0x0800
#define URX_DATA        0x00FF
/* UTX (transmitter) */
#define UTX_FIFOEMPTY   0x8000
#define UTX_FIFOHALF    0x4000
#define UTX_AVAIL       0x2000
#define UTX_IGNORECTS   0x0800
#define UTX_CTSSTATUS   0x0200
#define UTX_DATA        0x00FF
/* UBAUD */
#define UBAUD_DIVIDER   0x0700  /* bits 10:8 */
#define UBAUD_PRESCALER 0x003F
/* UMISC (miscellaneous) — bit layout per the M68328/EZ328 register map. Only the
 * handful the firmware cares about are named; the rest are stored verbatim. */
#define UMISC_LOOP      0x1000  /* internal TX->RX loopback (self-test)        */
#define UMISC_RTS_CONT  0x0080  /* RTS driven by RX-FIFO state (vs the RTS bit) */
#define UMISC_RTS       0x0040  /* manual RTS output level                      */
#define UMISC_IRDA_EN   0x0020  /* IrDA mode — UART repurposed for IR beaming   */

/* LCD Controller */
#define REG_LSSA        0xA00  /* 32-bit: screen starting address */
#define REG_LVPW        0xA05  /* 8-bit: virtual page width */
#define REG_LXMAX       0xA08  /* 16-bit: screen width */
#define REG_LYMAX       0xA0A  /* 16-bit: screen height */
#define REG_LCXP        0xA18  /* 16-bit: cursor X position */
#define REG_LCYP        0xA1A  /* 16-bit: cursor Y position */
#define REG_LCWCH       0xA1C  /* 16-bit: cursor width/height */
#define REG_LBLKC       0xA1F  /* 8-bit: blink control */
#define REG_LPICF       0xA20  /* 8-bit: panel interface config */
#define REG_LPOLCF      0xA21  /* 8-bit: polarity config */
#define REG_LACDRC      0xA23  /* 8-bit: ACD rate control */
#define REG_LPXCD       0xA25  /* 8-bit: pixel clock divider */
#define REG_LCKCON      0xA27  /* 8-bit: clock control */
#define REG_LRRA        0xA29  /* 8-bit: refresh rate adjustment */
#define REG_LGPMR       0xA2A  /* 16-bit: grayscale palette mapping */
#define REG_LDMR        0xA31  /* 8-bit: DMA control */

/* RTC */
#define REG_HMSR        0xB00  /* 32-bit: hours/min/sec */
#define REG_ALARM       0xB04  /* 32-bit: alarm */
#define REG_RTCCTL      0xB0C  /* 16-bit: control */
#define REG_RTCISR      0xB0E  /* 16-bit: interrupt status */
#define REG_RTCIENR     0xB10  /* 16-bit: interrupt enable */
#define REG_STPWCH      0xB14  /* 16-bit: stopwatch */
#define REG_DAYR        0xB1A  /* 16-bit: day counter */

/* DRAM Controller */
#define REG_DRAMMC      0xC00
#define REG_DRAMC       0xC02

/* Interrupt source bits (for IMR/ISR/IPR) */
#define INT_SPIM       0
#define INT_TMR        1
#define INT_UART       2
#define INT_WDT        3
#define INT_RTC        4
#define INT_KB         6
#define INT_PWM        7
#define INT_INT0       8
#define INT_INT1       9
#define INT_INT2       10
#define INT_INT3       11
#define INT_IRQ1       16
#define INT_IRQ2       17
#define INT_IRQ3       18
#define INT_IRQ6       19
#define INT_PEN        20

/* Peripheral state */
typedef struct {
    /* Interrupt controller */
    uint8_t  ivr;
    uint16_t icr;
    uint32_t imr;
    uint32_t iwr;
    uint32_t isr;
    uint32_t ipr;

    /* PLL */
    uint16_t pllcr;
    uint16_t pllfsr;
    uint8_t  pctlr;

    /* Chip selects: CSA-CSD at offsets 0x110-0x11E (each 32-bit = two 16-bit halves) */
    uint16_t grpbase[4];   /* 0x100-0x106 */
    uint16_t csctrl;       /* 0x108 (reserved/control) */
    uint16_t cs[8];        /* 0x110-0x11E: CSA0,CSA1,CSB0,CSB1,CSC0,CSC1,CSD0,CSD1 */

    /* GPIO: ports A-G (0-6), J (7), K (8) */
    uint8_t port_dir[9];
    uint8_t port_data[9];
    uint8_t port_sel[9];
    uint8_t port_puen[9];
    uint8_t pd_pol;
    uint8_t pd_irqen;
    uint8_t pd_irqedge;

    /* Scanned keyboard matrix: bit (row*4+col) set = key held. Columns are
     * read on Port D bits 0-3; rows are selected via Port F bits 4-6. */
    uint16_t kbd_keys;

    /* Watchdog */
    uint16_t wctlr;
    uint16_t wcmpr;
    uint16_t wcntr;

    /* Timer */
    uint16_t tctl;
    uint16_t tprer;
    uint16_t tcmp;
    uint16_t tcr;
    uint16_t tcn;
    uint16_t tstat;

    /* SPI */
    uint16_t spim_data;
    uint16_t spim_cont;

    /* UART */
    uint16_t ustcnt;
    uint16_t ubaud;
    uint16_t urx;
    uint16_t utx;
    uint16_t umisc;

    /* LCD */
    uint32_t lssa;
    uint8_t  lvpw;
    uint16_t lxmax;
    uint16_t lymax;
    uint16_t lcxp;
    uint16_t lcyp;
    uint16_t lcwch;
    uint8_t  lblkc;
    uint8_t  lpicf;
    uint8_t  lpolcf;
    uint8_t  lacdrc;
    uint8_t  lpxcd;
    uint8_t  lckcon;
    uint8_t  lrra;
    uint16_t lgpmr;
    uint8_t  ldmr;

    /* RTC */
    uint32_t hmsr;
    uint32_t alarm;
    uint16_t rtcctl;
    uint16_t rtcisr;
    uint16_t rtcienr;

    /* SCR */
    uint8_t scr;

    /* Cycle counter for timer */
    uint32_t timer_cycles;
    uint32_t timer_pending;
    uint16_t tstat_latched; /* bits read from TSTAT (for read-then-clear) */
} dragonball_state_t;

extern dragonball_state_t db;

void dragonball_init(void);

uint8_t  dragonball_read8(uint16_t offset);
uint16_t dragonball_read16(uint16_t offset);
uint32_t dragonball_read32(uint16_t offset);

void dragonball_write8(uint16_t offset, uint8_t val);
void dragonball_write16(uint16_t offset, uint16_t val);
void dragonball_write32(uint16_t offset, uint32_t val);

/* Sub-module interfaces */
void     db_interrupt_raise(int source);
void     db_interrupt_clear(int source);
int      db_interrupt_pending_level(void);

uint32_t db_lcd_framebuffer_addr(void);

void     db_timer_advance(uint32_t cpu_cycles);

/* Scanned keyboard matrix (Palm IIIx "Sumo"): Port D cols 0-3 x Port F rows
 * 0-2. Row0 = app keys; Row1 = PgUp(c0)/PgDn(c1); Row2 = Power(c0)/Contrast(c1).
 * Pressing the Power key is the ONLY way to wake the device from sleep. */
#define KBD_ROW_APPS   0
#define KBD_ROW_PAGE   1
#define KBD_ROW_POWER  2
#define KBD_KEY_PGUP   KBD_ROW_PAGE, 0
#define KBD_KEY_PGDN   KBD_ROW_PAGE, 1
#define KBD_KEY_POWER  KBD_ROW_POWER, 0
void     db_kbd_set(int row, int col, bool pressed);

/* True when the CPU is in PLL-off sleep (display should be blanked). */
bool     db_is_asleep(void);
/* Mimic the EZ328 PLL auto-restart on STOP wake (clears DISPLL). Wire to
 * m68k cpu.resume_callback. */
void     db_cpu_resume(void);

/* UART (MC68EZ328) — register access (called by the bus) plus the byte-pipe API
 * used by the firmware transport / debug console. RX bytes are pushed by a
 * producer task; the CPU task drains URX and raises INT_UART. */
uint16_t db_uart_read16(uint16_t offset);
void     db_uart_write16(uint16_t offset, uint16_t val);
void     db_uart_rx_push(const uint8_t *data, uint32_t len);  /* producer -> RX ring */
uint32_t db_uart_tx_pop(uint8_t *out, uint32_t max);          /* TX ring -> consumer */
uint32_t db_uart_rx_free(void);      /* free space in the RX ring (for back-pressure) */
bool     db_uart_rx_ring_nonempty(void);
void     db_uart_update_irq(void);   /* (re)evaluate INT_UART; CPU task only */
void     db_uart_set_loopback(bool on);

void     db_spi_set_touch(int16_t x, int16_t y, bool down);
bool     db_spi_pen_down(void);
uint32_t db_spi_xch_count(void);   /* cumulative ADS7846 conversions */
/* Set the 12-bit ADS7846 battery sample (channel 2). The firmware maps the real
 * M5Paper battery voltage to this; the ROM reads level = sample>>4. */
void     db_spi_set_battery_sample(uint16_t sample);
bool     db_iwr_wake_pending(void);
uint32_t db_int_vector_base(void);   /* (IVR & 0xF8): DragonBall vectored-IRQ base */
