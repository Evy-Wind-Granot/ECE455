/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
/* Kernel includes. */
#include "stm32f4xx.h"
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"
#include "../FreeRTOS_Source/include/projdefs.h"

// Task parameters
#define task1_execution 95
#define task1_periodicity PERIODIC
#define task1_period 250

#define task2_execution 150
#define task2_periodicity PERIODIC
#define task2_period 500

#define task3_execution 250
#define task3_periodicity PERIODIC
#define task3_period 750

#ifndef pdTICKS_TO_MS
    #define pdTICKS_TO_MS( xTimeInTicks )    ( ( TickType_t ) ( ( ( uint64_t ) ( xTimeInTicks ) * ( uint64_t ) 1000U ) / ( uint64_t ) configTICK_RATE_HZ ) )
#endif

#define Queue_Length 5
#define Queue_Timeout 20
#define wait 100000

typedef enum task_type {PERIODIC, APERIODIC} task_type;
typedef enum dds_interface_call {RELEASE, COMPLETE, GET_ACTIVE, GET_COMPLETED, GET_OVERDUE} dds_interface_call;

typedef struct dd_task {
    TaskHandle_t t_handle;
    task_type type;
    uint32_t task_id;
    uint32_t release_time;
    uint32_t absolute_deadline;
    uint32_t completion_time;
} dd_task;

typedef struct dd_task_list {
    dd_task task;
    struct dd_task_list* next_task;
} dd_task_list;

typedef struct queueMessage {
    dds_interface_call interface_call;
    dd_task task;
    uint32_t id;
} queueMessage;

void release_dd_task(TaskHandle_t t_handle, task_type type, uint32_t task_id, uint32_t absolute_deadline);
void complete_dd_task(uint32_t task_id);
int get_active_dd_task_list_length(void);
int get_complete_dd_task_list_length(void);
int get_overdue_dd_task_list_length(void);

static void prvSetupHardware(void);
static void Task_1_Handler(void *pvParameters);
static void Task_2_Handler(void *pvParameters);
static void Task_3_Handler(void *pvParameters);
static void Dummy_Task(void *pvParameters);

static void Monitor_Task(TimerHandle_t xTimer);
static void DDTaskGenerator(TimerHandle_t xTimer);
static void DDTaskScheduler(void *pvParameters);

void TM_Delay_Init(void);
void TM_DelayMillis(uint32_t millis);

// Priorities
#define monitorTaskPriority 5
#define taskSchedulerPriority 3
#define activeTaskPriority 2
#define idleTaskPriority 1
#define lowPriority 1

xQueueHandle xQueue_Msg_To_DDS = 0;
xQueueHandle xQueue_Active_Tasks = 0;
xQueueHandle xQueue_Completed_Tasks = 0;
xQueueHandle xQueue_Overdue_Tasks = 0;

TaskHandle_t task1Handle;
TaskHandle_t task2Handle;
TaskHandle_t task3Handle;
TaskHandle_t dummyTaskHandle;
TaskHandle_t taskScheduler;

TimerHandle_t xTimers[3];
TimerHandle_t monitorTimer;

uint32_t timeBegin;
uint32_t delayMultiplier;

int main(void)
{
    TM_Delay_Init();
    prvSetupHardware();

    xQueue_Msg_To_DDS = xQueueCreate(Queue_Length, sizeof(queueMessage));
    xQueue_Completed_Tasks = xQueueCreate(Queue_Length, sizeof(int));
    xQueue_Active_Tasks = xQueueCreate(Queue_Length, sizeof(int));
    xQueue_Overdue_Tasks = xQueueCreate(Queue_Length, sizeof(int));

    xTaskCreate(DDTaskScheduler, "Task Scheduler", configMINIMAL_STACK_SIZE * 2, NULL, taskSchedulerPriority, &taskScheduler);
    xTaskCreate(Task_1_Handler, "Task1", configMINIMAL_STACK_SIZE, NULL, idleTaskPriority, &task1Handle);
    xTaskCreate(Task_2_Handler, "Task2", configMINIMAL_STACK_SIZE, NULL, idleTaskPriority, &task2Handle);
    xTaskCreate(Task_3_Handler, "Task3", configMINIMAL_STACK_SIZE, NULL, idleTaskPriority, &task3Handle);
    xTaskCreate(Dummy_Task, "Dummy Task", configMINIMAL_STACK_SIZE, NULL, activeTaskPriority, &dummyTaskHandle);

    xTimers[0] = xTimerCreate("T1 Timer", pdMS_TO_TICKS(task1_period), pdTRUE, (void *) 1, DDTaskA);
    xTimers[1] = xTimerCreate("T2 Timer", pdMS_TO_TICKS(task2_period), pdTRUE, (void *) 2, DDTaskGenerator);
    xTimers[2] = xTimerCreate("T3 Timer", pdMS_TO_TICKS(task3_period), pdTRUE, (void *) 3, DDTaskGenerator);

    xTimerStart(xTimers[0], 0);
    xTimerStart(xTimers[1], 0);
    xTimerStart(xTimers[2], 0);

    monitorTimer = xTimerCreate("Monitor Timer", pdMS_TO_TICKS(1000), pdTRUE, (void *) 1, Monitor_Task);
    xTimerStart(monitorTimer, 0);

    timeBegin = pdTICKS_TO_MS(xTaskGetTickCount());

    DDTaskGenerator(xTimers[0]);
    DDTaskGenerator(xTimers[1]);
    DDTaskGenerator(xTimers[2]);

    vTaskStartScheduler();
}

