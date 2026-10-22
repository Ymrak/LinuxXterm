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
#include <QList>
#include <QTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QThread>
#include "connectionmanager.h"
#include "connectiondialog.h"
#include "sshsession.h"
#include "passwordmanager.h"
#include "masterpassworddialog.h"
#include "customtermwidget.h"
#include "sftpmanager.h"
#include "droptreewidget.h"

struct TerminalInfo {
    CustomTermWidget *terminal;
    QString connectionId;
    QString connectionName;
    QString terminalOutput;
    bool isConnected;
    QTimer *statusTimer;
    QTimer *cwdMonitorTimer;
    SftpManager *sftpManager = nullptr;
    QString sftpPath;
    Connection connection;
    QString password;
    qint64 previousCpuTotal = 0;
    qint64 previousCpuIdle = 0;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onNewConnection();
    void onEditConnection();
    void onDeleteConnection();
    void onConnect();
    void onDisconnect();
    void onCloseTab(int index);
    void onTabCurrentChanged(int index);
    void onTerminalProcessFinished();
    void onSftpError(const QString &message);
    void onDirectoryListed(const QString &path, const QList<SftpFileInfo> &files);
    void onFileDoubleClicked(QTreeWidgetItem *item, int column);
    void onSftpDownload();
    void onSftpUpload();
    void onSftpDelete();
    void onSftpCreateDirectory();
    void onFilesDropped(const QStringList &filePaths);
    void onNavigateToPath();
    void onSyncWithTerminal();
    void onRequestDragFromServer(const QStringList &remotePaths);
    void onCwdMonitorTimeout();
    void updateServerMetrics();
    void displayServerMetrics(CustomTermWidget *terminal, const QString &output);
    void onAbout();
    
    void filterConnections(const QString &searchText);

private:
    void setupUi();
    void setupMenuBar();
    void setupToolBar();
    void setupConnections();
    void updateConnectionsList();
    void setupTerminalWidget(CustomTermWidget *terminal);
    void replaceTerminalInTab(int index, CustomTermWidget *newTerminal, const QString &tabName, bool isConnected, const QString &connectionId);
    bool createTerminal(const QString &program, const QStringList &args, const QString &tabName,
                        bool allowLegacyHostKeys = false, const QString &sshpassPassword = QString());
    bool ensureVaultUnlocked();
    void checkSshpassInstalled();
    void openConnectionInNewTab(const QString &connectionId);
    QString shortenName(const QString &name);
    void refreshSftpDirectory(const QString &path);
    void startCwdMonitoring(int tabIndex);
    void stopCwdMonitoring(int tabIndex);
    QString readCwdFromFile();
    void reindexTerminalTabs();
    void setTransferProgress(const QString &fileName, qint64 bytesTransferred, qint64 bytesTotal, bool upload = false);
    void setSftpPlaceholder(const QString &text);
    void wireSftpManager(SftpManager *manager);
    void removeTerminalTab(int index, bool createLocalTab);

    QSplitter *mainSplitter;
    QTabWidget *leftPanelTabs;
    
    QWidget *connectionsPanelWidget;
    QLineEdit *searchLineEdit;
    QTreeWidget *connectionsTree;
    
    QWidget *filePanelWidget;
    DropTreeWidget *fileTreeWidget;
    QLineEdit *pathLineEdit;
    QPushButton *navigateButton;
    QPushButton *syncButton;
    
    QTabWidget *terminalTabs;
    QStatusBar *statusBarWidget;
    QProgressBar *transferProgressBar;
    QLabel *serverInfoLabel;

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
    QTimer *m_metricsTimer;
    QList<QThread *> m_workerThreads;
    bool m_dragDownloadInProgress;
    bool m_metricsRequestInFlight;
    quint64 m_metricsGeneration;
    
    QString m_currentSftpPath;
    QString m_lastKnownCwd;
};

#endif