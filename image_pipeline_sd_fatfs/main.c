/* Copyright (C) 2023-2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */
#include <time.h>
#include <stdlib.h>
#include <sys_utils.h>
#include <math.h>

#include "RTE_Components.h"
#include CMSIS_device_header
#include "Driver_IO.h"
#include "board_defs.h"
#include "alifs_profile.h"
#include "pinconf.h"

#include "aipl_image.h"
#include "board_config.h"
#include "camera.h"
#include "disp.h"
#include "jpeg_app.h"

#include "ff.h"
#include "diskio.h"

#include "power_management.h"
#include "se_services_port.h"
#include "alif_logo.h"

#include "Driver_CPI.h"

extern void clk_init();  // time.h clock functionality (from retarget.c)
extern ARM_DRIVER_CPI Driver_CPI;
static ARM_DRIVER_CPI *CAMERAdrv = &Driver_CPI;

// declare y_buffer object here.
#if (ISP_OUTPUT_SIZE_Y)
extern uint8_t y_buffer[RTE_ISP_BUFFER_COUNT][ISP_OUTPUT_SIZE_Y];
#endif
// declare cbcr_bufer object.
#if (ISP_OUTPUT_SIZE_CBCR)
extern uint8_t cbcr_buffer[RTE_ISP_BUFFER_COUNT][ISP_OUTPUT_SIZE_CBCR];
#endif

extern ARM_JPEG_BUFFER     jpeg_buffer_output;

#include "aipl_color_conversion.h"
#include "isp_header.h"

// Check if UART trace is disabled
#if !defined(DISABLE_UART_TRACE)
#include <stdio.h>

#include "uart_tracelib.h"

static void uart_callback(uint32_t event) {}
#else
#define printf(fmt, ...) (0)
#endif

uint32_t time_0, time_1, time_2, time_3, time_4, time_5;
// This is used in the image file name "IMG_000x.jpeg".
uint32_t image_number = 1;

// Success indicator
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB0_G_GPIO_PORT);
static ARM_DRIVER_GPIO* green_port = &ARM_Driver_GPIO_(BOARD_LEDRGB0_G_GPIO_PORT);

#if BOARD_LEDRGB0_G_GPIO_PORT != BOARD_LEDRGB0_R_GPIO_PORT
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB0_R_GPIO_PORT);
#endif
static ARM_DRIVER_GPIO* red_port = &ARM_Driver_GPIO_(BOARD_LEDRGB0_R_GPIO_PORT);

// User button press indicator
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_JOY_SW_CENTER_GPIO_PORT);
static ARM_DRIVER_GPIO* joy_sw_port = &ARM_Driver_GPIO_(BOARD_JOY_SW_CENTER_GPIO_PORT);
bool camera_trigger = false;

// SD card controller reset
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_SD_RESET_GPIO_PORT);
ARM_DRIVER_GPIO *sd_rst_gpio = &ARM_Driver_GPIO_(BOARD_SD_RESET_GPIO_PORT);

// Print measurements
#define PRINT_INTERVAL_SEC    (1)
#define PRINT_INTERVAL_CLOCKS (PRINT_INTERVAL_SEC * CLOCKS_PER_SEC)
extern uint32_t SystemCoreClock;

// Onboard user button call back.
static void joy_sw_callback(uint32_t event) {
    if(event & ARM_GPIO_IRQ_EVENT_EXTERNAL)
        camera_trigger = true;
}

void sd_routine(uint32_t size, uint32_t *num);
uint32_t sd_get_next_image_number(void);

