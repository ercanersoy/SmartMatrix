/*
 * SmartMatrix Library - Refresh Code for Teensy 3.x Platform
 *
 * Copyright (c) 2014 Louis Beaudoin (Pixelmatix)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <math.h>
#include <algorithm>
#include "arm_math.h"

#include "SmartMatrix.h"
#include "CircularBuffer.h"

// these definitions may change if switching major display type
#define MATRIX_ROW_PAIR_OFFSET      (MATRIX_HEIGHT/2)
#define MATRIX_ROWS_PER_FRAME       (MATRIX_HEIGHT/2)
#define PIXELS_UPDATED_PER_CLOCK    2
#define COLOR_CHANNELS_PER_PIXEL    3
#define LATCHES_PER_ROW             (COLOR_DEPTH_RGB/COLOR_CHANNELS_PER_PIXEL)
#define DMA_UPDATES_PER_CLOCK       2

// hardware-specific definitions
// prescale of 0 is F_BUS
#define LATCH_TIMER_PRESCALE  0x00
#define NS_TO_TICKS(X)      (uint32_t)(F_BUS * ((X) / 1000000000.0))
#define LATCH_TIMER_PULSE_WIDTH_TICKS   NS_TO_TICKS(LATCH_TIMER_PULSE_WIDTH_NS)
#define TICKS_PER_ROW   (F_BUS/MATRIX_REFRESH_RATE/MATRIX_ROWS_PER_FRAME)
#define MSB_BLOCK_TICKS     (TICKS_PER_ROW/2)
#define MIN_BLOCK_PERIOD_TICKS  NS_TO_TICKS(MIN_BLOCK_PERIOD_NS)


typedef struct timerpair {
    uint16_t timer_oe;
    uint16_t timer_period;
} timerpair;

typedef struct addresspair {
    uint16_t bits_to_clear;
    uint16_t bits_to_set;
} addresspair;

typedef struct matrixUpdateBlock {
    timerpair timerValues;
    addresspair addressValues;
} matrixUpdateBlock;

static CircularBuffer dmaBuffer;
static DMAMEM matrixUpdateBlock matrixUpdateBlocks[DMA_BUFFER_NUMBER_OF_ROWS][LATCHES_PER_ROW];

/*
  buffer contains:
    COLOR_DEPTH/sizeof(int32_t) * 2 words for each pair of pixels (pixel data from n, and n+MATRIX_ROW_PAIR_OFFSET)
      first half of the words contain a byte for each shade, going from LSB to MSB
      second half of the words have the same data, plus a high bit in each byte for the clock
    there are MATRIX_WIDTH number of these in order to refresh a row (pair of rows)
 */
static DMAMEM uint32_t matrixUpdateData[DMA_BUFFER_NUMBER_OF_ROWS][MATRIX_WIDTH][(LATCHES_PER_ROW / sizeof(uint32_t)) * DMA_UPDATES_PER_CLOCK];

addresspair addressLUT[MATRIX_ROWS_PER_FRAME];

timerpair timerLUT[LATCHES_PER_ROW];

// 2x uint32_t to match size and spacing of values it is updating: GPIOx_PSOR and GPIOx_PCOR are 32-bit and adjacent to each other
typedef struct gpiopair {
    uint32_t  gpio_psor;
    uint32_t  gpio_pcor;
} gpiopair;

gpiopair gpiosync;


SmartMatrix::SmartMatrix() {

}

void SmartMatrix::matrixCalculations(void) {
    static unsigned char currentRow = 0;

    // only run the loop if there is free space, and fill the entire buffer before returning
    while (!cbIsFull(&dmaBuffer)) {
        // do once-per-frame updates
        if (!currentRow) {
            handleBufferSwap();

#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_3, HIGH); // oscilloscope trigger
#endif
            updateForeground();
#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_3, LOW);
#endif

            if (brightnessChange) {
                calculateTimerLut();
                brightnessChange = false;
            }
        }

        // do once-per-line updates
        // none right now

        // enqueue row
        if (++currentRow >= MATRIX_ROWS_PER_FRAME)
            currentRow = 0;

        SmartMatrix::loadMatrixBuffers(currentRow);
        cbWrite(&dmaBuffer);
    }
}

void SmartMatrix::calculateTimerLut(void) {
    int i;

    for (i = 0; i < LATCHES_PER_ROW; i++) {
        // set period and OE values for current block - going from smallest timer values to largest
        // order needs to be smallest to largest so the last update of the row has the largest time between
        // the falling edge of the latch and the rising edge of the latch on the next row - an ISR
        // updates the row in this time

        // period is max on time for this block, plus the dead time while the latch is high
        uint16_t period = (MSB_BLOCK_TICKS >> (LATCHES_PER_ROW - i - 1)) + LATCH_TIMER_PULSE_WIDTH_TICKS;
        // on-time is the max on-time * dimming factor, plus the dead time while the latch is high
        uint16_t ontime = (((MSB_BLOCK_TICKS >> (LATCHES_PER_ROW - i - 1)) * dimmingFactor) / dimmingMaximum) + LATCH_TIMER_PULSE_WIDTH_TICKS;

        if (period < MIN_BLOCK_PERIOD_TICKS) {
            uint16_t padding = MIN_BLOCK_PERIOD_TICKS - period;
            period += padding;
            ontime += padding;
        }

        timerLUT[i].timer_period = period;
        timerLUT[i].timer_oe = ontime;
    }
}

