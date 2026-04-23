// GPIO pin definitions
#define YEL GPIO_Pin_1
#define GRN GPIO_Pin_2
#define RED GPIO_Pin_0
#define ADC_PIN GPIO_Pin_3
#define LED_YEL GPIOC, GPIO_Pin_1
#define LED_GRN GPIOC, GPIO_Pin_2
#define LED_RED GPIOC, GPIO_Pin_0
#define POT GPIOC, GPIO_Pin_3

// shift register pin definitions
#define CLR_PIN GPIO_Pin_8  // reset
#define CLK_PIN GPIO_Pin_7  // clock
#define DATA_PIN GPIO_Pin_6 // data
#define SHIFT_PORT GPIOC

/*
 * Hardware specific clock configuration
 * that was not already performed before main() was called.
 */
static void gpio_init(void);

/*
 * The queue send and receive tasks as described in the comments at the top of
 * this file.
 */
static void flow_adjust_Task(void *pvParameters);
static void traffic_gen_Task(void *pvParameters);
static void light_controller_Task(void *pvParameters);
static void display_Task(void *pvParameters);

#define CAR_STEP_MS 800U
#define DISPLAY_REFRESH_MS 800U
#define FLOW_SAMPLE_MS 100U
#define LIGHT_YELLOW_MS 1500U
#define LIGHT_GREEN_MIN_MS 5000U
#define LIGHT_GREEN_MAX_MS 10000U
#define LIGHT_RED_MIN_MS 5000U
#define LIGHT_RED_MAX_MS 10000U

/*--------------------------Main-----------------------------*/

int main(void)
{

    data_Rst();
    /* Check if any of the queues or timer creation failed */
    if (light_timer == NULL || flow_to_traffic_q == NULL || flow_to_light_q == NULL ||
        light_to_traffic_q == NULL || light_to_display_q == NULL ||
        road_to_display_q == NULL || light_event_q == NULL)
    {
        for (;;)
            ;
    }

    /* Create tasks after creating queues and timers to ensure they are available when the tasks start running */
    xTaskCreate(flow_adjust_Task, "Flow_Adjust", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(traffic_gen_Task, "Traffic_Gen", configMINIMAL_STACK_SIZE, NULL, 1, NULL);
    xTaskCreate(light_controller_Task, "Light_Controller", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(display_Task, "Display", configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    /* Start the tasks and timer running. */
    vTaskStartScheduler();

    return 0;
}

/*----------------------Helper Functions---------------------*/

// Initialize hardware
static void gpio_init()
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE); // enable clock for portC
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);  // enable clock for ADC1

    // LED's
    GPIO_InitTypeDef GPIO_Output_Config;
    GPIO_StructInit(&GPIO_Output_Config);                                           // reset all members of the struct to default values
    GPIO_Output_Config.GPIO_Pin = (YEL | GRN | RED | CLR_PIN | CLK_PIN | DATA_PIN); // we want to control the LEDs and shift register, so we need to set the corresponding pins
    GPIO_Output_Config.GPIO_Mode = GPIO_Mode_OUT;                                   // we want to control the LEDs, so we need output mode
    GPIO_Output_Config.GPIO_PuPd = GPIO_PuPd_NOPULL;                                // no pull-up or pull-down resistors
    GPIO_Output_Config.GPIO_Speed = GPIO_Speed_2MHz;                                // the speed of the output signal is not important, so we set it to the lowest
    GPIO_Output_Config.GPIO_OType = GPIO_OType_PP;                                  // push/pull
    GPIO_Init(GPIOC, &GPIO_Output_Config);

    // ADC
    GPIO_InitTypeDef GPIO_ADC_Config;
    GPIO_StructInit(&GPIO_ADC_Config);            // reset all members of the struct to default values
    GPIO_ADC_Config.GPIO_Pin = GPIO_Pin_3;        // we want to read the value of the potentiometer, which is connected to pin 3 of port C
    GPIO_ADC_Config.GPIO_Mode = GPIO_Mode_AN;     // we want to read the value of the potentiometer, so we need analog mode
    GPIO_ADC_Config.GPIO_PuPd = GPIO_PuPd_NOPULL; // no pull-up or pull-down resistors
    GPIO_ADC_Config.GPIO_Speed = GPIO_Speed_2MHz; // the speed of the output signal is not important, so we set it to the lowest
    GPIO_Init(GPIOC, &GPIO_ADC_Config);
    ADC_CommonInitTypeDef ADC_Common;                               // we need to configure the common ADC settings, such as the clock and the mode of operation
    ADC_CommonStructInit(&ADC_Common);                              // reset all members of the struct to default values
    ADC_Common.ADC_Mode = ADC_Mode_Independent;                     // use ADC1 only
    ADC_Common.ADC_Prescaler = ADC_Prescaler_Div4;                  // reduce ADC clock
    ADC_Common.ADC_DMAAccessMode = ADC_DMAAccessMode_Disabled;      // no DMA
    ADC_Common.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles; // default delay
    ADC_CommonInit(&ADC_Common);

    ADC_InitTypeDef ADC_Config;
    ADC_StructInit(&ADC_Config);                                         // reset all members of the struct to default
    ADC_Config.ADC_ContinuousConvMode = DISABLE;                         // single conversions on demand
    ADC_Config.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None; // software trigger only
    ADC_Config.ADC_DataAlign = ADC_DataAlign_Right;                      // we want the data to be right-aligned, so we set the data alignment to right
    ADC_Init(ADC1, &ADC_Config);
    ADC_Cmd(ADC1, ENABLE);                                                     // enable the ADC
    ADC_RegularChannelConfig(ADC1, ADC_Channel_13, 1, ADC_SampleTime_3Cycles); // we want to read the value of the potentiometer, channel 13 of the ADC, so we need to configure the regular channel
}