int main(void) 
{
    bool camera_triggered = false;
    int32_t board_init_ret = board_pins_config();
    if(board_init_ret) {
        __BKPT(0);
    }

    board_init_ret = board_gpios_config();
    if(board_init_ret) {
        __BKPT(0);
    }

    /* Initialize the SE services */
    se_services_port_init();

    board_init_ret = board_clocks_config(CLKEN_HFOSC_MASK | CLKEN_CLK_100M_MASK);
    if(board_init_ret) {
        __BKPT(0);
    }
    
    // Enable 20MHz clock for SD card interface (accoding to demo_sd.c)
    uint32_t service_error_code;
    uint32_t error_code = SERVICES_REQ_SUCCESS;
    error_code = SERVICES_clocks_enable_clock(se_services_s_handle,
                                              CLKEN_CLK_20M,
                                              true,
                                              &service_error_code);
    if (error_code) {
        printf("SE: SDMMC 20MHz clock enable = %" PRIu32 "\n", error_code);
        return 0;
    }

    green_port->Initialize(BOARD_LEDRGB0_G_GPIO_PIN, NULL);
    green_port->PowerControl(BOARD_LEDRGB0_G_GPIO_PIN, ARM_POWER_FULL);
    green_port->SetDirection(BOARD_LEDRGB0_G_GPIO_PIN, GPIO_PIN_DIRECTION_OUTPUT);

    red_port->Initialize(BOARD_LEDRGB0_R_GPIO_PIN, NULL);
    red_port->PowerControl(BOARD_LEDRGB0_R_GPIO_PIN, ARM_POWER_FULL);
    red_port->SetDirection(BOARD_LEDRGB0_R_GPIO_PIN, GPIO_PIN_DIRECTION_OUTPUT);

    joy_sw_port->Initialize(BOARD_JOY_SW_CENTER_GPIO_PIN, joy_sw_callback);
    joy_sw_port->PowerControl(BOARD_JOY_SW_CENTER_GPIO_PIN, ARM_POWER_FULL);
    joy_sw_port->SetDirection(BOARD_JOY_SW_CENTER_GPIO_PIN, GPIO_PIN_DIRECTION_INPUT);
    uint32_t flags = (ARM_GPIO_IRQ_EDGE_SENSITIVE_SINGLE | ARM_GPIO_IRQ_SENSITIVE_EDGE | ARM_GPIO_IRQ_POLARITY_HIGH);
    joy_sw_port->Control(BOARD_JOY_SW_CENTER_GPIO_PIN, ARM_GPIO_ENABLE_INTERRUPT, &flags);

    sd_rst_gpio->Initialize(BOARD_SD_RESET_GPIO_PIN, NULL);
    sd_rst_gpio->PowerControl(BOARD_SD_RESET_GPIO_PIN, ARM_POWER_FULL);
    sd_rst_gpio->SetDirection(BOARD_SD_RESET_GPIO_PIN, GPIO_PIN_DIRECTION_OUTPUT);
    sd_rst_gpio->SetValue(BOARD_SD_RESET_GPIO_PIN, GPIO_PIN_OUTPUT_STATE_HIGH);

#ifdef BOARD_SD_CARD_DETECT_GPIO_PORT
    /* Configure card detect GPIO with pull-up and read enable */
    pinconf_set(PORT_(BOARD_SD_CARD_DETECT_GPIO_PORT), BOARD_SD_CARD_DETECT_GPIO_PIN,
                PINMUX_ALTERNATE_FUNCTION_0,
                (PADCTRL_DRIVER_DISABLED_PULL_UP | PADCTRL_READ_ENABLE));
#endif

    pinconf_set(PORT_(BOARD_SD_CMD_A_GPIO_PORT),
                BOARD_SD_CMD_A_GPIO_PIN,
                BOARD_SD_CMD_ALTERNATE_FUNCTION,
                PADCTRL_READ_ENABLE | PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA);  // cmd
    pinconf_set(PORT_(BOARD_SD_CLK_A_GPIO_PORT),
                BOARD_SD_CLK_A_GPIO_PIN,
                BOARD_SD_CLK_ALTERNATE_FUNCTION,
                PADCTRL_READ_ENABLE | PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA);  // clk
    pinconf_set(PORT_(BOARD_SD_D0_A_GPIO_PORT),
                BOARD_SD_D0_A_GPIO_PIN,
                BOARD_SD_D0_ALTERNATE_FUNCTION,
                PADCTRL_READ_ENABLE | PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA);  // d0
#if RTE_SDC_BUS_WIDTH == SDMMC_4_BIT_MODE
    pinconf_set(PORT_(BOARD_SD_D1_A_GPIO_PORT),
                BOARD_SD_D1_A_GPIO_PIN,
                BOARD_SD_D1_ALTERNATE_FUNCTION,
                PADCTRL_READ_ENABLE | PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA);  // d1
    pinconf_set(PORT_(BOARD_SD_D2_A_GPIO_PORT),
                BOARD_SD_D2_A_GPIO_PIN,
                BOARD_SD_D2_ALTERNATE_FUNCTION,
                PADCTRL_READ_ENABLE | PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA);  // d2
    pinconf_set(PORT_(BOARD_SD_D3_A_GPIO_PORT),
                BOARD_SD_D3_A_GPIO_PIN,
                BOARD_SD_D3_ALTERNATE_FUNCTION,
                PADCTRL_READ_ENABLE | PADCTRL_OUTPUT_DRIVE_STRENGTH_12MA);  // d3
#endif

    /* Enable MIPI power */
    bool pm_ok = init_power_management();
    if (!pm_ok) {
        printf("\r\nError: power management init failed.\r\n");
        __BKPT(0);
    }

#if !defined(DISABLE_UART_TRACE)
    tracelib_init(NULL, uart_callback);
#endif
    int ret = 0;
    sys_busy_loop_init();

    ret = camera_init();
    if (ret != ARM_DRIVER_OK) {
        __BKPT(1);
    }
    ret = display_init();
    if (ret != ARM_DRIVER_OK) {
        printf("\r\ndisplay initialization fail.\r\n");
        __BKPT(2);
    }
    
    ret = jpeg_enc_init();
    if (ret != ARM_DRIVER_OK) {
        printf("jpeg driver initialization fail.\r\n");
        __BKPT(2);
    }
        
    printf("\r\nAlifSemiconductor Image Pipeline Demo Application\r\n");
    printf(">>Hardware initialization successful.\r\n");

    image_number = sd_get_next_image_number();
    printf("Next image number: %" PRIu32 "\r\n", image_number);
        
    aipl_image_t lcd_image;
    lcd_image.format    = AIPL_COLOR_RGB565;
    lcd_image.height    = ISP_OUTPUT_X;
    lcd_image.pitch     = ISP_OUTPUT_Y;
    lcd_image.width     = ISP_OUTPUT_Y;
    
    while (ret == ARM_DRIVER_OK)
    {
        // Blink green LED
        green_port->SetValue(BOARD_LEDRGB0_G_GPIO_PIN, GPIO_PIN_OUTPUT_STATE_TOGGLE);        

        time_0  = DWT->CYCCNT;
        ret = camera_capture();
        time_1  = DWT->CYCCNT;
        
        if (ret == ARM_DRIVER_OK)
        {
            if(camera_trigger == true)
            {
                camera_trigger = false;
                camera_triggered = true;
                ret = jpeg_compress();
                if (ret != ARM_DRIVER_OK) {
                    printf("\r\nJPEG encoder compression fail.\r\n");
                    __BKPT(2);
                }
                
                // Calculate block/sector size that occupied by the jpeg image. This value is aligned with 512 bytes.
                uint32_t block_size = (jpeg_buffer_output.size + 511)/512*512;
                sd_routine(block_size, &image_number);
                ret = ARM_DRIVER_OK;
            }
            
            aipl_error_t aipl_ret = 0;
            lcd_image.data = disp_inactive_buffer();    // pick up the inactive display to receive data
            time_2  = DWT->CYCCNT;
            aipl_ret = aipl_color_convert_nv12_to_rgb565(y_buffer[0], lcd_image.data, ISP_OUTPUT_X, ISP_OUTPUT_X, ISP_OUTPUT_Y);
            
            disp_next_frame_vsync();
            time_3  = DWT->CYCCNT;
            
            printf("CPI+ISP: %.3fms, %.3fHz,", 
                        (time_1-time_0) * 1000.0f / SystemCoreClock, 
                        SystemCoreClock * 1.0f / (time_1-time_0));
            printf(" AIPL: %.3fms, Total: %.3fms, %.3fHz\r\n", 
                        (time_3-time_2) * 1000.0f / SystemCoreClock, 
                        (time_3-time_0) * 1000.0f / SystemCoreClock, 
                        SystemCoreClock * 1.0f / (time_3-time_0));
            if(camera_triggered == true) 
            {
                float t_comp_ms = (time_4-time_1) * 1000.0f / SystemCoreClock;
                float tp = (RTE_ISP_OUTPUT_WIDTH*RTE_ISP_OUTPUT_HEIGHT/1000.0f) / t_comp_ms;
                printf(">> JPEG: %.3fms, %.2fMPixel/s, SD card: %.3fms\r\n", 
                        t_comp_ms, 
                        tp,        // Throughput
                        (time_5-time_4) * 1000.0f / SystemCoreClock);
                printf(">> savebin \"dump_sram.jpeg\", 0x%08X, 0x%08X\r\n", (unsigned int)jpeg_buffer_output.buf, (unsigned int)jpeg_buffer_output.size);
                camera_triggered = false;
            }
        }
        else 
        {
            printf("\r\n Error: CAMERA Capture Frame failed.\r\n");
        }
    }

    // Set RED LED in error case
    red_port->SetValue(BOARD_LEDRGB0_R_GPIO_PIN, GPIO_PIN_OUTPUT_STATE_HIGH);

    while (1) {
        __WFI();
    }
}

