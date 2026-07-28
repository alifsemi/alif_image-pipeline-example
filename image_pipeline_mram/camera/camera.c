/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 */
#include "camera.h"
#include "isp_header.h"

#include <math.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys_utils.h>

#include "Driver_CPI.h"
#include "alifs_profile.h"

#if RTE_CPI_AXI_PORT
#error "RTE_CPI_AXI_PORT should be disabled in this example to save the raw camera frame buffer memory"
#endif

/* Camera  Driver instance 0 */
extern ARM_DRIVER_CPI Driver_CPI;
static ARM_DRIVER_CPI *CAMERAdrv = &Driver_CPI;

typedef enum {
    CAM_CB_EVENT_NONE                 = 0,
    CAM_CB_EVENT_CAPTURE_STOPPED      = (1 << 0),       // event 1
    CAM_CB_EVENT_ERROR                = (1 << 1),       // all CPI error

    ISP_VSYNC_CB_EVENT                = (1 << 2),       // event 2
    ISP_MI_FRAME_DUMP_EVENT           = (1 << 3),       // event 3
    ISP_CB_EVENT_ERROR                = (1 << 4),       // all ISP errors
    
    JPEG_FRAME_READY_CB_EVENT         = (1 << 5),       // event 4
    JPEG_BUF_FULL_ERR_CB_EVENT        = (1 << 6),       // all JPEG errors
}CAM_CB_EVENT;

static volatile CAM_CB_EVENT g_cam_cb_events = CAM_CB_EVENT_NONE;

// The following is moved from the isp_header.h. Move object definition
// to source file to avoid double define, so the header can be reused.
#if (ISP_OUTPUT_SIZE_Y)
uint8_t y_buffer[RTE_ISP_BUFFER_COUNT][ISP_OUTPUT_SIZE_Y] \
    __attribute__((section(".bss.isp_buf1"), aligned(32)));
#endif
#if (ISP_OUTPUT_SIZE_CB)
uint8_t cb_buffer[RTE_ISP_BUFFER_COUNT][ISP_OUTPUT_SIZE_CB] \
    __attribute__((section(".bss.lcd_frame_buf"), aligned(32)));
#endif

#if (ISP_OUTPUT_SIZE_CR)
uint8_t cr_buffer[RTE_ISP_BUFFER_COUNT][ISP_OUTPUT_SIZE_CR] \
    __attribute__((section(".bss.lcd_frame_buf"), aligned(32)));
#endif

#if (ISP_OUTPUT_SIZE_CBCR)
uint8_t cbcr_buffer[RTE_ISP_BUFFER_COUNT][ISP_OUTPUT_SIZE_CBCR] \
    __attribute__((section(".bss.isp_buf2"), aligned(32)));
#endif

VIDEO_BUF_S buffer_array[RTE_ISP_BUFFER_COUNT];
// Above is moved from isp_header.h

static void isp_buffer_init(void) 
{
    for (int i = 0; i < RTE_ISP_BUFFER_COUNT; i++) 
    {
        buffer_array[i].index = i;
        switch (ISP_AUX_BUFFER_TYPE)        // the buffer type is ISP_INTERLEAVED
        {
            case ISP_PLANAR:
                buffer_array[i].numPlanes = 3;
                break;
            case ISP_SEMIPLANAR:
                buffer_array[i].numPlanes = 2;
                break;
            case ISP_NONE:
            case ISP_INTERLEAVED:
            default:
                buffer_array[i].numPlanes = 1;
                break;
        }
        buffer_array[i].imageSize = ISP_OUTPUT_TOTAL_SIZE;
        buffer_array[i].planes[0].dmaPhyAddr = (vsi_dma_t)y_buffer[i];
#if ISP_OUTPUT_SIZE_CB
        buffer_array[i].planes[1].dmaPhyAddr = (vsi_dma_t)cb_buffer[i];
#endif
#if ISP_OUTPUT_SIZE_CR
        buffer_array[i].planes[2].dmaPhyAddr = (vsi_dma_t)cr_buffer[i];
#endif
#if ISP_OUTPUT_SIZE_CBCR
        buffer_array[i].planes[1].dmaPhyAddr = (vsi_dma_t)cbcr_buffer[i];
#endif
    }
}