void SmartMatrix::begin()
{
    int i;
    cbInit(&dmaBuffer, DMA_BUFFER_NUMBER_OF_ROWS);

    // fill addressLUT
    for (i = 0; i < MATRIX_ROWS_PER_FRAME; i++) {

        // set all bits that are 1 in address
        addressLUT[i].bits_to_set = 0x00;
        if (i & 0x01)
            addressLUT[i].bits_to_set |= (1 << ADDX_PIN_0);
        if (i & 0x02)
            addressLUT[i].bits_to_set |= (1 << ADDX_PIN_1);
        if (i & 0x04)
            addressLUT[i].bits_to_set |= (1 << ADDX_PIN_2);
#ifdef ADDX_PIN_3
        if (i & 0x08)
            addressLUT[i].bits_to_set |= (1 << ADDX_PIN_3);
#endif

        // set all bits that are clear in address
        addressLUT[i].bits_to_clear = (~addressLUT[i].bits_to_set) & ADDX_PIN_MASK;
    }

    // fill timerLUT
    calculateTimerLut();

    // fill buffer with data before enabling DMA
    matrixCalculations();

    // setup debug output
#ifdef DEBUG_PINS_ENABLED
    pinMode(DEBUG_PIN_1, OUTPUT);
    digitalWriteFast(DEBUG_PIN_1, HIGH); // oscilloscope trigger
    digitalWriteFast(DEBUG_PIN_1, LOW);
    pinMode(DEBUG_PIN_2, OUTPUT);
    digitalWriteFast(DEBUG_PIN_2, HIGH); // oscilloscope trigger
    digitalWriteFast(DEBUG_PIN_2, LOW);
    pinMode(DEBUG_PIN_3, OUTPUT);
    digitalWriteFast(DEBUG_PIN_3, HIGH); // oscilloscope trigger
    digitalWriteFast(DEBUG_PIN_3, LOW);
#endif

    // configure the 7 output pins (one pin is left as input, though it can't be used as GPIO output)
    pinMode(GPIO_PIN_CLK_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_B0_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_R0_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_R1_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_G0_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_G1_TEENSY_PIN, OUTPUT);
    pinMode(GPIO_PIN_B1_TEENSY_PIN, OUTPUT);

    // configure the address pins
    pinMode(ADDX_TEENSY_PIN_0, OUTPUT);
    pinMode(ADDX_TEENSY_PIN_1, OUTPUT);
    pinMode(ADDX_TEENSY_PIN_2, OUTPUT);
#ifdef ADDX_TEENSY_PIN_3
    pinMode(ADDX_TEENSY_PIN_3, OUTPUT);
#endif

    // setup FTM1
    FTM1_SC = 0;
    FTM1_CNT = 0;
    FTM1_MOD = MSB_BLOCK_TICKS;

    // setup FTM1 compares:
    // latch pulse width set based on max time to update address pins
    FTM1_C0V = LATCH_TIMER_PULSE_WIDTH_TICKS;
    // output OE signal - set to max at first to disable OE
    FTM1_C1V = MSB_BLOCK_TICKS;

    // setup PWM outputs
    ENABLE_LATCH_PWM_OUTPUT();
    ENABLE_OE_PWM_OUTPUT();

    // setup GPIO interrupts
    ENABLE_LATCH_RISING_EDGE_GPIO_INT();
    ENABLE_LATCH_FALLING_EDGE_GPIO_INT();


    // enable clocks to the DMA controller and DMAMUX
    SIM_SCGC7 |= SIM_SCGC7_DMA;
    SIM_SCGC6 |= SIM_SCGC6_DMAMUX;

    // enable minor loop mapping so addresses can get reset after minor loops
    DMA_CR = 1 << 7;

    // disable DMA
    DMA_ERQ = 0;

    // reset state of DMAMUX
    DMAMUX0_CHCFG0 = 0;
    DMAMUX0_CHCFG1 = 0;
    DMAMUX0_CHCFG2 = 0;
    DMAMUX0_CHCFG3 = 0;

    // DMA channel #0 - on latch rising edge, read address from fixed address temporary buffer, and output address on GPIO
    // using combo of writes to set+clear registers, to only modify the address pins and not other GPIO pins
    // address temporary buffer is refreshed before each DMA trigger (by DMA channel #2)
    // only use single major loop, never disable channel
#define ADDRESS_ARRAY_REGISTERS_TO_UPDATE   2
    DMA_TCD0_SADDR = &gpiosync.gpio_pcor;
    DMA_TCD0_SOFF = (int)&gpiosync.gpio_psor - (int)&gpiosync.gpio_pcor;
    DMA_TCD0_SLAST = (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * ((int)&ADDX_GPIO_CLEAR_REGISTER - (int)&ADDX_GPIO_SET_REGISTER));
    DMA_TCD0_ATTR = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
    // Destination Minor Loop Offset Enabled - transfer appropriate number of bytes per minor loop, and put DADDR back to original value when minor loop is complete
    // Source Minor Loop Offset Enabled - source buffer is same size and offset as destination so values reset after each minor loop
    DMA_TCD0_NBYTES_MLOFFYES = DMA_TCD_NBYTES_SMLOE | DMA_TCD_NBYTES_DMLOE |
                               ((ADDRESS_ARRAY_REGISTERS_TO_UPDATE * ((int)&ADDX_GPIO_CLEAR_REGISTER - (int)&ADDX_GPIO_SET_REGISTER)) << 10) |
                               (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * sizeof(gpiosync.gpio_psor));
    // start on higher value of two registers, and make offset decrement to avoid negative number in NBYTES_MLOFFYES (TODO: can switch order by masking negative offset)
    DMA_TCD0_DADDR = &ADDX_GPIO_CLEAR_REGISTER;
    // update destination address so the second update per minor loop is ADDX_GPIO_SET_REGISTER
    DMA_TCD0_DOFF = (int)&ADDX_GPIO_SET_REGISTER - (int)&ADDX_GPIO_CLEAR_REGISTER;
    DMA_TCD0_DLASTSGA = (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * ((int)&ADDX_GPIO_CLEAR_REGISTER - (int)&ADDX_GPIO_SET_REGISTER));
    // single major loop
    DMA_TCD0_CITER_ELINKNO = 1;
    DMA_TCD0_BITER_ELINKNO = 1;
    // link channel 1, enable major channel-to-channel linking, don't clear enable on major loop complete
    DMA_TCD0_CSR = (1 << 8) | (1 << 5);
    DMAMUX0_CHCFG0 = DMAMUX_SOURCE_LATCH_RISING_EDGE | DMAMUX_ENABLE;

    // DMA channel #1 - copy address values from current position in array to buffer to temporarily hold row values for the next timer cycle
    // only use single major loop, never disable channel
    DMA_TCD1_SADDR = &matrixUpdateBlocks[0][0].addressValues;
    DMA_TCD1_SOFF = sizeof(uint16_t);
    DMA_TCD1_SLAST = sizeof(matrixUpdateBlock) - (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * sizeof(uint16_t));
    DMA_TCD1_ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1);
    // 16-bit = 2 bytes transferred
    // transfer two 16-bit values, reset destination address back after each minor loop
    DMA_TCD1_NBYTES_MLOFFNO = (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * sizeof(uint16_t));
    // start with the register that's the highest location in memory and make offset decrement to avoid negative number in NBYTES_MLOFFYES register (TODO: can switch order by masking negative offset)
    DMA_TCD1_DADDR = &gpiosync.gpio_pcor;
    DMA_TCD1_DOFF = (int)&gpiosync.gpio_psor - (int)&gpiosync.gpio_pcor;
    DMA_TCD1_DLASTSGA = (ADDRESS_ARRAY_REGISTERS_TO_UPDATE * ((int)&gpiosync.gpio_pcor - (int)&gpiosync.gpio_psor));
    // no minor loop linking, single major loop, single minor loop, don't clear enable after major loop complete
    DMA_TCD1_CITER_ELINKNO = 1;
    DMA_TCD1_BITER_ELINKNO = 1;
    DMA_TCD1_CSR = 0;

    // DMA channel #2 - on latch falling edge, load FTM1_CV1 and FTM1_MOD with with next values from current block
    // only use single major loop, never disable channel
    // link to channel 3 when complete
