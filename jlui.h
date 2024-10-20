#ifndef __JLUI_H__
#define __JLUI_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum UiObjectType
{
    UiLine,
    UiRect,
    UiCircle,
    UiEllipse,
    UiArc,
    UiFloat,
    UiInt,
    UiStr,
};

enum UiObjectColor
{
    UiTeam,
    UiYellow,
    UiGreen,
    UiOrange,
    UiMagenta,
    UiPink,
    UiCyan,
    UiBlack,
    UiWhite,
};

enum UiOperation
{
    UiOpNoop,
    UiOpAdd,
    UiOpModify,
    UiOpDelete,
};

enum UiidErrorCode
{
    UiNoMoreSpace = -1,
    UiMutexTimeout = -2,
};

#ifdef __cplusplus
extern "C"
{
#endif

struct JluiUiObject;

typedef int32_t Uiid;

// ================================================================================================
//     JLUI 移植接口
// ================================================================================================

// 【用户需要以10Hz的频率调用此函数】
void JLUI_10HzTick();

// 【用户需要实现此函数】
// 对互斥锁上锁。如果成功获取了锁，返回1；否则返回0。
int JLUI_MutexLock(void *mutex);

// 【用户需要实现此函数】
// 对互斥锁解锁。
void JLUI_MutexUnlock(void *mutex);

// 【用户需要实现此函数】
// 从串口向裁判系统发送数据函数。注意！从函数返回后，data缓冲区即应视为失效。如要做异步发送，请自行拷贝data。
void JLUI_SendData(const uint8_t *data, size_t len);

// 【用户需要实现此函数，如果项目中已存在，就无需再添加】
// 对缓冲区内指定长度的字节序列计算裁判系统CRC8，并添加在数据之后
void Append_CRC8_Check_Sum(unsigned char *data, unsigned int length);

// 【用户需要实现此函数，如果项目中已存在，就无需再添加】
// 对缓冲区内指定长度的字节序列计算裁判系统CRC16，并添加在数据之后
void Append_CRC16_Check_Sum(uint8_t *data, uint32_t length);

// ================================================================================================
//     JLUI API函数
// ================================================================================================

// 【用户需要在使用库前调用此函数进行初始化】
// 设置JLUI使用的互斥锁对象的指针。
void JLUI_SetMutexObject(void *mutex);

// 【用户需要在使用库前调用此函数进行初始化】
// 设置交互数据包的发送者ID和接收者ID。此函数同时会初始化其他需要单次初始化的包缓冲区字段，
// 以及清空图形缓冲区进行初始化，所以使用任何其他功能前必须调用！！
void JLUI_SetSenderReceiverId(uint16_t senderId, uint16_t receiverId);

// 获取所有已占用的图形个数
void JLUI_GetTotalObjectCount();

// 创建直线对象
Uiid JLUI_CreateLine(int width, enum UiObjectColor color, int layer, int x1, int y1, int x2,
                     int y2);

// 创建矩形对象
Uiid JLUI_CreateRect(int width, enum UiObjectColor color, int layer, int x1, int y1, int x2,
                     int y2);

// 创建圆形对象
Uiid JLUI_CreateCircle(int width, enum UiObjectColor color, int layer, int x, int y, int radius);

// 创建椭圆对象
Uiid JLUI_CreateEllipse(int width, enum UiObjectColor color, int layer, int x, int y, int xSemiaxis,
                        int ySemiaxis);

// 创建圆弧对象
Uiid JLUI_CreateArc(int width, enum UiObjectColor color, int layer, int x, int y, int xSemiaxis,
                    int ySemiaxis, int startAngle, int endAngle);

// 创建浮点数对象
Uiid JLUI_CreateFloat(int width, enum UiObjectColor color, int layer, int x, int y, int fontSize,
                      float value);
Uiid JLUI_CreateFloatWithInt(int width, enum UiObjectColor color, int layer, int x, int y,
                             int fontSize, int value);

// 创建整数对象
Uiid JLUI_CreateInt(int width, enum UiObjectColor color, int layer, int x, int y, int fontSize,
                    int value);

// 创建字符串对象
Uiid JLUI_CreateString(int width, enum UiObjectColor color, int layer, int x, int y, int fontSize,
                       const char *str);

// 修改图形可见性
void JLUI_SetVisible(Uiid id, bool visible);

// 修改图形颜色
void JLUI_SetColor(Uiid id, enum UiObjectColor color);

// 修改图形宽度
void JLUI_SetWidth(Uiid id, int width);

// 修改文字类图形项字体大小
void JLUI_SetFontSize(Uiid id, int fontSize);

// 标记字符串已改变
void JLUI_SetStringChanged(Uiid id);

// 修改图形坐标
void JLUI_MoveTo(Uiid id, int x, int y);

// 修改直线、矩形的第二个坐标
void JLUI_MoveP2To(Uiid id, int x, int y);

// 修改圆形的半径
void JLUI_SetRadius(Uiid id, int radius);

// 修改椭圆、圆弧的半轴
void JLUI_SetSemiaxis(Uiid id, int xSemiaxis, int ySemiaxis);

// 修改圆弧的起始角度
void JLUI_SetStartAngle(Uiid id, int startAngle);

// 修改圆弧的终止角度
void JLUI_SetEndAngle(Uiid id, int endAngle);

// 修改浮点数
void JLUI_SetFloat(Uiid id, float value); ///< 直接设置浮点数，内部乘以1000，可能丢失精度
void JLUI_SetFloatWithInt(Uiid id, int value); ///< 直接设置乘以1000的数，不丢失精度

// 修改整数
void JLUI_SetInt(Uiid id, int value);

// 修改字符串
void JLUI_SetString(Uiid id, const char *str);
void JLUI_SetStringWithLength(Uiid id, const char *str, int length);

// 删除图形
void JLUI_Delete(Uiid id);

// 删除所有图形
void JLUI_DeleteAll();

// ================================================================================================
//     私有数据结构
// ================================================================================================

#ifdef __GNUC__
#define HEAD_PACK
#define TAIL_PACK __attribute((packed))
#elif defined(__ARMCC_VERSION)
#define HEAD_PACK __packed
#define TAIL_PACK
#pragma anon_unions
#endif

HEAD_PACK struct JluiTxBufferHeader
{
    // 包头
    uint8_t SOF;
    uint16_t DataLength;
    uint8_t Seq;
    uint8_t Crc8;
    // 命令ID
    uint16_t CommandId;
    // 数据段包结构的头
    uint16_t ContentId;
    uint16_t SenderId;
    uint16_t ReceiverId;
} TAIL_PACK;

HEAD_PACK struct JluiUiObject
{
    // 图形对象元数据
    HEAD_PACK struct UiObjectMetadata
    {
        bool valid : 1; ///< 置位时表示此图形项有效。图形项被删除时不会清空，而是在下次Tick时清空。
        bool deleted : 1; ///< 置位时表示此图形项等待删除。该图形项下次被Tick时将被移除。
        bool dirty : 1;           ///< 已被修改位。如置位，下次Tick时会被更新。
        bool dirtyVisibility : 1; ///< 可见性已被修改位，如置位，下次Tick时会被创建/删除
        bool visible : 1; ///< 可见性位，图形可见时为1，不可见时为0。
                          ///< 此位需要和dirtyVisibility配合使用。
    } metadata TAIL_PACK;

    // 裁判系统三字节索引
    uint8_t refereeHandle[3];

    // 下面4字节把官方的3比特操作类型改成了3个脏比特
    HEAD_PACK union _DetailDword1Internals {
        uint32_t dw;
        HEAD_PACK struct
        {
            uint32_t operation : 3; ///< [内存中未使用] 图形操作
            uint32_t type : 3;      ///< [UiObjectType] 图形类型;
            uint32_t layer : 4;     ///< 图层；0～9为合法；非法图层会被强制设为0
            uint32_t color : 4;   ///< [UiObjectColor] 颜色；非法颜色会被强制设为UiTeam
            uint32_t detailA : 9; ///< （圆弧）起始角度/（文本）字体大小
            uint32_t detailB : 9; ///< （圆弧）终止角度/（字符串）长度
        } TAIL_PACK;
    } detailDword1 TAIL_PACK;

    // 下面4字节与官方一致
    HEAD_PACK union _DetailDword2Internals {
        uint32_t dw;
        HEAD_PACK struct
        {
            uint32_t width : 10; // 宽度
            uint32_t x : 11;     // 第一个x坐标
            uint32_t y : 11;     // 第一个y坐标
        } TAIL_PACK;
    } detailDword2 TAIL_PACK;

    // 下面4字节与官方一致
    HEAD_PACK union _DetailDword3Internals {
        uint32_t dw;
        HEAD_PACK struct
        {
            uint32_t radius : 10; // 正圆半径
            uint32_t reserved : 22;
        } circle TAIL_PACK; // 正圆使用的DetailDword3
        HEAD_PACK struct
        {
            uint32_t reserved : 10;
            uint32_t x2 : 11; // 第二个x坐标
            uint32_t y2 : 11; // 第二个y坐标
        } line TAIL_PACK;     // 直线和矩形使用的DetailDword3
        HEAD_PACK struct
        {
            uint32_t reserved : 10;
            uint32_t xSemiaxis : 11; // x半轴长度
            uint32_t ySemiaxis : 11; // y半轴长度
        } ellipse TAIL_PACK;
        uint32_t intVal;    ///< 整型数
        uint32_t floatVal;  ///< 浮点数
        const char *strVal; ///< 字符串指针
    } detailDword3 TAIL_PACK;
} TAIL_PACK;

HEAD_PACK struct JluiOfficialUiObject
{
    char name[3];
    HEAD_PACK union {
        uint32_t detailDword1;
        HEAD_PACK struct
        {
            uint32_t operation : 3; ///< 图形操作类型
            uint32_t not_used : 29; ///< 未使用
        } detailDword1Internal TAIL_PACK;
    } TAIL_PACK;
    uint32_t detailDword2;
    uint32_t detailDword3;
} TAIL_PACK;

#undef HEAD_PACK
#undef TAIL_PACK

#ifdef __cplusplus
} // extern "C"
#endif

#endif
