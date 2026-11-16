#include "vehicle.h"

QString resText(int r)
{
    // 脏点: 又是一个 if 链; 加一种结果要同时改这里和 refreshTable 里的判断
    if (r == RES_RUNNING)
        return QString::fromUtf8("进行中");
    if (r == RES_ISSUED)
        return QString::fromUtf8("已发卡");
    if (r == RES_RECYCLED)
        return QString::fromUtf8("已回收");
    if (r == RES_TIMEOUT)
        return QString::fromUtf8("超时未取");
    if (r == RES_NO_CARD)
        return QString::fromUtf8("卡箱无卡");
    return QString::fromUtf8("未知(%1)").arg(r);
}