// Shift Register Data High
void data_High()
{
    GPIO_ResetBits(SHIFT_PORT, CLK_PIN);
    GPIO_SetBits(SHIFT_PORT, CLR_PIN);
    GPIO_SetBits(SHIFT_PORT, DATA_PIN);
    // busy-wait ~1us instead of 10ms vTaskDelay
    volatile uint32_t c = (SystemCoreClock / 4000000U);
    while (c--)
    {
        __NOP();
    }
    GPIO_SetBits(SHIFT_PORT, CLK_PIN);
    c = (SystemCoreClock / 4000000U);
    while (c--)
    {
        __NOP();
    }
    GPIO_ResetBits(SHIFT_PORT, CLK_PIN);
    c = (SystemCoreClock / 4000000U);
    while (c--)
    {
        __NOP();
    }
}

void data_Low()
{
    GPIO_ResetBits(SHIFT_PORT, CLK_PIN);
    GPIO_SetBits(SHIFT_PORT, CLR_PIN);
    GPIO_ResetBits(SHIFT_PORT, DATA_PIN);
    volatile uint32_t c = (SystemCoreClock / 4000000U);
    while (c--)
    {
        __NOP();
    }
    GPIO_SetBits(SHIFT_PORT, CLK_PIN);
    c = (SystemCoreClock / 4000000U);
    while (c--)
    {
        __NOP();
    }
    GPIO_ResetBits(SHIFT_PORT, CLK_PIN);
    c = (SystemCoreClock / 4000000U);
    while (c--)
    {
        __NOP();
    }
}
// Turn off all LEDs on the shift register
void data_Rst()
{
    GPIO_ResetBits(SHIFT_PORT, CLR_PIN); // drives the reset line low
    delay(10);                           // delay 10ms
    GPIO_SetBits(SHIFT_PORT, CLR_PIN);   // set reset to high
}

/* Writes car data to the shift register.
The data is a 19-bit value, where each bit represents a car in the 3 lanes.
The MSB represents the oldest car, and the LSB represents the newest car.
A bit value of 1 means there is a car, and a bit value of 0 means there is no car */
void screen_Write(uint32_t data)
{
    // data_Rst(); // clear display
    char car_To_Send = 0x0;

    for (int i = 18; i >= 0; i--)
    { // MSB first, so we start from the highest bit and shift down to the lowest bit
        car_To_Send = 0x1 & (data >> i);
        if (car_To_Send == 0x1)
        {
            data_High();
        }
        else
        {
            data_Low();
        }
    }
}

// Reads the ADC
static uint16_t read_pot_adc(void)
{
    /* Start a single ADC conversion and return the 12-bit value. */
    ADC_SoftwareStartConv(ADC1);
    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET)
    { // wait for conversion to complete
    }
    return (uint16_t)ADC_GetConversionValue(ADC1);
}

