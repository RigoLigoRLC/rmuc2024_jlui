
//
// 在Keil ARMCC5中编译时，需要在编译选项中添加 --cpp11 为本文件启用C++11支持
//

#include "jlui.h"
#include <algorithm>
#include <assert.h>
#include <functional>
#include <stdbool.h>
#include <string.h>


// 图形元素总个数，每个图形元素需要吃掉16字节内存，请根据自身具体需要修改！
#ifndef JLUI_TOTAL_COUNT
#define JLUI_TOTAL_COUNT 30

#endif

// 是否开启所有断言，鉴于电控一般不喜欢开优化，提供一个关闭来提高性能的选项
// 如果不想开启所有断言，注释掉下面这一行
#define JLUI_ENABLE_ASSERT

// 图形项列表，保存了所有已经创建的图形项。
static JluiUiObject jluiObjectList[JLUI_TOTAL_COUNT];

// 发送缓冲区，按最大的七个图形的包尺寸（子内容105字节，加上包头尾9字节、子内容元数据6字节）计算
static uint8_t jluiTxBuffer[120];

// 每次10Hz Tick时，将从这个索引处开始查找脏位置位的图形项并将尽可能多个图形项更新到服务器。
static uint32_t jluiScanOffset = 0;

// 如果遇到需要更新字符串的情况，字符串会占用掉整个包的带宽。所以，采用字符串和其余图形项交替更新的方式。
// 存在脏字符串时，10Hz Tick照常扫描对象列表。但是一旦遇到脏字符串对象，就将其跳过，记录此脏字符串的索引，
// 设置标志位表明下次Tick时仅更新一个字符串对象，并在此次Tick中继续扫描其他脏图形项并将它们打包到大包中发出。
// 下次Tick时，看到标志位被置位，就仅构造此字符串的包并发出；然后，向后继续扫描存在哪些脏字符串对象（一直到
// 索引不再小于jluiScanOffset为止）；如果还找到了其他字符串对象，则不清除该标志位，且记录找到的这个脏字符串
// 的索引，然后退出Tick函数，等待下一次10Hz Tick时刷新这个字符串；如果没有找到，就清除标志位，下一次
// 10Hz Tick时将会从jluiScanOffset处开始正常扫描图形项。

// 标志位，为true时表示下一次要更新的图形项为字符串。
static bool jluiPendingUpdateIsString = false;

// 当下一次要更新的图形项为字符串时，此值代表着字符串对象在列表中的索引。
static uint32_t jluiPendingStringIndex = 0;

// 对象列表互斥锁；当操作读写时将加锁。
static void *jluiObjectListMutex = (void*)1;

// 自增ID，用作给裁判系统的索引，但只取低三字节。
static uint32_t jluiAutoIncrementId = 1;

// ================================================================================================
//     函数实现
// ================================================================================================

#ifdef JLUI_ENABLE_ASSERT
#define JLUI_ASSERT assert
#else
#define JLUI_ASSERT(x)
#endif

#define JLUI_TRY_LOCK(...)                                                                         \
    do                                                                                             \
    {                                                                                              \
        if (!JLUI_MutexLock(jluiObjectListMutex))                                                  \
        {                                                                                          \
            return __VA_ARGS__;                                                                    \
        }                                                                                          \
    } while (0)

static inline void JLUI_CHECK_DATA()
{
    JLUI_ASSERT(jluiObjectListMutex != nullptr);
    JLUI_ASSERT(jluiScanOffset < JLUI_TOTAL_COUNT);
    JLUI_ASSERT(jluiPendingStringIndex < JLUI_TOTAL_COUNT);
}

static inline void JLUI_CHECK_ID(Uiid id)
{
    JLUI_ASSERT(id < JLUI_TOTAL_COUNT);
    JLUI_ASSERT(jluiObjectList[id].metadata.valid);
}