void TM_Delay_Init(void) {
    RCC_ClocksTypeDef RCC_Clocks;
    RCC_GetClocksFreq(&RCC_Clocks);
    delayMultiplier = RCC_Clocks.HCLK_Frequency / 7000;
}

void TM_DelayMillis(uint32_t millis) {
    volatile uint32_t millis_copy = millis * delayMultiplier - 10;
    while (millis_copy--);
}

void insertionSort(dd_task_list* listHead, dd_task newItem){
    dd_task_list* curListNode = listHead;
    while (curListNode->next_task != NULL && curListNode->next_task->task.absolute_deadline <= newItem.absolute_deadline) {
        curListNode = curListNode->next_task;
    }
    dd_task_list* newList = pvPortMalloc(sizeof(dd_task_list)); // Changed to FreeRTOS malloc
    newList->task = newItem;
    newList->next_task = curListNode->next_task;
    curListNode->next_task = newList;
}

void taskListUpdatePriorities(dd_task_list* listHead){
    vTaskPrioritySet(task1Handle, idleTaskPriority);
    vTaskPrioritySet(task2Handle, idleTaskPriority);
    vTaskPrioritySet(task3Handle, idleTaskPriority);

    if (listHead->next_task != NULL) {
        vTaskPrioritySet(dummyTaskHandle, lowPriority);
        vTaskPrioritySet(listHead->next_task->task.t_handle, activeTaskPriority);
    } else {
        vTaskPrioritySet(dummyTaskHandle, activeTaskPriority);
    }
}

static void DDTaskScheduler(void *pvParameters){
    queueMessage receivedMessage;
    dd_task_list activeTaskListHead = {{0}, NULL};
    dd_task_list completeTaskListHead = {{0}, NULL};
    dd_task_list overdueTaskListHead = {{0}, NULL};
    int activeTaskListLength = 0;
    int completeTaskListLength = 0;
    int overdueTaskListLength = 0;
    uint32_t currentTime;

    while(1){
        if (xQueueReceive(xQueue_Msg_To_DDS, &receivedMessage, portMAX_DELAY)){
            switch(receivedMessage.interface_call) {
                case RELEASE:
                    currentTime = pdTICKS_TO_MS(xTaskGetTickCount()) - timeBegin;
                    receivedMessage.task.release_time = currentTime;
                    insertionSort(&activeTaskListHead, receivedMessage.task);
                    activeTaskListLength++;
                    taskListUpdatePriorities(&activeTaskListHead);
                    break;
                case COMPLETE:
                    currentTime = pdTICKS_TO_MS(xTaskGetTickCount()) - timeBegin;
                    if (activeTaskListHead.next_task != NULL) {
                        dd_task_list* prevList = &activeTaskListHead;
                        dd_task_list* curList = activeTaskListHead.next_task;
                        while (curList != NULL && curList->task.task_id != receivedMessage.id){
                            prevList = curList;
                            curList = curList->next_task;
                        }
                        if(curList != NULL) {
                            prevList->next_task = curList->next_task;
                            curList->task.completion_time = currentTime;
                            activeTaskListLength--;
                            if(curList->task.absolute_deadline >= curList->task.completion_time){
                                curList->next_task = completeTaskListHead.next_task;
                                completeTaskListHead.next_task = curList;
                                completeTaskListLength++;
                            } else {
                                curList->next_task = overdueTaskListHead.next_task;
                                overdueTaskListHead.next_task = curList;
                                overdueTaskListLength++;
                            }
                            taskListUpdatePriorities(&activeTaskListHead);
                            printf("ID: %d, Rel: %d, Done: %d, Dead: %d\n", (int)curList->task.task_id, (int)curList->task.release_time, (int)curList->task.completion_time, (int)curList->task.absolute_deadline);
                        }
                    }
                    break;
                case GET_ACTIVE:
                    xQueueSend(xQueue_Active_Tasks, &activeTaskListLength, Queue_Timeout);
                    break;
                case GET_COMPLETED:
                    xQueueSend(xQueue_Completed_Tasks, &completeTaskListLength, Queue_Timeout);
                    break;
                case GET_OVERDUE:
                    xQueueSend(xQueue_Overdue_Tasks, &overdueTaskListLength, Queue_Timeout);
                    break;
            }
        }
    }
}