// Steps the cars forward and handles stopping at the red light
static uint32_t road_step(uint32_t road_bits, uint8_t light_state)
{
    /* Shift cars forward by one LED per tick. */
    uint32_t moved = 0U;

    if (light_state == LIGHT_GRN)
    {
        moved = (road_bits << 1) & ROAD_MASK; // advance every car one position, drop anything past the road length
        return moved;
    }

    /* When yellow or red, cars must queue at the stop line. */
    uint32_t lower_mask = (1U << (STOP_LINE_INDEX + 1U)) - 1U; // bits from 0..stop line
    uint32_t upper_mask = ROAD_MASK & ~lower_mask;             // bits past the stop line
    uint32_t lower = road_bits & lower_mask;
    uint32_t upper = (road_bits & upper_mask) << 1; // cars already past the line keep moving

    /* Move cars forward only if the next position is free (single-lane queue). */
    for (int i = (int)STOP_LINE_INDEX - 1; i >= 0; i--)
    {
        uint32_t cur_bit = 1U << (uint32_t)i;
        uint32_t next_bit = 1U << (uint32_t)(i + 1);
        if ((lower & cur_bit) != 0U && (lower & next_bit) == 0U)
        {
            lower &= ~cur_bit;
            lower |= next_bit;
        }
    }

    moved = (upper | lower) & ROAD_MASK;
    return moved;
}

// RNG algorithm
static uint32_t rng_next(uint32_t *state)
{
    /* Simple xor shift RNG for car spawning decisions. */
    uint32_t x = *state; // read the current state
    x ^= x << 13;        // xor shift operations to produce the next state
    x ^= x >> 17;        // these operations are designed to produce a good distribution of random values while being computationally efficient
    x ^= x << 5;         // the specific shift values (13, 17, 5) are commonly used in xor shift RNGs and have been found to work well empirically
    *state = x;          // write the new state back to the variable pointed to by state
    return x;
}

// Called when the light timer expires, and it posts an event to the light controller task to indicate that it's time to change the light state.
static void light_timer_callback(TimerHandle_t timer)
{
    /* Post an event to advance the light state. */
    uint8_t event = 1;
    (void)timer;
    // we don't actually need to send any data with the event, since the light controller just needs to know that the timer has expired
    // and can use its existing state to determine the next light state, but we need to send something since the queue doesn't support sending empty messages
    if (light_event_q != NULL)
    {
        xQueueOverwrite(light_event_q, &event);
    }
}

void delay(uint32_t ms)
{
    if (ms == 0U)
    {
        return;
    }

    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        /* Yield to other tasks when the scheduler is running. */
        TickType_t ticks = pdMS_TO_TICKS(ms);
        if (ticks > 0U)
        {
            vTaskDelay(ticks);
            return;
        }
    }

    /* Fallback for early init or sub-tick delays: simple CPU busy-wait. */
    volatile uint32_t count = (SystemCoreClock / 8000U) * ms;
    while (count-- != 0U)
    {
        __NOP();
    }
}

/*------------------------Tasks------------------------------*/

/* Reads pot value and adjusts traffic flow based on it */
static void flow_adjust_Task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount(); // initialize the last wake time variable for vTaskDelayUntil
    uint16_t flow_value = 0;

    (void)pvParameters; // avoid unused parameter warning, we don't need to pass any parameters to this task

    for (;;)
    { // Run forever until task is stopped
        flow_value = read_pot_adc();
        printf("Flow Adjustment Task Running -- ");
        printf("Flow Value: %u\n", flow_value);

        if (flow_to_traffic_q != NULL)
        { // send to traffic generator
            xQueueOverwrite(flow_to_traffic_q, &flow_value);
        }
        if (flow_to_light_q != NULL)
        { // send to light controller
            xQueueOverwrite(flow_to_light_q, &flow_value);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FLOW_SAMPLE_MS));
    }
}

