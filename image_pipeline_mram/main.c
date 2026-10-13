/* Copyright (C) 2023-2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */
#include <stdlib.h>
#include <sys_utils.h>

#include "RTE_Components.h"
#include CMSIS_device_header
#include "Driver_IO.h"
#include "board_defs.h"
#include "alifs_profile.h"

#include "aipl_image.h"
#include "aipl_color_conversion.h"
#include "isp_header.h"
#include "board_config.h"
#include "camera.h"
#include "disp.h"
#include "jpeg_app.h"

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

// extern uint8_t out_buffer[OUT_BUF_SIZE];
extern ARM_JPEG_BUFFER     jpeg_buffer_output;

// Check if UART trace is disabled
#if !defined(DISABLE_UART_TRACE)
#include <stdio.h>

#include "uart_tracelib.h"

static void uart_callback(uint32_t event) {}
#else
#define printf(fmt, ...) (0)
#endif

extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB0_G_GPIO_PORT);
static ARM_DRIVER_GPIO* green_port = &ARM_Driver_GPIO_(BOARD_LEDRGB0_G_GPIO_PORT);

#if BOARD_LEDRGB0_G_GPIO_PORT != BOARD_LEDRGB0_R_GPIO_PORT
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_LEDRGB0_R_GPIO_PORT);
#endif

static ARM_DRIVER_GPIO* red_port = &ARM_Driver_GPIO_(BOARD_LEDRGB0_R_GPIO_PORT);

extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(BOARD_JOY_SW_CENTER_GPIO_PORT);
static ARM_DRIVER_GPIO* joy_sw_port = &ARM_Driver_GPIO_(BOARD_JOY_SW_CENTER_GPIO_PORT);
bool camera_trigger = false;

static void joy_sw_callback(uint32_t event) {
    if(event & ARM_GPIO_IRQ_EVENT_EXTERNAL)
        camera_trigger = true;
}

int main(void) 
{
    uint32_t time_0, time_1, time_2, time_3, time_4, time_5;
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
    // Init camera
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
    ret = mram_init();
    if (ret != ARM_DRIVER_OK) {
        printf("MRAM driver initialization fail.\r\n");
        __BKPT(2);
    }
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    printf("\r\nAlifSemiconductor Image Pipeline Demo Application\r\n");
    printf(">>Hardware initialization successful\r\n");
        
    aipl_image_t lcd_image;
    lcd_image.format    = AIPL_COLOR_RGB565;
    lcd_image.height    = ISP_OUTPUT_X;
    lcd_image.pitch     = ISP_OUTPUT_Y;
    lcd_image.width     = ISP_OUTPUT_Y;
    
    while (ret == ARM_DRIVER_OK)
    {
        // Blink green LED
        green_port->SetValue(BOARD_LEDRGB0_G_GPIO_PIN, GPIO_PIN_OUTPUT_STATE_TOGGLE);
        // save the last time_0 stamp to calculate a full camera cycle.
        // because when the rest of the processings are done, the next frame is not ready yet.
        uint32_t time_00 = time_0;
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
                    printf("\r\njpeg encoder compression fail.\r\n");
                    __BKPT(2);
                }
                
                time_4  = DWT->CYCCNT;
                // MRAM write address is the APP_MRAM_USER_BASE defined in app_mem_regions.h
                ret = MRAM_drv->ProgramData(0, jpeg_buffer_output.buf, jpeg_buffer_output.size);
                if (ret != jpeg_buffer_output.size) 
                {
                    printf("\r\n Error: MRAM ProgramData failed.\r\n");
                }
                time_5  = DWT->CYCCNT;
                ret = ARM_DRIVER_OK;
            }
            
            aipl_error_t aipl_ret = 0;
            lcd_image.data = disp_inactive_buffer();    // pick up the inactive display to receive data
            time_2  = DWT->CYCCNT;
            aipl_ret = aipl_color_convert_nv12_to_rgb565(y_buffer[0], lcd_image.data, ISP_OUTPUT_X, ISP_OUTPUT_X, ISP_OUTPUT_Y);
                        
            disp_next_frame_vsync();
            time_3  = DWT->CYCCNT;

            printf("CPI+ISP: %.3fms,", 
                        (time_1-time_0) * 1000.0f / SystemCoreClock);
            printf(" AIPL: %.3fms, Total: %.3fms, %.3fHz\r\n", 
                        (time_3-time_2) * 1000.0f / SystemCoreClock, 
                        (time_3-time_0) * 1000.0f / SystemCoreClock, 
                        SystemCoreClock * 1.0f / (time_0-time_00));
            if(camera_triggered == true) 
            {
                float t_comp_ms = (time_4-time_1) * 1000.0f / SystemCoreClock;
                float tp = (RTE_ISP_OUTPUT_WIDTH*RTE_ISP_OUTPUT_HEIGHT/1000.0f) / t_comp_ms;
                printf(">> JPEG: %.3fms, %.2fMPixel/s, MRAM: %.3fms\r\n", 
                        t_comp_ms, 
                        tp,        // Throughput
                        (time_5-time_4) * 1000.0f / SystemCoreClock);
                printf(">> savebin \"dump_sram.jpeg\", 0x%08X, 0x%08X\r\n", (unsigned int)jpeg_buffer_output.buf, (unsigned int)jpeg_buffer_output.size);
                printf(">> savebin \"dump_mram.jpeg\", 0x%08X, 0x%08X\r\n", 0x80400000, (unsigned int)jpeg_buffer_output.size);
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