static inline JluiTxBufferHeader *JLUI_Internal_GetBufferHeader()
{
    return reinterpret_cast<JluiTxBufferHeader *>(jluiTxBuffer);
}

static inline JluiOfficialUiObject *JLUI_Internal_GetBufferNthUiObject(uint32_t index)
{
    return reinterpret_cast<JluiOfficialUiObject *>(jluiTxBuffer + sizeof(JluiTxBufferHeader) +
                                                    index * sizeof(JluiOfficialUiObject));
}

static inline char *JLUI_Internal_GetBufferStringBuffer()
{
    return reinterpret_cast<char *>(jluiTxBuffer + sizeof(JluiTxBufferHeader) +
                                    sizeof(JluiOfficialUiObject));
}

// Keil AC5不支持C++11的std::function，必须使用模板让编译器自己推导出lambda参数类型
// 在下面传入lambda时也使用了auto类型让编译器自行推导
// 傻逼ST什么时候让CubeMX FreeRTOS支持AC6啊
template <typename T>
static void JLUI_Internal_LoopScanObjectList(uint32_t fromIndex,  ///< 包含
                                             uint32_t untilIndex, ///< 不包含
                                             T callback)
{
    // 上界小于下界就默认是从末尾环绕回开头
    uint32_t upperBound =
        ((untilIndex <= fromIndex) ? (untilIndex + JLUI_TOTAL_COUNT) : untilIndex);
    for (size_t i = fromIndex; i < upperBound; i++)
    {
        // 回调返回false时，跳出循环
        if (!callback(i % JLUI_TOTAL_COUNT, jluiObjectList[i % JLUI_TOTAL_COUNT]))
        {
            break;
        }
    }
}

static inline void JLUI_Internal_LoopIncrement(uint32_t &x) { x = (x + 1) % JLUI_TOTAL_COUNT; }

static void JLUI_Internal_TransmitStringObject(uint32_t index, UiOperation op)
{
    auto &strObj = jluiObjectList[index];
    JLUI_ASSERT(strObj.detailDword1.type == UiStr);
    JLUI_ASSERT(strObj.detailDword3.strVal);

    auto header = JLUI_Internal_GetBufferHeader();
    header->DataLength = 51; // (6)+15+30
    Append_CRC8_Check_Sum(reinterpret_cast<unsigned char *>(header), 5);
    header->ContentId = 0x0110;
    
    auto stringMeta = JLUI_Internal_GetBufferNthUiObject(0);
    stringMeta->detailDword1 = strObj.detailDword1.dw;
    stringMeta->detailDword2 = strObj.detailDword2.dw;
    stringMeta->detailDword3 = strObj.detailDword3.dw;
    memcpy((void *)&(stringMeta->name), (void *)&(strObj.refereeHandle), 3);
    stringMeta->detailDword1Internal.operation = op;

    auto stringBuffer = JLUI_Internal_GetBufferStringBuffer();
    memset(stringBuffer, 0, 30);
    strncpy(stringBuffer,
            strObj.detailDword3.strVal,
            std::min(uint32_t(strObj.detailDword1.detailB), 30u));
    Append_CRC16_Check_Sum(jluiTxBuffer, 60); // (5+2+6)+15+30+2

    JLUI_SendData(jluiTxBuffer, 60);
}

static void JLUI_Internal_TransmitOtherObjects(size_t count)
{
    constexpr uint8_t elementCountInPacketTable[] = {0, 1, 2, 5, 5, 5, 7, 7};
    constexpr uint16_t contentIdTable[] =
        {0, 0x0101, 0x0102, 0x0103, 0x0103, 0x0103, 0x0104, 0x0104};

    JLUI_ASSERT(count <= 7);
    uint32_t elementCountInPacket = elementCountInPacketTable[count];

    auto header = JLUI_Internal_GetBufferHeader();
    header->DataLength = 6 + elementCountInPacket * 15; // (2+2+2)+15*elementCountInPacket
    Append_CRC8_Check_Sum(reinterpret_cast<unsigned char *>(header), 5);
    header->ContentId = contentIdTable[count];
    // 将没有用到的图形的操作码清除
    for (size_t i = count; i < elementCountInPacket; i++)
    {
        auto bufferObj = JLUI_Internal_GetBufferNthUiObject(i);
        bufferObj->detailDword1Internal.operation = UiOpNoop;
    }

    Append_CRC16_Check_Sum(jluiTxBuffer, 13 + elementCountInPacket * 15 + 2);

    JLUI_SendData(jluiTxBuffer, 13 + elementCountInPacket * 15 + 2);
}