/* random traffic generator */
static void traffic_gen_Task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    uint16_t flow_value = 0;
    uint32_t car_bits = 0;           // bitfield representing the presence of cars on the road, where each bit is a position on the road and a value of 1 means there is a car at that position
    uint32_t rng_state = 0x1234ABCD; // initial state for the random number generator, can be any non-zero value
    uint8_t light_state = LIGHT_RED;
    uint8_t spawn_countdown = 1;

    (void)pvParameters;

    for (;;)
    {
        uint16_t flow_update = 0;
        uint8_t light_update = 0;
        // check for updates from flow adjuster and light controller, but don't block if they haven't sent anything
        if (flow_to_traffic_q != NULL && xQueueReceive(flow_to_traffic_q, &flow_update, 0) == pdPASS)
        {
            flow_value = flow_update;
        }
        // if we haven't received an update from the light controller, we can just use the last known state, so we don't block waiting for an update
        if (light_to_traffic_q != NULL && xQueueReceive(light_to_traffic_q, &light_update, 0) == pdPASS)
        {
            light_state = light_update;
        }

        car_bits = road_step(car_bits, light_state); // move cars one step

        if (spawn_countdown > 0)
        {
            spawn_countdown--;
        }

        if (spawn_countdown == 0)
        {
            uint32_t roll = rng_next(&rng_state) & FLOW_MAX_VALUE;
            if (flow_value >= (FLOW_MAX_VALUE - 1U) || roll < flow_value)
            {
                if ((car_bits & 0x1U) == 0U)
                {
                    car_bits |= 0x1U;

                    printf("Car spawned! Road: 0x%05lX\n", (unsigned long)car_bits);
                }
            }

            /* Guard: if flow exceeds max, clamp it before subtraction */
            uint16_t clamped_flow = (flow_value > FLOW_MAX_VALUE) ? FLOW_MAX_VALUE : flow_value;
            spawn_countdown = (uint8_t)(1U + ((FLOW_MAX_VALUE - clamped_flow) * 5U) / FLOW_MAX_VALUE);
            if (spawn_countdown == 0U)
            {
                spawn_countdown = 1U;
            }
        }

        if (road_to_display_q != NULL)
        {
            xQueueOverwrite(road_to_display_q, &car_bits);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CAR_STEP_MS));
    }
}

/* controls the traffic light */
static void light_controller_Task(void *pvParameters)
{
    uint16_t flow_value = 0;
    uint8_t light_state = LIGHT_RED; // initial light state is red
    uint8_t timer_event = 0;

    (void)pvParameters; // avoid unused parameter warning, we don't need to pass any parameters to this task
    // Initialize display and traffic generator with initial light state
    if (light_to_display_q != NULL)
    {
        xQueueOverwrite(light_to_display_q, &light_state);
    }
    // Traffic generator needs to know the initial light state to properly hold cars at the stop line, so we send it the initial state as well
    if (light_to_traffic_q != NULL)
    {
        xQueueOverwrite(light_to_traffic_q, &light_state);
    }
    // Start the light timer with the initial red duration
    if (light_timer != NULL)
    { // start with red interval
        xTimerChangePeriod(light_timer, pdMS_TO_TICKS(LIGHT_RED_MAX_MS), 0);
        xTimerStart(light_timer, 0);
    }

    for (;;)
    {
        uint16_t flow_update = 0;
        if (flow_to_light_q != NULL && xQueueReceive(flow_to_light_q, &flow_update, 0) == pdPASS)
        {
            flow_value = flow_update;
        }
        // wait for a timer event to change the light state, but don't block waiting for flow updates
        if (light_event_q != NULL && xQueueReceive(light_event_q, &timer_event, portMAX_DELAY) == pdPASS)
        {
            /* Map flow to green/red durations. */
            uint32_t green_ms = LIGHT_GREEN_MIN_MS +
                                ((uint32_t)flow_value * (LIGHT_GREEN_MAX_MS - LIGHT_GREEN_MIN_MS)) / FLOW_MAX_VALUE;
            uint32_t red_ms = LIGHT_RED_MAX_MS -
                              ((uint32_t)flow_value * (LIGHT_RED_MAX_MS - LIGHT_RED_MIN_MS)) / FLOW_MAX_VALUE;

            if (light_state == LIGHT_RED)
            { // red -> green
                light_state = LIGHT_GRN;
                if (light_timer != NULL)
                {
                    xTimerChangePeriod(light_timer, pdMS_TO_TICKS(green_ms), 0);
                    xTimerStart(light_timer, 0);
                }
            }
            else if (light_state == LIGHT_GRN)
            { // green -> yellow
                light_state = LIGHT_YEL;
                if (light_timer != NULL)
                {
                    xTimerChangePeriod(light_timer, pdMS_TO_TICKS(LIGHT_YELLOW_MS), 0);
                    xTimerStart(light_timer, 0);
                }
            }
            else
            { // yellow -> red
                light_state = LIGHT_RED;
                if (light_timer != NULL)
                {
                    xTimerChangePeriod(light_timer, pdMS_TO_TICKS(red_ms), 0);
                    xTimerStart(light_timer, 0);
                }
            }

            uint8_t stale_event;
            while (xQueueReceive(light_event_q, &stale_event, 0) == pdPASS)
            {
            }

            // Update display and traffic generator with new light state
            if (light_to_display_q != NULL)
            {
                xQueueOverwrite(light_to_display_q, &light_state);
            }
            // Traffic generator needs to know the light state to properly hold cars at the stop line, so we send it the updated state as well
            if (light_to_traffic_q != NULL)
            {
                xQueueOverwrite(light_to_traffic_q, &light_state);
            }
        }
    }
}

