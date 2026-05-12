#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

BaseType_t xTaskCreateRestrictedPinnedToCore(const TaskParameters_t *const pxTaskDefinition,
                                             TaskHandle_t *pxCreatedTask,
                                             const BaseType_t xCoreID)
{
    if (pxTaskDefinition == NULL || pxTaskDefinition->pvTaskCode == NULL || pxTaskDefinition->puxStackBuffer == NULL) {
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }

#if (configSUPPORT_STATIC_ALLOCATION == 1)
    StaticTask_t *task_buffer = pvPortMalloc(sizeof(StaticTask_t));
    if (task_buffer == NULL) {
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(pxTaskDefinition->pvTaskCode,
                                                      pxTaskDefinition->pcName,
                                                      pxTaskDefinition->usStackDepth,
                                                      pxTaskDefinition->pvParameters,
                                                      pxTaskDefinition->uxPriority & ~portPRIVILEGE_BIT,
                                                      pxTaskDefinition->puxStackBuffer,
                                                      task_buffer,
                                                      xCoreID);
    if (task == NULL) {
        vPortFree(task_buffer);
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }

    if (pxCreatedTask != NULL) {
        *pxCreatedTask = task;
    }
    return pdPASS;
#else
    return xTaskCreatePinnedToCore(pxTaskDefinition->pvTaskCode,
                                   pxTaskDefinition->pcName,
                                   pxTaskDefinition->usStackDepth,
                                   pxTaskDefinition->pvParameters,
                                   pxTaskDefinition->uxPriority & ~portPRIVILEGE_BIT,
                                   pxCreatedTask,
                                   xCoreID);
#endif
}
