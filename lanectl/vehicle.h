#ifndef VEHICLE_H
#define VEHICLE_H

#include <QDateTime>
#include <QString>

// ============================================================================
//  一辆车的通行流水
//
//  脏点:
//    1. 全 public, 谁都能改 result
//    2. result 用 int 裸奔, 没有枚举; 3 是"超时"还是"中止"看运气
//    3. 没有流水号唯一性保证, 重启程序又从 1 开始 —— 真实项目里这是要上传
//       到收费中心的, 重号直接算事故
//    4. 字段是"够用就行", 没有交易流水号、没有班次、没有操作员
// ============================================================================
struct VehicleRecord
{
    VehicleRecord()
    {
        seq = 0;
        kind = 0;
        result = 0;
        cardNo = "";
        plate = "";
        remark = "";
    }

    int seq;                 // 流水号
    QDateTime arriveTime;    // 来车时间
    QDateTime takeTime;      // 取卡时间
    QString plate;           // 车牌
    int kind;                // 车型
    QString cardNo;          // 发出的 CPC 卡号
    int result;              // 0 进行中 1 正常发卡 2 已回收 3 超时未取 4 卡箱无卡
    QString remark;          // 备注
};

// 流水结果
#define RES_RUNNING     0
#define RES_ISSUED      1
#define RES_RECYCLED    2
#define RES_TIMEOUT     3
#define RES_NO_CARD     4
#define RES_ABORTED     5

QString resText(int r);

#endif // VEHICLE_H