static void camera_callback(uint32_t event) {
    // event 1: camera capture has completed
    if(event & ARM_CPI_EVENT_CAMERA_CAPTURE_STOPPED)
    {
        g_cam_cb_events |= CAM_CB_EVENT_CAPTURE_STOPPED;
    }
    // all CPI side of errors
    if(event & (ARM_CPI_EVENT_ERR_HARDWARE | \
                ARM_CPI_EVENT_ERR_CAMERA_INPUT_FIFO_OVERRUN | \
                ARM_CPI_EVENT_ERR_CAMERA_OUTPUT_FIFO_OVERRUN))
    {
        g_cam_cb_events |= CAM_CB_EVENT_ERROR;
    }
    // event 2: ISP has received image from CPI
    if (event & ARM_ISP_EVENT_FRAME_VSYNC_DETECTED)
    {
        g_cam_cb_events |= ISP_VSYNC_CB_EVENT;
    }
    // event 3: the ISP has completed image processing
    // and the image is dumped in memory
    if (event & ARM_ISP_MI_EVENT_MP_FRAME_END_DETECTED)
    {
        g_cam_cb_events |= ISP_MI_FRAME_DUMP_EVENT;
    }
    // all ISP side of errors
    if (event & (ARM_ISP_EVENT_DATALOSS_DETECTED | \
                ARM_ISP_EVENT_SIZE_ERR_DETECTED) )
    {
        g_cam_cb_events |= ISP_CB_EVENT_ERROR;
    }
}

int camera_init(void) 
{
    isp_buffer_init();
    int ret = CAMERAdrv->Initialize(camera_callback);
    if (ret != ARM_DRIVER_OK) {
        printf("\r\n Error: CAMERA Initialize failed.\r\n");
        return ret;
    }

    /* Power up Camera peripheral */
    ret = CAMERAdrv->PowerControl(ARM_POWER_FULL);
    if (ret != ARM_DRIVER_OK) {
        printf("\r\n Error: CAMERA Power Up failed.\r\n");
        return ret;
    }

    /* Control configuration for camera controller */
    ret = CAMERAdrv->Control(CPI_CONFIGURE, 0);
    if (ret != ARM_DRIVER_OK) {
        printf("\r\n Error: CPI Configuration failed.\r\n");
        return ret;
    }

    /* Control configuration for camera sensor */
    ret = CAMERAdrv->Control(CPI_CAMERA_SENSOR_CONFIGURE, 0);
    if (ret != ARM_DRIVER_OK) {
        printf("\r\n Error: CAMERA SENSOR Configuration failed.\r\n");
        return ret;
    }

    /*Control configuration for camera events */
    ret = CAMERAdrv->Control(CPI_EVENTS_CONFIGURE, \
            ARM_CPI_EVENT_CAMERA_CAPTURE_STOPPED | \
            ARM_CPI_EVENT_ERR_CAMERA_INPUT_FIFO_OVERRUN | \
            ARM_CPI_EVENT_ERR_CAMERA_OUTPUT_FIFO_OVERRUN | \
            ARM_CPI_EVENT_ERR_HARDWARE);
    if (ret != ARM_DRIVER_OK) {
        printf("\r\n Error: CAMERA SENSOR Event Configuration failed.\r\n");
        return ret;
    }

    for (int i = 0; i < RTE_ISP_BUFFER_COUNT; i++)
    {
        /* Control configuration for camera events */
        ret = CAMERAdrv->Control(ISP_CONTROL_QBUF, (uint32_t) &buffer_array[i]);
        if(ret != ARM_DRIVER_OK)
        {
            printf("\r\n Error: ISP buffer configuration failed.\r\n");
            return ret;
        }
    }
    printf("CPI camera Initialization Success\r\n");
    return ret;
}

int camera_capture(void) 
{
    g_cam_cb_events = CAM_CB_EVENT_NONE;
    int ret = ARM_DRIVER_OK;
    CAM_CB_EVENT callback_event =   CAM_CB_EVENT_CAPTURE_STOPPED    |\
                                    ISP_VSYNC_CB_EVENT              |\
                                    ISP_MI_FRAME_DUMP_EVENT;
    // It is safe to use dummy buffer address because RTE_CPI_AXI_PORT is disabled
    ret = CAMERAdrv->CaptureFrame((uint8_t*)0xABCDABCD);

    if (ret != ARM_DRIVER_OK) {
        printf("\r\n Error: CAMERA Capture Frame failed.\r\n");
        return ret;
    }
    // Wait for capture
    if (ret == ARM_DRIVER_OK) {
        while (g_cam_cb_events != callback_event)
        {
            __WFI();
            if(g_cam_cb_events & (CAM_CB_EVENT_ERROR | ISP_CB_EVENT_ERROR))
            {
                printf("CAM Error: 0b%08b\r\n", g_cam_cb_events);
                return ARM_DRIVER_ERROR;
            }
        }
    }

    ret = CAMERAdrv->Control(ISP_PROCESS_FRAME_END, 0);
    if (ret != ARM_DRIVER_OK){
        printf("\r\n Error: ISP Process Frame End failed.\r\n");
        return ret;
    }
    return ret;
}