/* Outputs to LEDs */
static void display_Task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t road_bits = 0;
    uint8_t light_state = LIGHT_RED;

    (void)pvParameters; // avoid unused parameter warning, we don't need to pass any parameters to this task

    for (;;)
    {
        uint32_t road_update = 0;
        uint8_t light_update = 0;
        // check for updates from traffic generator and light controller, but don't block if they haven't sent anything,
        // since we want to refresh the display at a consistent rate regardless of whether we've received updates
        if (road_to_display_q != NULL && xQueueReceive(road_to_display_q, &road_update, 0) == pdPASS)
        {
            road_bits = road_update;
        }
        // if we haven't received an update from the light controller, we can just use the last known state, so we don't block waiting for an update
        if (light_to_display_q != NULL && xQueueReceive(light_to_display_q, &light_update, 0) == pdPASS)
        {
            light_state = light_update;
        }

        GPIO_ResetBits(LED_RED);
        GPIO_ResetBits(LED_YEL);
        GPIO_ResetBits(LED_GRN);

        if (light_state == LIGHT_GRN)
        { // set traffic light LEDs
            GPIO_SetBits(LED_GRN);
        }
        else if (light_state == LIGHT_YEL)
        {
            GPIO_SetBits(LED_YEL);
        }
        else
        {
            GPIO_SetBits(LED_RED);
        }

        screen_Write(road_bits & ROAD_MASK); // push road state to shift register

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DISPLAY_REFRESH_MS)); // refresh display at a consistent rate
    }
}

/*-----------------------------------------------------------*/

/*---------------------------Other---------------------------*/

void vApplicationMallocFailedHook(void)
{
    /* The malloc failed hook is enabled by setting
    configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.

    Called if a call to pvPortMalloc() fails because there is insufficient
    free memory available in the FreeRTOS heap.  pvPortMalloc() is called
    internally by FreeRTOS API functions that create tasks, queues, software
    timers, and semaphores.  The size of the FreeRTOS heap is set by the
    configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
    for (;;)
        ;
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook(xTaskHandle pxTask, signed char *pcTaskName)
{
    (void)pcTaskName;
    (void)pxTask;

    /* Run time stack overflow checking is performed if
    configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
    function is called if a stack overflow is detected.  pxCurrentTCB can be
    inspected in the debugger if the task name passed into this function is
    corrupt. */
    for (;;)
        ;
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook(void)
{
    volatile size_t xFreeStackSpace;

    /* The idle task hook is enabled by setting configUSE_IDLE_HOOK to 1 in
    FreeRTOSConfig.h.

    This function is called on each cycle of the idle task.  In this case it
    does nothing useful, other than report the amount of FreeRTOS heap that
    remains unallocated. */
    xFreeStackSpace = xPortGetFreeHeapSize();

    if (xFreeStackSpace > 100)
    {
        /* By now, the kernel has allocated everything it is going to, so
        if there is a lot of heap remaining unallocated then
        the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
        reduced accordingly. */
    }
}
/*-----------------------------------------------------------*/