#define TIMER_REGISTERS_TO_UPDATE   2
    DMA_TCD2_SADDR = &matrixUpdateBlocks[0][0].timerValues.timer_oe;
    DMA_TCD2_SOFF = sizeof(uint16_t);
    DMA_TCD2_SLAST = sizeof(matrixUpdateBlock) - (TIMER_REGISTERS_TO_UPDATE * sizeof(uint16_t));
    DMA_TCD2_ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1);
    // 16-bit = 2 bytes transferred
    DMA_TCD2_NBYTES_MLOFFNO = TIMER_REGISTERS_TO_UPDATE * sizeof(uint16_t);
    DMA_TCD2_DADDR = &FTM1_C1V;
    DMA_TCD2_DOFF = (int)&FTM1_MOD - (int)&FTM1_C1V;
    DMA_TCD2_DLASTSGA = TIMER_REGISTERS_TO_UPDATE * ((int)&FTM1_C1V - (int)&FTM1_MOD);
    // no minor loop linking, single major loop
    DMA_TCD2_CITER_ELINKNO = 1;
    DMA_TCD2_BITER_ELINKNO = 1;
    // link channel 3, enable major channel-to-channel linking, don't clear enable after major loop complete
    DMA_TCD2_CSR = (3 << 8) | (1 << 5);
    DMAMUX0_CHCFG2 = DMAMUX_SOURCE_LATCH_FALLING_EDGE | DMAMUX_ENABLE;