static void JLUI_Internal_WriteTxBufferObjectOperation(uint32_t txBufferIndex,
                                                       uint32_t objectListIndex, UiOperation op)
{
    auto &obj = jluiObjectList[objectListIndex];
    auto bufferObj = JLUI_Internal_GetBufferNthUiObject(txBufferIndex);

    bufferObj->detailDword1 = obj.detailDword1.dw;
    bufferObj->detailDword2 = obj.detailDword2.dw;
    bufferObj->detailDword3 = obj.detailDword3.dw;
    memcpy((void *)&(bufferObj->name), (void *)&(obj.refereeHandle), 3);
    bufferObj->detailDword1Internal.operation = op;
}

static int32_t JLUI_Internal_CreateAndInitObject()
{
    for (int32_t i = 0; i < JLUI_TOTAL_COUNT; i++)
    {
        auto &obj = jluiObjectList[i];
        if (!obj.metadata.valid)
        {
            // 初始化元数据，保证下次Tick时能更新上去
            obj.metadata.valid = true;
            obj.metadata.deleted = false;
            obj.metadata.dirty = false;
            obj.metadata.dirtyVisibility = true; // 让后面能扫到它的更新
            obj.metadata.visible = true;
            // 初始化索引
            memcpy(obj.refereeHandle, (void *)&jluiAutoIncrementId, 3);
            // 初始化所有细节字段为0
            obj.detailDword1.dw = 0;
            obj.detailDword2.dw = 0;
            obj.detailDword3.dw = 0;
            // 自增ID增加
            jluiAutoIncrementId++;
            return i;
        }
    }
    return -1;
}

extern "C" void JLUI_SetMutexObject(void *mutex) { jluiObjectListMutex = mutex; }

extern "C" void JLUI_SetSenderReceiverId(uint16_t senderId, uint16_t receiverId)
{
    auto header = JLUI_Internal_GetBufferHeader();
    header->SOF = 0xA5;
    header->Seq = 0;
    header->SenderId = senderId;
    header->ReceiverId = receiverId;
    header->CommandId = 0x0301;
    memset(jluiObjectList, 0, sizeof(jluiObjectList));
}

extern "C" void JLUI_10HzTick()
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK();
    
