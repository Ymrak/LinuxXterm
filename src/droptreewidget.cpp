#include "droptreewidget.h"
#include <QDebug>
#include <QFile>
#include <QUrl>
#include <QMimeData>
#include <QDrag>
#include <QApplication>

DropTreeWidget::DropTreeWidget(QWidget *parent)
    : QTreeWidget(parent)
{
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDragDropOverwriteMode(false);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    
    qDebug() << "DropTreeWidget: создан, режим DragDrop";
}

void DropTreeWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        m_dragRequested = false;
    }
    QTreeWidget::mousePressEvent(event);
}

void DropTreeWidget::mouseMoveEvent(QMouseEvent *event)
{
    if ((event->buttons() & Qt::LeftButton) && !m_dragRequested) {
        int distance = (event->pos() - m_dragStartPos).manhattanLength();
        if (distance >= QApplication::startDragDistance()) {
            m_dragRequested = true;
            startDrag();
            event->accept();
            return;
        }
    }
    if (m_dragRequested) {
        event->accept();
        return;
    }
    QTreeWidget::mouseMoveEvent(event);
}

void DropTreeWidget::startDrag()
{
    QList<QTreeWidgetItem*> selectedItems = this->selectedItems();
    
    if (selectedItems.isEmpty()) {
        qDebug() << "DropTreeWidget::startDrag: нет выделенных элементов";
        return;
    }
    
    QStringList remotePaths;
    for (QTreeWidgetItem* item : selectedItems) {
        QString path = item->data(0, Qt::UserRole).toString();
        bool isDir = item->data(0, Qt::UserRole + 1).toBool();
        
        if (path != "spacer" && path != ".." && !isDir) {
            remotePaths.append(path);
        }
    }
    
    if (remotePaths.isEmpty()) {
        qDebug() << "DropTreeWidget::startDrag: нет файлов для перетаскивания";
        return;
    }
    
    qDebug() << "DropTreeWidget::startDrag: запрашиваем перетаскивание" << remotePaths.size() << "файл(ов) с сервера";
    
    emit requestDragFromServer(remotePaths);
}

void DropTreeWidget::startDragWithLocalFiles(const QStringList &localPaths)
{
    if (localPaths.isEmpty()) {
        qDebug() << "DropTreeWidget::startDragWithLocalFiles: нет локальных файлов";
        return;
    }
    
    qDebug() << "DropTreeWidget::startDragWithLocalFiles: начинаем перетаскивание" << localPaths.size() << "локальных файлов";
    
    QMimeData *mimeData = new QMimeData();
    
    QList<QUrl> urls;
    for (const QString &path : localPaths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    mimeData->setUrls(urls);
    
    QDrag *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    
    Qt::DropAction dropAction = drag->exec(Qt::CopyAction, Qt::CopyAction);
    
    qDebug() << "DropTreeWidget::startDragWithLocalFiles: перетаскивание завершено, действие:" << dropAction;
    
}

void DropTreeWidget::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    
    qDebug() << "===== DropTreeWidget::dragEnterEvent =====";
    qDebug() << "MIME types:" << mimeData->formats();
    
    if (mimeData->hasUrls()) {
        bool allLocal = true;
        for (const QUrl &url : mimeData->urls()) {
            if (!url.isLocalFile()) {
                allLocal = false;
            }
        }
        if (allLocal) {
            event->acceptProposedAction();
            qDebug() << "DropTreeWidget: принимаем перетаскивание URL";
            return;
        }
    }
    
    if (mimeData->hasFormat("text/uri-list")) {
        event->acceptProposedAction();
        qDebug() << "DropTreeWidget: принимаем text/uri-list";
        return;
    }
    
    if (mimeData->hasText()) {
        event->acceptProposedAction();
        qDebug() << "DropTreeWidget: принимаем text/plain";
        return;
    }
    
    event->ignore();
    qDebug() << "DropTreeWidget: отклоняем перетаскивание";
}

void DropTreeWidget::dragMoveEvent(QDragMoveEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    
    if (mimeData->hasUrls() || mimeData->hasFormat("text/uri-list") || mimeData->hasText()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void DropTreeWidget::dropEvent(QDropEvent *event)
{
    qDebug() << "===== DropTreeWidget::dropEvent =====";
    
    const QMimeData *mimeData = event->mimeData();
    QStringList filePaths;
    
    if (mimeData->hasUrls()) {
        for (const QUrl &url : mimeData->urls()) {
            if (url.isLocalFile()) {
                filePaths.append(url.toLocalFile());
            } else {
                QString path = QUrl::fromUserInput(url.toString()).toLocalFile();
                if (!path.isEmpty() && QFile::exists(path)) {
                    filePaths.append(path);
                }
            }
        }
    }
    
    if (filePaths.isEmpty() && mimeData->hasFormat("text/uri-list")) {
        QString uriList = QString::fromUtf8(mimeData->data("text/uri-list"));
        QStringList lines = uriList.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            QString trimmed = line.trimmed();
            if (trimmed.startsWith("file://")) {
                QString path = QUrl::fromPercentEncoding(trimmed.toUtf8());
                path = path.remove(0, 7);
                if (QFile::exists(path)) {
                    filePaths.append(path);
                }
            }
        }
    }
    
    if (filePaths.isEmpty() && mimeData->hasText()) {
        QString text = mimeData->text();
        QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            QString trimmed = line.trimmed();
            if (QFile::exists(trimmed)) {
                filePaths.append(trimmed);
            }
        }
    }
    
    if (!filePaths.isEmpty()) {
        event->acceptProposedAction();
        emit filesDropped(filePaths);
    } else {
        event->ignore();
    }
}