#define DMA_TCD_MLOFF_MASK  (0x3FFFFC00)

    // DMA channel #3 - repeatedly load gpio_array into GPIOD_PDOR, stop and int on major loop complete
    DMA_TCD3_SADDR = matrixUpdateData[0][0];
    DMA_TCD3_SOFF = sizeof(matrixUpdateData[0][0]) / 2;
    // SADDR will get updated by ISR, no need to set SLAST
    DMA_TCD3_SLAST = 0;
    DMA_TCD3_ATTR = DMA_TCD_ATTR_SSIZE(0) | DMA_TCD_ATTR_DSIZE(0);
    // after each minor loop, set source to point back to the beginning of this set of data,
    // but advance by 1 byte to get the next significant bits data
    DMA_TCD3_NBYTES_MLOFFYES = DMA_TCD_NBYTES_SMLOE |
                               (((1 - sizeof(matrixUpdateData[0])) << 10) & DMA_TCD_MLOFF_MASK) |
                               (MATRIX_WIDTH * DMA_UPDATES_PER_CLOCK);
    DMA_TCD3_DADDR = &GPIOD_PDOR;
    DMA_TCD3_DOFF = 0;
    DMA_TCD3_DLASTSGA = 0;
    DMA_TCD3_CITER_ELINKNO = LATCHES_PER_ROW;
    DMA_TCD3_BITER_ELINKNO = LATCHES_PER_ROW;
    // int after major loop is complete
    DMA_TCD3_CSR = DMA_TCD_CSR_INTMAJOR;
    // for debugging - enable bandwidth control (space out GPIO updates so they can be seen easier on a low-bandwidth logic analyzer)
    //DMA_TCD3_CSR |= (0x02 << 14);

    // enable a done interrupt when all DMA operations are complete
    NVIC_ENABLE_IRQ(IRQ_DMA_CH3);

    // enable additional dma interrupt used as software interrupt
    NVIC_SET_PRIORITY(IRQ_DMA_CH1, 0xFF); // 0xFF = lowest priority
    NVIC_ENABLE_IRQ(IRQ_DMA_CH1);

    // enable channels 0, 1, 2, 3
    DMA_ERQ = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3);

    // at the end after everything is set up: enable timer from system clock, with appropriate prescale
    FTM1_SC = FTM_SC_CLKS(1) | FTM_SC_PS(LATCH_TIMER_PRESCALE);
}

static const uint16_t lightPowerMap16bit2[] = {
    0x00, 0x00, 0x00, 0x01, 0x02, 0x04, 0x06, 0x08,
    0x0b, 0x0f, 0x14, 0x19, 0x1f, 0x26, 0x2e, 0x37,
    0x41, 0x4b, 0x57, 0x63, 0x71, 0x80, 0x8f, 0xa0,
    0xb2, 0xc5, 0xda, 0xef, 0x106, 0x11e, 0x137, 0x152,
    0x16e, 0x18b, 0x1a9, 0x1c9, 0x1eb, 0x20e, 0x232, 0x257,
    0x27f, 0x2a7, 0x2d2, 0x2fd, 0x32b, 0x359, 0x38a, 0x3bc,
    0x3ef, 0x425, 0x45c, 0x494, 0x4cf, 0x50b, 0x548, 0x588,
    0x5c9, 0x60c, 0x651, 0x698, 0x6e0, 0x72a, 0x776, 0x7c4,
    0x814, 0x866, 0x8b9, 0x90f, 0x967, 0x9c0, 0xa1b, 0xa79,
    0xad8, 0xb3a, 0xb9d, 0xc03, 0xc6a, 0xcd4, 0xd3f, 0xdad,
    0xe1d, 0xe8f, 0xf03, 0xf79, 0xff2, 0x106c, 0x10e9, 0x1168,
    0x11e9, 0x126c, 0x12f2, 0x137a, 0x1404, 0x1490, 0x151f, 0x15b0,
    0x1643, 0x16d9, 0x1771, 0x180b, 0x18a7, 0x1946, 0x19e8, 0x1a8b,
    0x1b32, 0x1bda, 0x1c85, 0x1d33, 0x1de2, 0x1e95, 0x1f49, 0x2001,
    0x20bb, 0x2177, 0x2236, 0x22f7, 0x23bb, 0x2481, 0x254a, 0x2616,
    0x26e4, 0x27b5, 0x2888, 0x295e, 0x2a36, 0x2b11, 0x2bef, 0x2cd0,
    0x2db3, 0x2e99, 0x2f81, 0x306d, 0x315a, 0x324b, 0x333f, 0x3435,
    0x352e, 0x3629, 0x3728, 0x3829, 0x392d, 0x3a33, 0x3b3d, 0x3c49,
    0x3d59, 0x3e6b, 0x3f80, 0x4097, 0x41b2, 0x42d0, 0x43f0, 0x4513,
    0x463a, 0x4763, 0x488f, 0x49be, 0x4af0, 0x4c25, 0x4d5d, 0x4e97,
    0x4fd5, 0x5116, 0x525a, 0x53a1, 0x54eb, 0x5638, 0x5787, 0x58da,
    0x5a31, 0x5b8a, 0x5ce6, 0x5e45, 0x5fa7, 0x610d, 0x6276, 0x63e1,
    0x6550, 0x66c2, 0x6837, 0x69af, 0x6b2b, 0x6caa, 0x6e2b, 0x6fb0,
    0x7139, 0x72c4, 0x7453, 0x75e5, 0x777a, 0x7912, 0x7aae, 0x7c4c,
    0x7def, 0x7f94, 0x813d, 0x82e9, 0x8498, 0x864b, 0x8801, 0x89ba,
    0x8b76, 0x8d36, 0x8efa, 0x90c0, 0x928a, 0x9458, 0x9629, 0x97fd,
    0x99d4, 0x9bb0, 0x9d8e, 0x9f70, 0xa155, 0xa33e, 0xa52a, 0xa71a,
    0xa90d, 0xab04, 0xacfe, 0xaefb, 0xb0fc, 0xb301, 0xb509, 0xb715,
    0xb924, 0xbb37, 0xbd4d, 0xbf67, 0xc184, 0xc3a5, 0xc5ca, 0xc7f2,
    0xca1e, 0xcc4d, 0xce80, 0xd0b7, 0xd2f1, 0xd52f, 0xd771, 0xd9b6,
    0xdbfe, 0xde4b, 0xe09b, 0xe2ef, 0xe547, 0xe7a2, 0xea01, 0xec63,
    0xeeca, 0xf134, 0xf3a2, 0xf613, 0xf888, 0xfb02, 0xfd7e, 0xffff,
    0xffff
};

