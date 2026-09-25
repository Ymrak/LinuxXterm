#ifndef CUSTOMTERMWIDGET_H
#define CUSTOMTERMWIDGET_H

#include <qtermwidget.h>
#include <QContextMenuEvent>
#include <QMouseEvent>

class CustomTermWidget : public QTermWidget
{
    Q_OBJECT

public:
    explicit CustomTermWidget(int startnow, QWidget *parent = nullptr);
    ~CustomTermWidget();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
    void onCopyAvailable(bool available);  // оставим слот, но не будем его использовать
};

#endif // CUSTOMTERMWIDGET_H