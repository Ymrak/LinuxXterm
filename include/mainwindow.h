#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSplitter>
#include <QTreeWidget>
#include <QTabWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QAction>
#include <QStatusBar>
#include <QMap>
#include <QTimer>
#include "connectionmanager.h"
#include "connectiondialog.h"
#include "sshsession.h"
#include "passwordmanager.h"
#include "masterpassworddialog.h"
#include "customtermwidget.h"
#include "sftpmanager.h"

struct TerminalInfo {
    CustomTermWidget *terminal;
    QString connectionId;
    QString connectionName;
    bool isConnected;
    QTimer *statusTimer;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // Управление подключениями
    void onNewConnection();
    void onEditConnection();
    void onDeleteConnection();
    void onConnect();
    void onDisconnect();
    
    // Управление вкладками терминала
    void onCloseTab(int index);
    void onTabCurrentChanged(int index);
    void onTerminalProcessFinished();
    
    // SFTP Файловый менеджер
    void onSftpError(const QString &message);
    void onDirectoryListed(const QString &path, const QList<SftpFileInfo> &files);
    void onFileDoubleClicked(QTreeWidgetItem *item, int column);
    void onSftpDownload();
    void onSftpUpload();
    void onSftpDelete();
    void onSftpCreateDirectory();
    
    // Прочее
    void onAbout();

private:
    void setupUi();
    void setupMenuBar();
    void setupToolBar();
    void setupConnections();
    void updateConnectionsList();
    void setupTerminalWidget(CustomTermWidget *terminal);
    void replaceTerminalInTab(int index, CustomTermWidget *newTerminal, const QString &tabName, bool isConnected, const QString &connectionId);
    void createTerminal(const QString &program, const QStringList &args, const QString &tabName);
    void ensureVaultUnlocked();
    void checkSshpassInstalled();
    void openConnectionInNewTab(const QString &connectionId);
    QString shortenName(const QString &name);
    void refreshSftpDirectory(const QString &path);

    QSplitter *mainSplitter;
    QTabWidget *leftPanelTabs;      // Вкладки левой панели (Подключения / Файлы)
    QTreeWidget *connectionsTree;
    QTreeWidget *fileTreeWidget;
    QTabWidget *terminalTabs;
    QStatusBar *statusBarWidget;

    QMenu *menuFile;
    QMenu *menuConnections;
    QMenu *menuHelp;

    QToolBar *mainToolBar;

    QAction *actionNewConnection;
    QAction *actionEditConnection;
    QAction *actionDeleteConnection;
    QAction *actionConnect;
    QAction *actionDisconnect;
    QAction *actionExit;
    QAction *actionAbout;

    ConnectionManager *m_connectionManager;
    SshSession *m_sshSession;
    PasswordManager *m_passwordManager;
    SftpManager *m_sftpManager;
    
    QMap<int, TerminalInfo> m_terminalTabs;
    int m_currentTabIndex;
    
    QString m_currentSftpPath;
};

#endif // MAINWINDOW_H