static void DDTaskGenerator(TimerHandle_t xTimer){
    uint32_t timerId = (uint32_t) pvTimerGetTimerID(xTimer);
    uint32_t deadline = pdTICKS_TO_MS(xTaskGetTickCount()) - timeBegin;

    if (timerId == 1){
        release_dd_task(task1Handle, task1_periodicity, 1, task1_period + deadline);
    } else if (timerId == 2){
        release_dd_task(task2Handle, task2_periodicity, 2, task2_period + deadline);
    } else if (timerId == 3){
        release_dd_task(task3Handle, task3_periodicity, 3, task3_period + deadline);
    }
}

void release_dd_task(TaskHandle_t t_handle, task_type type, uint32_t task_id, uint32_t absolute_deadline){
    dd_task new_task = {t_handle, type, task_id, 0, absolute_deadline, 0};
    queueMessage new_message = {RELEASE, new_task, 0};
    xQueueSend(xQueue_Msg_To_DDS, &new_message, Queue_Timeout);
}

void complete_dd_task(uint32_t task_id){
    queueMessage new_message = {COMPLETE, {NULL}, task_id};
    xQueueSend(xQueue_Msg_To_DDS, &new_message, Queue_Timeout);
}

int get_active_dd_task_list_length(void){
    queueMessage msg = {GET_ACTIVE, {NULL}, 0};
    int active = 0;
    xQueueSend(xQueue_Msg_To_DDS, &msg, Queue_Timeout);
    xQueueReceive(xQueue_Active_Tasks, &active, Queue_Timeout);
    return active;
}

int get_complete_dd_task_list_length(void){
    queueMessage msg = {GET_COMPLETED, {NULL}, 0};
    int complete = 0;
    xQueueSend(xQueue_Msg_To_DDS, &msg, Queue_Timeout);
    xQueueReceive(xQueue_Completed_Tasks, &complete, Queue_Timeout);
    return complete;
}

int get_overdue_dd_task_list_length(void){
    queueMessage msg = {GET_OVERDUE, {NULL}, 0};
    int overdue = 0;
    xQueueSend(xQueue_Msg_To_DDS, &msg, Queue_Timeout);
    xQueueReceive(xQueue_Overdue_Tasks, &overdue, Queue_Timeout);
    return overdue;
}

static void Monitor_Task(TimerHandle_t xTimer) {
    int active = get_active_dd_task_list_length();
    int complete = get_complete_dd_task_list_length();
    int overdue = get_overdue_dd_task_list_length();
    printf("Monitor: Active: %d, Done: %d, Overdue: %d\n", active, complete, overdue);
}

static void Task_1_Handler(void *pvParameters) {
    while(1) {
        TM_DelayMillis(task1_execution);
        complete_dd_task(1);
    }
}

static void Task_2_Handler(void *pvParameters) {
    while(1) {
        TM_DelayMillis(task2_execution);
        complete_dd_task(2);
    }
}

static void Task_3_Handler(void *pvParameters) {
    while(1) {
        TM_DelayMillis(task3_execution);
        complete_dd_task(3);
    }
}

static void Dummy_Task(void *pvParameters) {
    while(1) {
        for(volatile int i = 0; i < wait; ++i);
    }
}

static void prvSetupHardware(void) {
    NVIC_SetPriorityGrouping(0);
}

// Error hooks
void vApplicationMallocFailedHook(void) { for(;;); }
void vApplicationStackOverflowHook(xTaskHandle pxTask, signed char *pcTaskName) { for(;;); }
void vApplicationIdleHook(void) { }
