#include "customtermwidget.h"
#include <QDebug>
#include <QMenu>
#include <QContextMenuEvent>
#include <QMouseEvent>

CustomTermWidget::CustomTermWidget(int startnow, QWidget *parent)
    : QTermWidget(startnow, parent)
{
    // Фокус и политика контекстного меню
    setFocusPolicy(Qt::StrongFocus);
    setContextMenuPolicy(Qt::DefaultContextMenu);

    // Сигнал copyAvailable больше не подключаем, так как его нет в этой версии
    // Вместо этого используем mouseReleaseEvent для автокопирования
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
    // Вызываем базовую реализацию для корректной обработки выделения
    QTermWidget::mouseReleaseEvent(event);

    // Если после отпускания кнопки есть выделенный текст, копируем его в системный буфер
    if (!selectedText().isEmpty()) {
        copyClipboard();
        // qDebug() << "CustomTermWidget: выделение скопировано в Clipboard";
    }
}

// Слот оставлен для совместимости, но больше не вызывается
void CustomTermWidget::onCopyAvailable(bool available)
{
    Q_UNUSED(available);
    // Ничего не делаем
}