//ALWAYS_INLINE
static inline
uint32_t lutInterpolate(const uint16_t *lut, uint32_t arg)
{
    /*
     * Using our color LUT for the indicated channel, convert the
     * 16-bit intensity "arg" in our input colorspace to a corresponding
     * 16-bit intensity in the device colorspace.
     *
     * Remember that our LUT is 257 entries long. The final entry corresponds to an
     * input of 0x10000, which can't quite be reached.
     *
     * 'arg' is in the range [0, 0xFFFF]
     *
     * This operation is equivalent to the following:
     *
     *      unsigned index = arg >> 8;          // Range [0, 0xFF]
     *      unsigned alpha = arg & 0xFF;        // Range [0, 0xFF]
     *      unsigned invAlpha = 0x100 - alpha;  // Range [1, 0x100]
     *
     *      // Result in range [0, 0xFFFF]
     *      return (lut[index] * invAlpha + lut[index + 1] * alpha) >> 8;
     *
     * This is easy to understand, but it turns out to be a serious bottleneck
     * in terms of speed and memory bandwidth, as well as register pressure that
     * affects the compilation of updatePixel().
     *
     * To speed this up, we try and do the lut[index] and lut[index+1] portions
     * in parallel using the SMUAD instruction. This is a pair of 16x16 multiplies,
     * and the results are added together. We can combine this with an unaligned load
     * to grab two adjacent entries from the LUT. The remaining complications are:
     *
     *   1. We wanted unsigned, not signed
     *   2. We still need to generate the input values efficiently.
     *
     * (1) is easy to solve if we're okay with 15-bit precision for the LUT instead
     * of 16-bit, which is fine. During LUT preparation, we right-shift each entry
     * by 1, keeping them within the positive range of a signed 16-bit int.
     *
     * For (2), we need to quickly put 'alpha' in the high halfword and invAlpha in
     * the low halfword, or vice versa. One fast way to do this is (0x01000000 + x - (x << 16).
     */

    uint32_t index = arg >> 8;          // Range [0, 0xFF]

    // Load lut[index] into low halfword, lut[index+1] into high halfword.
    uint32_t pair = *(const uint32_t*)(lut + index);

    unsigned alpha = arg & 0xFF;        // Range [0, 0xFF]

    // Reversed halfword order
    uint32_t pairAlpha = (0x01000000 + alpha - (alpha << 16));

    return __SMUADX(pairAlpha, pair) >> 7;
}

