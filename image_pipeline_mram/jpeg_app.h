#ifndef __JPEG_APP_H__
#define __JPEG_APP_H__

#include <stdio.h>
#include <stdint.h>
#include "soc.h"
#include "RTE_Device.h"
#include "isp_header.h"
#include "alifs_profile.h"
#include "Driver_JPEG.h"
#include "Driver_IO.h"
#include "Driver_MRAM.h"
/* MRAM Driver */
extern ARM_DRIVER_MRAM  Driver_MRAM;
static ARM_DRIVER_MRAM *MRAM_drv = &Driver_MRAM;

#define OUT_BUF_SIZE            1400000
#define JPEG_QUALITY_FACTOR     70

#define JPEG_WIDTH          RTE_ISP_OUTPUT_WIDTH
#define JPEG_HEIGHT         RTE_ISP_OUTPUT_HEIGHT

void jpeg_callback(uint32_t event);
int jpeg_enc_init(void);
int jpeg_compress(void);
int mram_init(void);

#endif