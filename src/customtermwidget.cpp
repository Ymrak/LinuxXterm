#include "customtermwidget.h"
#include <QDebug>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMouseEvent>

CustomTermWidget::CustomTermWidget(int startnow, QWidget *parent)
    : QTermWidget(startnow, parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setContextMenuPolicy(Qt::DefaultContextMenu);

    qDebug() << "CustomTermWidget: инициализирован";
}

CustomTermWidget::~CustomTermWidget() = default;

void CustomTermWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = new QMenu(this);

    QAction *copyAction = menu->addAction(tr("Копировать"));
    copyAction->setEnabled(!selectedText().isEmpty());
    connect(copyAction, &QAction::triggered, this, &CustomTermWidget::copyClipboard);

    QAction *pasteAction = menu->addAction(tr("Вставить"));
    connect(pasteAction, &QAction::triggered, this, &CustomTermWidget::pasteClipboard);

    menu->addSeparator();
    QAction *clearAction = menu->addAction(tr("Очистить"));
    connect(clearAction, &QAction::triggered, this, &CustomTermWidget::clear);

    menu->exec(event->globalPos());
    delete menu;
}

void CustomTermWidget::mouseReleaseEvent(QMouseEvent *event)
{
    QTermWidget::mouseReleaseEvent(event);

    if (!selectedText().isEmpty()) {
        copyClipboard();
    }
}

void CustomTermWidget::onCopyAvailable(bool available)
{
    Q_UNUSED(available);
}