extern bool hasForeground;
void SmartMatrix::loadMatrixBuffers(unsigned char currentRow) {

    int i, j;

    addresspair rowAddressPair;

    rowAddressPair.bits_to_set = addressLUT[currentRow].bits_to_set;
    rowAddressPair.bits_to_clear = addressLUT[currentRow].bits_to_clear;

    unsigned char freeRowBuffer = cbGetNextWrite(&dmaBuffer);

    // for each color bit, fill buffer with pixel data for all columns in current rows
    for (j = 0; j < LATCHES_PER_ROW; j++) {
        // copy bits to set and clear to generate address for current block
        matrixUpdateBlocks[freeRowBuffer][j].addressValues.bits_to_clear = rowAddressPair.bits_to_clear;
        matrixUpdateBlocks[freeRowBuffer][j].addressValues.bits_to_set = rowAddressPair.bits_to_set;

        matrixUpdateBlocks[freeRowBuffer][j].timerValues.timer_period = timerLUT[j].timer_period;
        matrixUpdateBlocks[freeRowBuffer][j].timerValues.timer_oe = timerLUT[j].timer_oe;
    }

    rgb24 tempPixel0;
    rgb24 tempPixel1;

    bool bHasForeground = hasForeground;
    bool bHasCC = SmartMatrix::_ccmode != ccNone;
    rgb24 *pRow = SmartMatrix::getCurrentRefreshRow(currentRow);
    rgb24 *pRow2 = SmartMatrix::getCurrentRefreshRow(currentRow + MATRIX_ROW_PAIR_OFFSET);
    rgb24 *pPrevRow = SmartMatrix::getPreviousRefreshRow(currentRow);
    rgb24 *pPrevRow2 = SmartMatrix::getPreviousRefreshRow(currentRow + MATRIX_ROW_PAIR_OFFSET);

    uint32_t fcCoefficient;
    uint32_t icPrev;
    uint32_t icNext;
    fcCoefficient = calculateFcInterpCoefficient();
    icPrev = 257 * (0x10000 - fcCoefficient);
    icNext = 257 * fcCoefficient;

    for (i = 0; i < MATRIX_WIDTH; i++) {

#if LATCHES_PER_ROW >= 12
        uint16_t temp0red,temp0green,temp0blue,temp1red,temp1green,temp1blue;
#else
        uint8_t temp0red,temp0green,temp0blue,temp1red,temp1green,temp1blue;
#endif

        if(bHasForeground) {
          if (!getForegroundPixel(i, currentRow, &tempPixel0))
              copyRgb24(tempPixel0, pRow[i]);
          if (!getForegroundPixel(i, currentRow + MATRIX_ROW_PAIR_OFFSET, &tempPixel1))
              copyRgb24(tempPixel1, pRow2[i]);

          temp0red = (tempPixel0.red);
          temp0green = (tempPixel0.green);
          temp0blue = (tempPixel0.blue);
          temp1red = (tempPixel1.red);
          temp1green = (tempPixel1.green);
          temp1blue = (tempPixel1.blue);
        } else {
          temp0red = lutInterpolate(lightPowerMap16bit2, ((pPrevRow[i].red * icPrev + pRow[i].red * icNext) >> 16));
          temp0green = lutInterpolate(lightPowerMap16bit2, ((pPrevRow[i].green * icPrev + pRow[i].green * icNext) >> 16));
          temp0blue = lutInterpolate(lightPowerMap16bit2, ((pPrevRow[i].blue * icPrev + pRow[i].blue * icNext) >> 16));
          temp1red = lutInterpolate(lightPowerMap16bit2, ((pPrevRow2[i].red * icPrev + pRow2[i].red * icNext) >> 16));
          temp1green = lutInterpolate(lightPowerMap16bit2, ((pPrevRow2[i].green * icPrev + pRow2[i].green * icNext) >> 16));
          temp1blue = lutInterpolate(lightPowerMap16bit2, ((pPrevRow2[i].blue * icPrev + pRow2[i].blue * icNext) >> 16));
        }


        if(bHasCC) {
#if LATCHES_PER_ROW >= 12
#if 0
            temp0red = colorCorrection8to16bit(temp0red);
            temp0green = colorCorrection8to16bit(temp0green);
            temp0blue = colorCorrection8to16bit(temp0blue);
            temp1red = colorCorrection8to16bit(temp1red);
            temp1green = colorCorrection8to16bit(temp1green);
            temp1blue = colorCorrection8to16bit(temp1blue);
#endif
#else
            temp0red = colorCorrection8bit(temp0red);
            temp0green = colorCorrection8bit(temp0green);
            temp0blue = colorCorrection8bit(temp0blue);
            temp1red = colorCorrection8bit(temp1red);
            temp1green = colorCorrection8bit(temp1green);
            temp1blue = colorCorrection8bit(temp1blue);
#endif
        } else {
#if LATCHES_PER_ROW == 16
            temp0red = temp0red << 8;
            temp0green = temp0green << 8;
            temp0blue = temp0blue << 8;
            temp1red = temp1red << 8;
            temp1green = temp1green << 8;
            temp1blue = temp1blue << 8;
#endif
        }

#if LATCHES_PER_ROW == 12
            temp0red >>= 4;
            temp0green >>= 4;
            temp0blue >>= 4;

            temp1red >>= 4;
            temp1green >>= 4;
            temp1blue >>= 4;
#endif
        // this technique is from Fadecandy
        union {
            uint32_t word;
            struct {
                // order of bits in word matches how GPIO connects to the display
                uint32_t GPIO_WORD_ORDER;
            };
        } o0, o1, clkset;


        o0.word = 0;
        // set bits starting from LSB brightness moving to MSB brightness with each byte across the word
        // each word contains four brightness levels for single set of pixels above
        // o0.p0clk = 0;
        // o0.p0pad = 0;
        o0.p0b1 = temp0blue    >> 0;
        o0.p0r1 = temp0red     >> 0;
        o0.p0r2 = temp1red     >> 0;
        o0.p0g1 = temp0green   >> 0;
        o0.p0g2 = temp1green   >> 0;
        o0.p0b2 = temp1blue    >> 0;

        // o0.p1clk = 0;
        // o0.p1pad = 0;
        o0.p1b1 = temp0blue    >> 1;
        o0.p1r1 = temp0red     >> 1;
        o0.p1r2 = temp1red     >> 1;
        o0.p1g1 = temp0green   >> 1;
        o0.p1g2 = temp1green   >> 1;
        o0.p1b2 = temp1blue    >> 1;

        // o0.p2clk = 0;
        // o0.p2pad = 0;
        o0.p2b1 = temp0blue    >> 2;
        o0.p2r1 = temp0red     >> 2;
        o0.p2r2 = temp1red     >> 2;
        o0.p2g1 = temp0green   >> 2;
        o0.p2g2 = temp1green   >> 2;
        o0.p2b2 = temp1blue    >> 2;

        // o0.p3clk = 0;
        // o0.p3pad = 0;
        o0.p3b1 = temp0blue    >> 3;
        o0.p3r1 = temp0red     >> 3;
        o0.p3r2 = temp1red     >> 3;
        o0.p3g1 = temp0green   >> 3;
        o0.p3g2 = temp1green   >> 3;
        o0.p3b2 = temp1blue    >> 3;


        // continue moving from LSB to MSB brightness with the next word
        o1.word = 0;
        // o1.p0clk = 0;
        // o1.p0pad = 0;
        o1.p0b1 = temp0blue    >> (0 + 1 * sizeof(uint32_t));
        o1.p0r1 = temp0red     >> (0 + 1 * sizeof(uint32_t));
        o1.p0r2 = temp1red     >> (0 + 1 * sizeof(uint32_t));
        o1.p0g1 = temp0green   >> (0 + 1 * sizeof(uint32_t));
        o1.p0g2 = temp1green   >> (0 + 1 * sizeof(uint32_t));
        o1.p0b2 = temp1blue    >> (0 + 1 * sizeof(uint32_t));

        // o1.p1clk = 0;
        // o1.p1pad = 0;
        o1.p1b1 = temp0blue    >> (1 + 1 * sizeof(uint32_t));
        o1.p1r1 = temp0red     >> (1 + 1 * sizeof(uint32_t));
        o1.p1r2 = temp1red     >> (1 + 1 * sizeof(uint32_t));
        o1.p1g1 = temp0green   >> (1 + 1 * sizeof(uint32_t));
        o1.p1g2 = temp1green   >> (1 + 1 * sizeof(uint32_t));
        o1.p1b2 = temp1blue    >> (1 + 1 * sizeof(uint32_t));

        // o1.p2clk = 0;
        // o1.p2pad = 0;
        o1.p2b1 = temp0blue    >> (2 + 1 * sizeof(uint32_t));
        o1.p2r1 = temp0red     >> (2 + 1 * sizeof(uint32_t));
        o1.p2r2 = temp1red     >> (2 + 1 * sizeof(uint32_t));
        o1.p2g1 = temp0green   >> (2 + 1 * sizeof(uint32_t));
        o1.p2g2 = temp1green   >> (2 + 1 * sizeof(uint32_t));
        o1.p2b2 = temp1blue    >> (2 + 1 * sizeof(uint32_t));

        // o1.p3clk = 0;
        // o1.p3pad = 0;
        o1.p3b1 = temp0blue    >> (3 + 1 * sizeof(uint32_t));
        o1.p3r1 = temp0red     >> (3 + 1 * sizeof(uint32_t));
        o1.p3r2 = temp1red     >> (3 + 1 * sizeof(uint32_t));
        o1.p3g1 = temp0green   >> (3 + 1 * sizeof(uint32_t));
        o1.p3g2 = temp1green   >> (3 + 1 * sizeof(uint32_t));
        o1.p3b2 = temp1blue    >> (3 + 1 * sizeof(uint32_t));

#if LATCHES_PER_ROW >= 12
        union {
            uint32_t word;
            struct {
                // order of bits in word matches how GPIO connects to the display
                uint32_t GPIO_WORD_ORDER;
            };
        } o2;

        o2.word = 0;
        //o2.p0clk = 0;
        //o2.p0pad = 0;
        o2.p0b1 = temp0blue    >> (0 + 2 * sizeof(uint32_t));
        o2.p0r1 = temp0red     >> (0 + 2 * sizeof(uint32_t));
        o2.p0r2 = temp1red     >> (0 + 2 * sizeof(uint32_t));
        o2.p0g1 = temp0green   >> (0 + 2 * sizeof(uint32_t));
        o2.p0g2 = temp1green   >> (0 + 2 * sizeof(uint32_t));
        o2.p0b2 = temp1blue    >> (0 + 2 * sizeof(uint32_t));

        //o2.p1clk = 0;
        //o2.p1pad = 0;
        o2.p1b1 = temp0blue    >> (1 + 2 * sizeof(uint32_t));
        o2.p1r1 = temp0red     >> (1 + 2 * sizeof(uint32_t));
        o2.p1r2 = temp1red     >> (1 + 2 * sizeof(uint32_t));
        o2.p1g1 = temp0green   >> (1 + 2 * sizeof(uint32_t));
        o2.p1g2 = temp1green   >> (1 + 2 * sizeof(uint32_t));
        o2.p1b2 = temp1blue    >> (1 + 2 * sizeof(uint32_t));

        //o2.p2clk = 0;
        //o2.p2pad = 0;
        o2.p2b1 = temp0blue    >> (2 + 2 * sizeof(uint32_t));
        o2.p2r1 = temp0red     >> (2 + 2 * sizeof(uint32_t));
        o2.p2r2 = temp1red     >> (2 + 2 * sizeof(uint32_t));
        o2.p2g1 = temp0green   >> (2 + 2 * sizeof(uint32_t));
        o2.p2g2 = temp1green   >> (2 + 2 * sizeof(uint32_t));
        o2.p2b2 = temp1blue    >> (2 + 2 * sizeof(uint32_t));


        //o2.p3clk = 0;
        //o2.p3pad = 0;
        o2.p3b1 = temp0blue    >> (3 + 2 * sizeof(uint32_t));
        o2.p3r1 = temp0red     >> (3 + 2 * sizeof(uint32_t));
        o2.p3r2 = temp1red     >> (3 + 2 * sizeof(uint32_t));
        o2.p3g1 = temp0green   >> (3 + 2 * sizeof(uint32_t));
        o2.p3g2 = temp1green   >> (3 + 2 * sizeof(uint32_t));
        o2.p3b2 = temp1blue    >> (3 + 2 * sizeof(uint32_t));
#endif

#if LATCHES_PER_ROW == 16
        union {
            uint32_t word;
            struct {
                // order of bits in word matches how GPIO connects to the display
                uint32_t GPIO_WORD_ORDER;
            };
        } o3;

        o3.word = 0;
        //o3.p0clk = 0;
        //o3.p0pad = 0;
        o3.p0b1 = temp0blue    >> (0 + 3 * sizeof(uint32_t));
        o3.p0r1 = temp0red     >> (0 + 3 * sizeof(uint32_t));
        o3.p0r2 = temp1red     >> (0 + 3 * sizeof(uint32_t));
        o3.p0g1 = temp0green   >> (0 + 3 * sizeof(uint32_t));
        o3.p0g2 = temp1green   >> (0 + 3 * sizeof(uint32_t));
        o3.p0b2 = temp1blue    >> (0 + 3 * sizeof(uint32_t));

        //o3.p1clk = 0;
        //o3.p1pad = 0;
        o3.p1b1 = temp0blue    >> (1 + 3 * sizeof(uint32_t));
        o3.p1r1 = temp0red     >> (1 + 3 * sizeof(uint32_t));
        o3.p1r2 = temp1red     >> (1 + 3 * sizeof(uint32_t));
        o3.p1g1 = temp0green   >> (1 + 3 * sizeof(uint32_t));
        o3.p1g2 = temp1green   >> (1 + 3 * sizeof(uint32_t));
        o3.p1b2 = temp1blue    >> (1 + 3 * sizeof(uint32_t));

        //o3.p2clk = 0;
        //o3.p2pad = 0;
        o3.p2b1 = temp0blue    >> (2 + 3 * sizeof(uint32_t));
        o3.p2r1 = temp0red     >> (2 + 3 * sizeof(uint32_t));
        o3.p2r2 = temp1red     >> (2 + 3 * sizeof(uint32_t));
        o3.p2g1 = temp0green   >> (2 + 3 * sizeof(uint32_t));
        o3.p2g2 = temp1green   >> (2 + 3 * sizeof(uint32_t));
        o3.p2b2 = temp1blue    >> (2 + 3 * sizeof(uint32_t));


        //o3.p3clk = 0;
        //o3.p3pad = 0;
        o3.p3b1 = temp0blue    >> (3 + 3 * sizeof(uint32_t));
        o3.p3r1 = temp0red     >> (3 + 3 * sizeof(uint32_t));
        o3.p3r2 = temp1red     >> (3 + 3 * sizeof(uint32_t));
        o3.p3g1 = temp0green   >> (3 + 3 * sizeof(uint32_t));
        o3.p3g2 = temp1green   >> (3 + 3 * sizeof(uint32_t));
        o3.p3b2 = temp1blue    >> (3 + 3 * sizeof(uint32_t));
#endif

        clkset.word = 0x00;
        clkset.p0clk = 1;
        clkset.p1clk = 1;
        clkset.p2clk = 1;
        clkset.p3clk = 1;

        // copy words to DMA buffer
        matrixUpdateData[freeRowBuffer][i][0] = o0.word;
        matrixUpdateData[freeRowBuffer][i][1] = o1.word;

        // copy the next set of words with the same data, but clock set high
        matrixUpdateData[freeRowBuffer][i][LATCHES_PER_ROW / sizeof(uint32_t) + 0] = o0.word | clkset.word;
        matrixUpdateData[freeRowBuffer][i][LATCHES_PER_ROW / sizeof(uint32_t) + 1] = o1.word | clkset.word;

#if LATCHES_PER_ROW >= 12
        matrixUpdateData[freeRowBuffer][i][2] = o2.word;
        matrixUpdateData[freeRowBuffer][i][LATCHES_PER_ROW / sizeof(uint32_t) + 2] = o2.word | clkset.word;
#endif
#if LATCHES_PER_ROW == 16
        matrixUpdateData[freeRowBuffer][i][3] = o3.word;
        matrixUpdateData[freeRowBuffer][i][LATCHES_PER_ROW / sizeof(uint32_t) + 3] = o3.word | clkset.word;
#endif
    }

}

