#ifndef CONSOLE_H
#define CONSOLE_H

#include <QObject>
#include <QString>

// ============================================================================
//  控制台输入 (Windows conio 轮询)
//  脏点: 只支持可见 ASCII, 不支持粘贴、方向键、中文输入;
//        换到 Linux 上就编译不过 (conio.h 是 Windows 的)
// ============================================================================
class ConsoleInput : public QObject
{
    Q_OBJECT

public:
    explicit ConsoleInput(QObject *parent = 0);
    void start();

signals:
    void command(const QString &cmd);

private slots:
    void poll();

private:
    QString m_line;
};

#endif // CONSOLE_H
