#include "console.h"

#include <QDebug>
#include <QTimer>
#include <stdio.h>

#ifdef Q_OS_WIN
#include <conio.h>
#endif

ConsoleInput::ConsoleInput(QObject *parent)
    : QObject(parent)
{
}

void ConsoleInput::start()
{
    QTimer *t = new QTimer(this);
    connect(t, SIGNAL(timeout()), this, SLOT(poll()));
    t->start(40);
}

void ConsoleInput::poll()
{
#ifdef Q_OS_WIN
    // 脏点: 直接轮询键盘缓冲, 回车才算一条命令
    while (_kbhit())
    {
        int c = _getch();

        if (c == '\r')
        {
            printf("\n");
            QString cmd = m_line.trimmed();
            m_line.clear();
            if (!cmd.isEmpty())
                emit command(cmd);
        }
        else if (c == 8)          // 退格
        {
            if (!m_line.isEmpty())
            {
                m_line.chop(1);
                printf("\b \b");
            }
        }
        else if (c >= 32 && c < 127)
        {
            m_line.append(QChar(c));
            putchar(c);           // 回显
        }
    }
#endif
}