RestartForStringProcessing:
    bool alreadySentStringOnce = false;
    
    // 要求更新字符串
    if (jluiPendingUpdateIsString)
    {
        auto &obj = jluiObjectList[jluiPendingStringIndex];
        obj.metadata.dirty = false;
        alreadySentStringOnce = true;
        if (obj.metadata.dirtyVisibility && obj.metadata.visible)
        {
            // 创建字符串
            obj.metadata.dirtyVisibility = false;
            JLUI_Internal_TransmitStringObject(jluiPendingStringIndex, UiOpAdd);
        }
        else
        {
            // 修改字符串
            JLUI_Internal_TransmitStringObject(jluiPendingStringIndex, UiOpModify);
        }
        // 前移一个图形元素
        JLUI_Internal_LoopIncrement(jluiPendingStringIndex);
        jluiPendingUpdateIsString = false;
        // 扫描剩下的字符串，找到字符串就恢复状态位
        auto iterStrObj = [&](uint32_t i, JluiUiObject &obj) -> bool {
            if (obj.detailDword1.type == UiStr)
            {
                jluiPendingStringIndex = i;
                jluiPendingUpdateIsString = true;
                return false;
            }
            return true;
        };
        JLUI_Internal_LoopScanObjectList(jluiPendingStringIndex, jluiScanOffset, iterStrObj);
    }
    else
    {
        // 扫描列表，查找需要更新的图形，至多7个
        size_t itemsProcessed = 0;
        auto iterObj = [&](uint32_t i, JluiUiObject &obj) -> bool {
            // 递增一次扫描起始点
            JLUI_Internal_LoopIncrement(jluiScanOffset);
            // 跳过未用到的项
            if (!obj.metadata.valid)
            {
                return true;
            }
            // 跳过没有任何脏标记的项
            if (!obj.metadata.dirty && !obj.metadata.dirtyVisibility && !obj.metadata.deleted)
            {
                return true;
            }
            // 处理脏标记
            if (obj.metadata.deleted)
            {
                // 如果已经标记为删除，添加删除操作，并去除有效位
                obj.metadata.valid = false;
                JLUI_Internal_WriteTxBufferObjectOperation(itemsProcessed, i, UiOpDelete);
                itemsProcessed++;
            }
            else if (obj.metadata.dirtyVisibility)
            {
                // 否则，如果可见性改变，则添加删除/添加操作
                if (obj.metadata.visible)
                {
                    // 字符串显示必须单独处理
                   if (obj.detailDword1.type == UiStr)
                    {
                        if (!jluiPendingUpdateIsString)
                        {
                            jluiPendingUpdateIsString = true;
                            jluiPendingStringIndex = i;
                        }
                    }
                    else
                    {
                        // 简单图形项的设置
                        obj.metadata.dirtyVisibility = false;
                        JLUI_Internal_WriteTxBufferObjectOperation(itemsProcessed, i, UiOpAdd);
                        itemsProcessed++;
                    }
                }
                else
                {
                    // 删除操作所有对象都是统一的
                    obj.metadata.dirtyVisibility = false;
                    JLUI_Internal_WriteTxBufferObjectOperation(itemsProcessed, i, UiOpDelete);
					itemsProcessed++;
                }
            }
            else if (obj.metadata.dirty)
            {
                // 对于字符串，设置标志位，下次Tick时只更新一个字符串
                // 注意不要在已经设置了字符串更新标志位的情况下再次记录，会丢失上次的记录
                if (obj.detailDword1.type == UiStr && !jluiPendingUpdateIsString)
                {
                    jluiPendingUpdateIsString = true;
                    jluiPendingStringIndex = i;
                }
                else
                {
                    // 否则，如果有脏标记，则添加修改操作
                    obj.metadata.dirty = false;
                    JLUI_Internal_WriteTxBufferObjectOperation(itemsProcessed, i, UiOpModify);
                    itemsProcessed++;
                }
            }
            else
            {
                // 不应该发生
                JLUI_ASSERT(false);
            }
            // 处理完这个脏图形后立即检测缓冲区是否已满，如果满，退出循环
            if (itemsProcessed >= 7)
            {
                return false;
            }
            return true;
        };
        JLUI_Internal_LoopScanObjectList(jluiScanOffset, jluiScanOffset, iterObj);
        // 如果有需要更新的图形，发送数据
        if (itemsProcessed > 0)
        {
            JLUI_Internal_TransmitOtherObjects(itemsProcessed);
        }
        else if (itemsProcessed == 0 && jluiPendingUpdateIsString && !alreadySentStringOnce)
        {
            // 如果没有处理简单图形项但是出现了需要处理的字符串，就跳到函数开头重新开始
            goto RestartForStringProcessing;
        }
    }
    // 解锁
    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" Uiid JLUI_CreateLine(int width, UiObjectColor color, int layer, int x1, int y1, int x2,
                                int y2)
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK(-1);

    auto newId = JLUI_Internal_CreateAndInitObject();
    if (newId < 0)
    {
        return UiNoMoreSpace;
    }

    auto &obj = jluiObjectList[newId];
    obj.detailDword1.color = color;
    obj.detailDword1.layer = layer;
    obj.detailDword1.type = UiLine;
    obj.detailDword2.width = width;
    obj.detailDword2.x = x1;
    obj.detailDword2.y = y1;
    obj.detailDword3.line.x2 = x2;
    obj.detailDword3.line.y2 = y2;

    JLUI_MutexUnlock(jluiObjectListMutex);
    return newId;
}