// SD card related application. The write feature follows the `demo_sd_fatfs.c` baremetal example
#define MEDIA_NAME                     "/SD_DISK/"
#define TEST_FILE                      "/image_captured.jpeg"

/* Buffer for FileX FF_Disk_t sector cache. This must be large enough for at least one sector ,
 * which are typically 512 bytes in size. */
FATFS sd_card __attribute__((section("sd_dma_buf"))) __attribute__((aligned(512)));
FIL test_file __attribute__((section("sd_dma_buf"))) __attribute__((aligned(512)));

// This routine is added by AI.
// Scans the SD card for existing IMG_XXXX.jpeg files and returns the next
// available image number (highest existing number + 1), or 1 if none found.
uint32_t sd_get_next_image_number(void)
{
    FRESULT fr;
    FILINFO fno;
    DIR dir;
    uint32_t max_num = 0;

    fr = f_mount(&sd_card, MEDIA_NAME, 1);
    if (fr) {
        printf("sd_get_next_image_number: mount failed (%" PRId16 ")\n", fr);
        return 1;
    }

    fr = f_findfirst(&dir, &fno, "/", "IMG_????.jpeg");
    while (fr == FR_OK && fno.fname[0] != '\0') {
        // fname format: IMG_XXXX.jpeg
        uint32_t n = 0;
        if (sscanf(fno.fname, "IMG_%" SCNu32 ".jpeg", &n) == 1) {
            if (n > max_num)
                max_num = n;
        }
        fr = f_findnext(&dir, &fno);
    }
    fr = f_closedir(&dir);
    fr = f_mount(NULL, MEDIA_NAME, 0);   // unmount; sd_routine will re-mount

    return max_num + 1;
}

