#ifndef DROPTREEWIDGET_H
#define DROPTREEWIDGET_H

#include <QTreeWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMouseEvent>
#include <QMimeData>
#include <QUrl>
#include <QDrag>

class DropTreeWidget : public QTreeWidget
{
    Q_OBJECT

public:
    explicit DropTreeWidget(QWidget *parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

signals:
    void filesDropped(const QStringList &filePaths);
    void requestDragFromServer(const QStringList &remotePaths);

public slots:
    void startDragWithLocalFiles(const QStringList &localPaths);

private:
    void startDrag();
    QPoint m_dragStartPos;
    bool m_dragRequested = false;
};

#endif