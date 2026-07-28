#include "jpeg_app.h"

extern ARM_DRIVER_JPEG Driver_JPEG;
static ARM_DRIVER_JPEG *JPEGdrv = &Driver_JPEG;


uint8_t out_buffer[OUT_BUF_SIZE/*RTE_JPEG_ENCODING_WIDTH * RTE_JPEG_ENCODING_HEIGHT*/] \
    __attribute__((section(".bss.jpeg_enc_buf"), aligned(512)));

// declare y_buffer object here.
#if (ISP_OUTPUT_SIZE_Y)
extern uint8_t y_buffer[RTE_ISP_BUFFER_COUNT][ISP_OUTPUT_SIZE_Y];
#endif
// declare cbcr_bufer object.
#if (ISP_OUTPUT_SIZE_CBCR)
extern uint8_t cbcr_buffer[RTE_ISP_BUFFER_COUNT][ISP_OUTPUT_SIZE_CBCR];
#endif

// Indicator pin for power measurement.
extern ARM_DRIVER_GPIO ARM_Driver_GPIO_(0);
static ARM_DRIVER_GPIO* indicator_port = &ARM_Driver_GPIO_(0);
#define BOARD_INDICATOR_GPIO_PIN 0

typedef enum {
	JPEG_CB_EVENT_NONE              = 0,	
    JPEG_FRAME_READY_CB_EVENT       = (1 << 2),         // SW_ENC_FRAME_RDY_STATUS
    JPEG_BUSS_ERROR_CB_EVENT        = (1 << 3),         // SW_ENC_BUS_ERROR_STATUS
    JPEG_SW_RESET_CB_EVENT          = (1 << 4),         // SW_ENC_SW_RESET
    JPEG_BUF_FULL_CB_EVENT          = (1 << 5),         // SW_ENC_BUFFER_FULL
    JPEG_TIME_OUT_CB_EVENT          = (1 << 6),         // SW_ENC_TIMEOUT
    JPEG_FUSE_ERROR_CB_EVENT        = (1 << 9),         // SW_ENC_IRQ_FUSE_ERROR
}JPEG_CB_EVENT;

JPEG_CB_EVENT jpeg_event_flag = 0;

void jpeg_callback(uint32_t event)
{
    // event 4: JPEG encoder has completed an image (event 1 to 3 are camera/ISP events)
    if(event & ARM_JPEG_EVENT_FRAME_READY)
    {
        jpeg_event_flag |= JPEG_FRAME_READY_CB_EVENT;
    }
    // JPEG buffer full error
    if(event & ARM_JPEG_EVENT_BUFFER_FULL)
    {
        jpeg_event_flag |= JPEG_BUF_FULL_CB_EVENT;
    }
}

int jpeg_enc_init(void)
{
    int ret = 0;
    ret = JPEGdrv->Initialize(jpeg_callback);
    if(ret != ARM_DRIVER_OK)
    {
        printf("JPEG Init fails.\r\n");
        return ret;
    }

    JPEGdrv->PowerControl(ARM_POWER_FULL);
    if(ret != ARM_DRIVER_OK)
        printf("JPEG PowerControl fails.\r\n");

    // Initialize JPEG configuration instance.
    ARM_JPEG_FORMAT fmt;
    fmt.pixel_format        = ARM_JPEG_FORMAT_NV12;
    fmt.pitch               = JPEG_WIDTH;
    fmt.width               = JPEG_WIDTH;
    fmt.height              = JPEG_HEIGHT;
    ret = JPEGdrv->Control(ARM_JPEG_CTRL_SET_FORMAT, (uint32_t)&fmt);
    if(ret != ARM_DRIVER_OK)
        printf("JPEG Control fails.\r\n");

    
    // Configure quality factor.
    ret = JPEGdrv->Control(ARM_JPEG_CTRL_SET_QUALITY, 80);
    if(ret != ARM_DRIVER_OK)
        printf("JPEG Control fails.\r\n");

    indicator_port->Initialize(BOARD_INDICATOR_GPIO_PIN, NULL);
    indicator_port->PowerControl(BOARD_INDICATOR_GPIO_PIN, ARM_POWER_FULL);
    indicator_port->SetDirection(BOARD_INDICATOR_GPIO_PIN, GPIO_PIN_DIRECTION_OUTPUT);
    indicator_port->SetValue(BOARD_INDICATOR_GPIO_PIN, GPIO_PIN_OUTPUT_STATE_LOW);

    return ret;
}


// define JPEG output buffer object
ARM_JPEG_BUFFER     jpeg_buffer_output  =
{
    .buf    = &out_buffer,
    // NV12 format, x1.5
    .size   = JPEG_WIDTH*JPEG_HEIGHT*3/2,
};
int jpeg_compress(void)
{
    uint32_t ret = 0;
    uint32_t cycle_jpeg_comp = 0;
    static uint32_t count = 0;     // encoding count
    jpeg_event_flag = JPEG_CB_EVENT_NONE;

    // define JPEG input buffer object
    static ARM_JPEG_INPUT_BUFFER        jpeg_buffer_input;
    jpeg_buffer_input.num_planes        = 3;
    jpeg_buffer_input.plane[0]          = &y_buffer[0][0];
    jpeg_buffer_input.plane[1]          = &cbcr_buffer[0][0];
    jpeg_buffer_input.plane_pitch[0]    = JPEG_WIDTH;
    jpeg_buffer_input.plane_pitch[1]    = JPEG_WIDTH;

    // reset the output buffer size to avoid buffer full error
    jpeg_buffer_output.size             = JPEG_WIDTH*JPEG_HEIGHT*3/2;
    // Turn on GPIO indicator and start profiling.
    // This bracketing includes the CPU time to generate the header.
    indicator_port->SetValue(BOARD_INDICATOR_GPIO_PIN, GPIO_PIN_OUTPUT_STATE_HIGH);
    cycle_jpeg_comp = alifs_profile_start();

    // Start the encoding.
    ret = JPEGdrv->Start(&jpeg_buffer_input, &jpeg_buffer_output);
    if(ret != ARM_DRIVER_OK){
        printf("JPEG Control fails.\r\n");
        return ARM_DRIVER_ERROR;
    }
    
    // Wait for encoding to complete.
    while (jpeg_event_flag != JPEG_FRAME_READY_CB_EVENT)
    {
        __WFI();
        if(jpeg_event_flag & JPEG_BUF_FULL_CB_EVENT){
            printf("<<JPEG buffer full error.\r\n");
            return ARM_DRIVER_ERROR;
        }
    }
    // Turn off encoding indicator GPIO and stop profiling
    cycle_jpeg_comp = alifs_profile_end(cycle_jpeg_comp);
    indicator_port->SetValue(BOARD_INDICATOR_GPIO_PIN, GPIO_PIN_OUTPUT_STATE_LOW);
    return ret;
}