void dma_ch1_isr(void) {
#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_2, HIGH); // oscilloscope trigger
#endif

    SmartMatrix::matrixCalculations();

#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_2, LOW);
#endif
}

// DMA transfer done (meaning data was shifted and timer value for MSB on current row just got loaded)
// set DMA up for loading the next row, triggered from the next timer latch
void dma_ch3_isr(void) {
#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_1, HIGH); // oscilloscope trigger
#endif
    // done with previous row, mark it as read
    cbRead(&dmaBuffer);

    // get next row to draw to display and update DMA pointers
    int currentRow = cbGetNextRead(&dmaBuffer);
    DMA_TCD1_SADDR = &matrixUpdateBlocks[currentRow][0].addressValues;
    DMA_TCD2_SADDR = &matrixUpdateBlocks[currentRow][0].timerValues.timer_oe;
    DMA_TCD3_SADDR = matrixUpdateData[currentRow][0];

    // clear pending GPIO int for PORTA before enabling DMA again
    CORE_PIN3_CONFIG |= (1 << 24);

    // trigger software interrupt (DMA channel used instead of actual softint)
    NVIC_SET_PENDING(IRQ_DMA_CH1);

    // clear pending int
    DMA_CINT = 3;

#ifdef DEBUG_PINS_ENABLED
    digitalWriteFast(DEBUG_PIN_1, LOW); // oscilloscope trigger
#endif
}