extern "C" Uiid JLUI_CreateRect(int width, UiObjectColor color, int layer, int x1, int y1, int x2,
                                int y2)
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK(UiMutexTimeout);

    auto newId = JLUI_Internal_CreateAndInitObject();
    if (newId < 0)
    {
        return UiNoMoreSpace;
    }

    auto &obj = jluiObjectList[newId];
    obj.detailDword1.color = color;
    obj.detailDword1.layer = layer;
    obj.detailDword1.type = UiRect;
    obj.detailDword2.width = width;
    obj.detailDword2.x = x1;
    obj.detailDword2.y = y1;
    obj.detailDword3.line.x2 = x2;
    obj.detailDword3.line.y2 = y2;

    JLUI_MutexUnlock(jluiObjectListMutex);
    return newId;
}

extern "C" Uiid JLUI_CreateCircle(int width, UiObjectColor color, int layer, int x, int y,
                                  int radius)
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK(UiMutexTimeout);

    auto newId = JLUI_Internal_CreateAndInitObject();
    if (newId < 0)
    {
        return UiNoMoreSpace;
    }

    auto &obj = jluiObjectList[newId];
    obj.detailDword1.color = color;
    obj.detailDword1.layer = layer;
    obj.detailDword1.type = UiCircle;
    obj.detailDword2.width = width;
    obj.detailDword2.x = x;
    obj.detailDword2.y = y;
    obj.detailDword3.circle.radius = radius;

    JLUI_MutexUnlock(jluiObjectListMutex);
    return newId;
}

extern "C" Uiid JLUI_CreateEllipse(int width, UiObjectColor color, int layer, int x, int y,
                                   int xSemiaxis, int ySemiaxis)
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK(UiMutexTimeout);

    auto newId = JLUI_Internal_CreateAndInitObject();
    if (newId < 0)
    {
        return UiNoMoreSpace;
    }

    auto &obj = jluiObjectList[newId];
    obj.detailDword1.color = color;
    obj.detailDword1.layer = layer;
    obj.detailDword1.type = UiEllipse;
    obj.detailDword2.width = width;
    obj.detailDword2.x = x;
    obj.detailDword2.y = y;
    obj.detailDword3.ellipse.xSemiaxis = xSemiaxis;
    obj.detailDword3.ellipse.ySemiaxis = ySemiaxis;

    JLUI_MutexUnlock(jluiObjectListMutex);
    return newId;
}

extern "C" Uiid JLUI_CreateArc(int width, UiObjectColor color, int layer, int x, int y,
                               int xSemiaxis, int ySemiaxis, int startAngle, int endAngle)
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK(UiMutexTimeout);

    auto newId = JLUI_Internal_CreateAndInitObject();
    if (newId < 0)
    {
        return UiNoMoreSpace;
    }

    auto &obj = jluiObjectList[newId];
    obj.detailDword1.color = color;
    obj.detailDword1.layer = layer;
    obj.detailDword1.type = UiArc;
    obj.detailDword2.width = width;
    obj.detailDword2.x = x;
    obj.detailDword2.y = y;
    obj.detailDword3.ellipse.xSemiaxis = xSemiaxis;
    obj.detailDword3.ellipse.ySemiaxis = ySemiaxis;
    obj.detailDword1.detailA = startAngle;
    obj.detailDword1.detailB = endAngle;

    JLUI_MutexUnlock(jluiObjectListMutex);
    return newId;
}

