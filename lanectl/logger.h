#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <stdio.h>

// ============================================================================
//  简陋日志 (和旧版一样烂, 只是换了目录)
//  脏点: 无锁、无分级、每条 fflush、路径写死 Windows 风格
// ============================================================================
class Logger
{
public:
    static Logger *inst();

    void init();
    void write(const QString &msg);

private:
    Logger() { m_fp = 0; }

    FILE *m_fp;
};

#define LOG(x) Logger::inst()->write(x)

#endif // LOGGER_H