// the size argument is image size that is being aligned to 512
// num is the file number in file name: IMG_000x.jpeg
void sd_routine(uint32_t size, uint32_t *num)
{
    FRESULT fr;
    UINT bw;

    time_4  = DWT->CYCCNT;
    /* Open the SD disk. and initialize SD controller */
    fr = f_mount(&sd_card, MEDIA_NAME, 1);

    /* Check the media open status.  */
    if (fr) {
        printf("media open fail status = %" PRId16 "...\n", fr);
        __BKPT(2);
    }
    // printf("SD Mounted Successfully...\n");

    char file_name[20];
    snprintf(file_name, sizeof(file_name), "IMG_%04d.jpeg", (int)*num);
    fr = f_open(&test_file, file_name, FA_CREATE_ALWAYS | FA_WRITE);

    /* Check the file open status.  */
    if (fr) {
        printf("File open status: %" PRIi16 "\n", fr);
        /* Error opening file, break the loop.  */
        __BKPT(2);
    }

    // printf("Writing Data in File...%s\n", file_name);

    for (int32_t i = 0; i < 1; i++) {

        /* Write content to the SD card  */
        // in DFP 2.2.0, the SD card driver only support write 512 bytes block at one time
        for (uint32_t offset = 0; offset < size; offset += 512) 
        {
            fr = f_write(&test_file, (void *)(jpeg_buffer_output.buf + offset), 512, &bw);
            if (fr) break;
        }

        /* Check the file write status.  */
        if (fr) {
            printf("ittr: %" PRId32 " File write status: %" PRIi16 "\n", i, fr);
            break;
        }
    }

    // printf("Closing File...%s\n", file_name);

    /* Close the test file.  */
    fr = f_close(&test_file);

    /* Check the file close status.  */
    if (fr) {
        printf("File close status: %" PRId16 "\n", fr);
        /* Error closing the file, break the loop.  */
        __BKPT(2);
    }
    (*num)++;
    time_5  = DWT->CYCCNT;
    printf("%s saved.\n",file_name);
}