extern "C" Uiid JLUI_CreateFloat(int width, enum UiObjectColor color, int layer, int x, int y,
                                 int fontSize, float value)
{
    JLUI_CreateFloatWithInt(width,
                            color,
                            layer,
                            x,
                            y,
                            fontSize,
                            static_cast<uint32_t>(value * 1000));
}

extern "C" Uiid JLUI_CreateFloatWithInt(int width, UiObjectColor color, int layer, int x, int y,
                                        int fontSize, int value)
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK(UiMutexTimeout);

    auto newId = JLUI_Internal_CreateAndInitObject();
    if (newId < 0)
    {
        return UiNoMoreSpace;
    }

    auto &obj = jluiObjectList[newId];
    obj.detailDword1.color = color;
    obj.detailDword1.layer = layer;
    obj.detailDword1.type = UiFloat;
    obj.detailDword1.detailA = fontSize;
    obj.detailDword2.width = width;
    obj.detailDword2.x = x;
    obj.detailDword2.y = y;
    obj.detailDword3.floatVal = value;

    JLUI_MutexUnlock(jluiObjectListMutex);
    return newId;
}

extern "C" Uiid JLUI_CreateInt(int width, UiObjectColor color, int layer, int x, int y,
                               int fontSize, int value)
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK(UiMutexTimeout);

    auto newId = JLUI_Internal_CreateAndInitObject();
    if (newId < 0)
    {
        return UiNoMoreSpace;
    }

    auto &obj = jluiObjectList[newId];
    obj.detailDword1.color = color;
    obj.detailDword1.layer = layer;
    obj.detailDword1.type = UiInt;
    obj.detailDword1.detailA = fontSize;
    obj.detailDword2.width = width;
    obj.detailDword2.x = x;
    obj.detailDword2.y = y;
    obj.detailDword3.intVal = value;

    JLUI_MutexUnlock(jluiObjectListMutex);
    return newId;
}

extern "C" Uiid JLUI_CreateString(int width, UiObjectColor color, int layer, int x, int y,
                                  int fontSize, const char *str)
{
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK(UiMutexTimeout);

    auto newId = JLUI_Internal_CreateAndInitObject();
    if (newId < 0)
    {
        return UiNoMoreSpace;
    }

    auto &obj = jluiObjectList[newId];
    obj.detailDword1.color = color;
    obj.detailDword1.layer = layer;
    obj.detailDword1.type = UiStr;
    obj.detailDword1.detailA = fontSize;
    obj.detailDword1.detailB = std::min(30u, strlen(str));
    obj.detailDword2.width = width;
    obj.detailDword2.x = x;
    obj.detailDword2.y = y;
    obj.detailDword3.strVal = str;

    JLUI_MutexUnlock(jluiObjectListMutex);
    return newId;
}

