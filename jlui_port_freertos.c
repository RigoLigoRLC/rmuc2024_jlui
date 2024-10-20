
#include "jlui.h"
#include "usart.h"
#include <freertos.h>
#include <semphr.h>

// 注：在使用JLUI库前需要传递一个已经创建好的互斥锁给JLUI库。
// 互斥锁的创建方式如下：
// void *mutex = xSemaphoreCreateMutex();
// JLUI_SetMutexObject(mutex);

// 【用户需要实现此函数】
// 对互斥锁上锁。如果成功获取了锁，返回1；否则返回0。
int JLUI_MutexLock(void *mutex)
{
    return xSemaphoreTake((SemaphoreHandle_t)mutex, 10) == pdTRUE;
    return 1;
}

// 【用户需要实现此函数】
// 对互斥锁解锁。
void JLUI_MutexUnlock(void *mutex)
{
    xSemaphoreGive((SemaphoreHandle_t)mutex);
}

// 【用户需要实现此函数】
// 从串口向裁判系统发送数据函数。注意！从函数返回后，data缓冲区即应视为失效。如要做异步发送，请自行拷贝data。
void JLUI_SendData(const uint8_t *data, size_t len)
{
    // TODO: !!!
    HAL_UART_Transmit(&huart6, data, len, 1000);
}
