/**
 * @file disp.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "disp.h"
#include <stdbool.h>
#include <stdio.h>

#include "RTE_Components.h"
#include CMSIS_device_header
#include <RTE_Device.h>
#include "Driver_CDC200.h" // Display driver
#include "aipl_image.h"
#include "alif_logo.h"
#include "Driver_CDC_Private.h"
#include "display.h"
/*********************
 *      DEFINES
 *********************/
// #ifndef MY_DISP_HOR_RES
//     // Replace the macro MY_DISP_HOR_RES with the actual screen width.
//     #define MY_DISP_HOR_RES    (RTE_PANEL_HACTIVE_TIME)
// #endif

// #ifndef MY_DISP_VER_RES
//     // Replace the macro MY_DISP_HOR_RES with the actual screen height.
//     #define MY_DISP_VER_RES    (RTE_PANEL_VACTIVE_LINE)
// #endif

/**********************
 *      TYPEDEFS
 **********************/

#pragma pack(1)
#if RTE_CDC200_PIXEL_FORMAT == 0    // ARGB8888
typedef uint32_t Pixel;
#elif RTE_CDC200_PIXEL_FORMAT == 1  // RGB888
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} Pixel;
#elif RTE_CDC200_PIXEL_FORMAT == 2  // RGB565
typedef uint16_t Pixel;
#else
#error "CDC200 Unsupported color format"
#endif
#pragma pack()

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void disp_callback(uint32_t event);

/**********************
 *  STATIC VARIABLES
 **********************/

 // In order to really initialize these two, linker script file needs to be updated.
 // By adding "LONG (ADDR(.bss.at_sram1)) LONG (SIZEOF(.bss.at_sram1)/4)" to "zero.table" section.
 // If not doing the linker script change, manually write 0xffff to each of the buffer
 // in the display_init() function.
static Pixel lcd_buffer_1[MY_DISP_VER_RES][MY_DISP_HOR_RES]
            __attribute__((section(".bss.lcd_frame_buf1"))) = {0};
static Pixel lcd_buffer_2[MY_DISP_VER_RES][MY_DISP_HOR_RES]
            __attribute__((section(".bss.lcd_frame_buf2"))) = {0};

enum {
    BUFFER_1 = 0,
    BUFFER_2 = 1,
    NUM_BUFFERS
};

static Pixel* buffers[NUM_BUFFERS] = { (Pixel*)&lcd_buffer_1, (Pixel*)&lcd_buffer_2 };

static uint8_t  current_buffer = BUFFER_1;
static uint32_t frame_durations[NUM_BUFFERS] = { 1, 1 };
static uint32_t switch_times[NUM_BUFFERS] = { 0, 0 };

extern ARM_DRIVER_CDC200 Driver_CDC200;
static ARM_DRIVER_CDC200 *CDCdrv = &Driver_CDC200;
extern CDC_RESOURCES CDC_RES;
/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

 // This routine converts bgralpha888 to rgb565
 // the alif_lut array stores color in 
 // [Blue, Green, Red, Alpha] sequence
uint16_t rgba888_to_rgb565(uint16_t index)
{
	const uint8_t *lut = get_alif_lut();
	uint16_t blu = lut[index*4];
	uint16_t gre = lut[index*4+1];
	uint16_t red = lut[index*4+2];
	red = (red>>3)<<11;
	gre = (gre>>2)<<5;
	blu = (blu>>3);
	return red|gre|blu;
}
#define LOGO_POSITION_X 100
#define LOGO_POSITION_Y 700

/*Initialize your display and the required peripherals.*/
int display_init(void)
{
    /* Initialize CDC driver */
    int ret = CDCdrv->Initialize(disp_callback);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: CDC init failed\n");
        return ret;
    }

    /* Power control CDC */
    ret = CDCdrv->PowerControl(ARM_POWER_FULL);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: CDC Power up failed\n");
        return ret;
    }

    /* configure CDC controller */
    uint32_t fb_add = (uint32_t)disp_active_buffer();
    ret = CDCdrv->Control(CDC200_CONFIGURE_DISPLAY, fb_add);
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: CDC controller configuration failed\n");
        return ret;
    }

    /* Start CDC */
    ret = CDCdrv->Start();
    if(ret != ARM_DRIVER_OK){
        printf("\r\n Error: CDC Start failed\n");
        return ret;
    }

    uint16_t row, col;
    for(row = 0; row < MY_DISP_VER_RES; row++)
    {
        for(col = 0; col < MY_DISP_HOR_RES; col++)
        {
            lcd_buffer_1[row][col] = 0xffff;
            lcd_buffer_2[row][col] = 0xffff;
        }
    }
    // the following attempts to print the logo on the white region
    // However the raw logo is coded through the lut which is coded
    // in RGB8888. a color conversion is needed to display the color properly.
    // the rgba888_to_rgb565 routine provides color conversion.
    const aipl_image_t *logo = get_alif_logo();
    const uint8_t *lut = get_alif_lut();
    const uint8_t *pixels = (const uint8_t *)logo->data;
    uint16_t index = 0;
    for(row = LOGO_POSITION_Y; row < LOGO_POSITION_Y+logo->height; row++)
    {
        for(col = LOGO_POSITION_X; col < LOGO_POSITION_X+logo->width; col++)
        {
            lcd_buffer_1[row][col] = rgba888_to_rgb565(pixels[index]);
            lcd_buffer_2[row][col] = rgba888_to_rgb565(pixels[index]);
            index++;
        }
    }
    return ret;
}

void disp_next_frame(void)
{
    current_buffer = (current_buffer + 1) % NUM_BUFFERS;

    CDCdrv->Control(CDC200_FRAMEBUF_UPDATE, (uint32_t)buffers[current_buffer]);
}

void disp_next_frame_vsync(void)
{
    current_buffer = (current_buffer + 1) % NUM_BUFFERS;

    CDCdrv->Control(CDC200_FRAMEBUF_UPDATE_VSYNC, (uint32_t)buffers[current_buffer]);
}

void* disp_active_buffer(void)
{
    return buffers[current_buffer];
}

void* disp_inactive_buffer(void)
{
    return buffers[(current_buffer + 1) % NUM_BUFFERS];
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/* Display errors handler
 */
static void disp_callback(uint32_t event)
{
    if(event & ARM_CDC_DSI_ERROR_EVENT)
    {
        // Transfer Error: Received Hardware error.
        __BKPT(0);
    }
}