extern "C" void JLUI_SetVisible(Uiid id, bool visible)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);

    auto &obj = jluiObjectList[id];
    if (obj.metadata.visible == visible)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.metadata.dirtyVisibility = true;
    obj.metadata.visible = visible;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetColor(Uiid id, UiObjectColor color)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    
    auto &obj = jluiObjectList[id];
    if (obj.detailDword1.color == color)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword1.color = color;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetWidth(Uiid id, int width)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    
    auto &obj = jluiObjectList[id];
    if (obj.detailDword2.width == width)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword2.width = width;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetFontSize(Uiid id, int fontSize)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiFloat ||
                jluiObjectList[id].detailDword1.type == UiInt ||
                jluiObjectList[id].detailDword1.type == UiStr);
                
    auto &obj = jluiObjectList[id];
    if (obj.detailDword1.detailA == fontSize)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword1.detailA = fontSize;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetStringChanged(Uiid id)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiStr);
    JLUI_TRY_LOCK();

    auto &obj = jluiObjectList[id];
    // 重新计算字符串长度
    obj.detailDword1.detailB = std::min(uint32_t(strlen(obj.detailDword3.strVal)), 30u);
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_MoveTo(Uiid id, int x, int y)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);

    auto &obj = jluiObjectList[id];
    if (obj.detailDword2.x == x && obj.detailDword2.y == y)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword2.x = x;
    obj.detailDword2.y = y;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_MoveP2To(Uiid id, int x, int y)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiLine ||
                jluiObjectList[id].detailDword1.type == UiRect);

    auto &obj = jluiObjectList[id];
    if (obj.detailDword3.line.x2 == x && obj.detailDword3.line.y2 == y)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword3.line.x2 = x;
    obj.detailDword3.line.y2 = y;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetRadius(Uiid id, int radius)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiCircle);

    auto &obj = jluiObjectList[id];
    if (obj.detailDword3.circle.radius == radius)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword3.circle.radius = radius;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetSemiaxis(Uiid id, int xSemiaxis, int ySemiaxis)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiEllipse ||
                jluiObjectList[id].detailDword1.type == UiArc);

    auto &obj = jluiObjectList[id];
    if (obj.detailDword3.ellipse.xSemiaxis == xSemiaxis &&
        obj.detailDword3.ellipse.ySemiaxis == ySemiaxis)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword3.ellipse.xSemiaxis = xSemiaxis;
    obj.detailDword3.ellipse.ySemiaxis = ySemiaxis;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetStartAngle(Uiid id, int startAngle)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiArc);

    auto &obj = jluiObjectList[id];
    if (obj.detailDword1.detailA == startAngle)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword1.detailA = startAngle;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetEndAngle(Uiid id, int endAngle)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiArc);
    
    auto &obj = jluiObjectList[id];
    if (obj.detailDword1.detailB == endAngle)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword1.detailB = endAngle;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetFloat(Uiid id, float value)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiFloat);

    auto &obj = jluiObjectList[id];
    int floatVal = static_cast<int>(value * 1000);
    if (obj.detailDword3.floatVal == floatVal)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword3.floatVal = floatVal;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetFloatWithInt(Uiid id, int value)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiFloat);

    auto &obj = jluiObjectList[id];
    if (obj.detailDword3.floatVal == value)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword3.floatVal = value;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetInt(Uiid id, int value)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiInt);

    auto &obj = jluiObjectList[id];
    if (obj.detailDword3.intVal == value)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword3.intVal = value;
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetString(Uiid id, const char *str)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiStr);

    auto &obj = jluiObjectList[id];
    if (strcmp(obj.detailDword3.strVal, str) == 0)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword3.strVal = str;
    obj.detailDword1.detailB = std::min(uint32_t(strlen(str)), 30u);
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_SetStringWithLength(Uiid id, const char *str, int length)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_ASSERT(jluiObjectList[id].detailDword1.type == UiStr);

    auto &obj = jluiObjectList[id];
    if (obj.detailDword1.detailB == length && strncmp(obj.detailDword3.strVal, str, length) == 0)
    {
        return;
    }

    JLUI_TRY_LOCK();

    obj.detailDword3.strVal = str;
    obj.detailDword1.detailB = std::min(uint32_t(length), 30u);
    obj.metadata.dirty = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_Delete(Uiid id)
{
    JLUI_CHECK_DATA();
    JLUI_CHECK_ID(id);
    JLUI_TRY_LOCK();

    auto &obj = jluiObjectList[id];
    obj.metadata.deleted = true;

    JLUI_MutexUnlock(jluiObjectListMutex);
}

extern "C" void JLUI_DeleteAll()
{
    // TODO: 可以直接用裁判系统的删除命令，但需要特殊数据包，暂未实现op
    JLUI_CHECK_DATA();
    JLUI_TRY_LOCK();

    for (int i = 0; i < JLUI_TOTAL_COUNT; i++)
    {
        jluiObjectList[i].metadata.deleted = true;
    }

    JLUI_MutexUnlock(jluiObjectListMutex);
}
