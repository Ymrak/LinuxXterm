#include "mainwindow.h"
#include "logger.h"
#include "legacyssh.h"
#include "sshpassproxy.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QClipboard>
#include <QMenu>
#include <QFont>
#include <QFontDatabase>
#include <QInputDialog>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QFileDialog>
#include <QTreeWidgetItem>
#include <QStandardPaths>
#include <QDir>
#include <QLocale>
#include <QMetaObject>
#include <QStyle>
#include <QMimeData>
#include <QProcessEnvironment>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QProgressBar>
#include <QTabBar>

namespace {
QString formatBytes(qint64 bytes)
{
    static const char *units[] = {"Б", "КБ", "МБ", "ГБ", "ТБ"};
    double amount = qMax<qint64>(bytes, 0);
    int unit = 0;
    while (amount >= 1024.0 && unit < 4) {
        amount /= 1024.0;
        ++unit;
    }
    return QString("%1 %2").arg(QLocale().toString(amount, 'f', unit == 0 ? 0 : 1), units[unit]);
}

const QString remoteMetricsCommand =
    "awk '/^MemTotal:/{t=$2} /^MemAvailable:/{a=$2} "
    "END{printf \"MEM %.0f %.0f\\n\",t*1024,(t-a)*1024}' /proc/meminfo; "
    "awk '/^cpu /{idle=$5+$6; total=0; for(i=2;i<=NF;i++) total+=$i; "
    "printf \"CPU %.0f %.0f\\n\",total,idle}' /proc/stat; "
    "df -P -B1 -x tmpfs -x devtmpfs -x overlay -x squashfs | "
    "awk 'NR>1 && $1 ~ /^\\/dev\\// && $1 !~ /^\\/dev\\/loop/ && "
    "($6 == \"/\" || $6 == \"/home\" || $1 ~ /^\\/dev\\/mapper\\// || $1 ~ /^\\/dev\\/dm-/) "
    "{printf \"DISK %s %s %s %s %s\\n\",$1,$2,$3,$4,$6}'";

void showCopyableError(QWidget *parent, const QString &title, const QString &message)
{
    QMessageBox box(QMessageBox::Critical, title, message, QMessageBox::Ok, parent);
    QPushButton *copyButton = box.addButton("Копировать текст ошибки", QMessageBox::ActionRole);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    box.exec();
    if (box.clickedButton() == copyButton) {
        QApplication::clipboard()->setText(message);
    }
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_connectionManager(new ConnectionManager(this))
    , m_sshSession(new SshSession(this))
    , m_passwordManager(new PasswordManager(this))
    , m_sftpManager(nullptr)
    , m_currentTabIndex(-1)
    , m_metricsTimer(new QTimer(this))
    , m_dragDownloadInProgress(false)
    , m_metricsRequestInFlight(false)
    , m_metricsGeneration(0)
{
    LOG_INFO("Запуск приложения LinuxXterm");
    
    setupUi();
    setupMenuBar();
    setupToolBar();
    setupConnections();
    checkSshpassInstalled();
    m_metricsTimer->setInterval(5000);
    connect(m_metricsTimer, &QTimer::timeout, this, &MainWindow::updateServerMetrics);
    m_metricsTimer->start();
    
    if (m_passwordManager->hasMasterPassword()) {
        ensureVaultUnlocked();
    }
    
    createTerminal("/bin/bash", QStringList(), "Локальный терминал");
}

MainWindow::~MainWindow()
{
    LOG_INFO("Закрытие приложения");
    m_metricsTimer->stop();
    for (QThread *thread : m_workerThreads) {
        if (thread && thread->isRunning()) {
            thread->wait();
        }
    }
    for (const TerminalInfo &info : m_terminalTabs) {
        if (info.sftpManager) {
            info.sftpManager->disconnect();
        }
    }
}

void MainWindow::setupUi()
{
    setWindowTitle("LinuxXterm");
    resize(1200, 700);

    mainSplitter = new QSplitter(Qt::Horizontal, this);

    leftPanelTabs = new QTabWidget(this);
    leftPanelTabs->setMinimumWidth(250);
    leftPanelTabs->setMaximumWidth(400);

    connectionsPanelWidget = new QWidget(this);
    QVBoxLayout *connLayout = new QVBoxLayout(connectionsPanelWidget);
    connLayout->setContentsMargins(2, 2, 2, 2);
    connLayout->setSpacing(2);
    
    searchLineEdit = new QLineEdit(this);
    searchLineEdit->setPlaceholderText("🔍 Поиск по имени или хосту...");
    searchLineEdit->setClearButtonEnabled(true);
    searchLineEdit->setToolTip("Введите текст для фильтрации списка подключений");
    connLayout->addWidget(searchLineEdit);
    
    connectionsTree = new QTreeWidget(this);
    connectionsTree->setHeaderLabels({"Имя", "Хост"});
    connectionsTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connLayout->addWidget(connectionsTree, 1);
    
    leftPanelTabs->addTab(connectionsPanelWidget, "Подключения");

    filePanelWidget = new QWidget(this);
    QVBoxLayout *filePanelLayout = new QVBoxLayout(filePanelWidget);
    filePanelLayout->setContentsMargins(2, 2, 2, 2);
    filePanelLayout->setSpacing(2);
    
    QHBoxLayout *pathLayout = new QHBoxLayout();
    pathLayout->setSpacing(2);
    
    pathLineEdit = new QLineEdit(this);
    pathLineEdit->setPlaceholderText("/путь/к/директории");
    pathLineEdit->setToolTip("Введите путь и нажмите Enter или кнопку");
    pathLayout->addWidget(pathLineEdit, 1);
    
    navigateButton = new QPushButton("→", this);
    navigateButton->setToolTip("Перейти к пути");
    navigateButton->setMaximumWidth(30);
    pathLayout->addWidget(navigateButton);
    
    syncButton = new QPushButton("⟳", this);
    syncButton->setToolTip("Синхронизировать с терминалом (выполнить pwd)");
    syncButton->setMaximumWidth(30);
    pathLayout->addWidget(syncButton);
    
    filePanelLayout->addLayout(pathLayout);
    
    fileTreeWidget = new DropTreeWidget(this);
    fileTreeWidget->setHeaderLabels({"Имя", "Размер", "Дата"});
    fileTreeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    fileTreeWidget->setRootIsDecorated(false);
    fileTreeWidget->setAlternatingRowColors(true);
    fileTreeWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    
    QTreeWidgetItem *placeholder = new QTreeWidgetItem();
    placeholder->setText(0, "Подключитесь к серверу");
    fileTreeWidget->addTopLevelItem(placeholder);
    filePanelLayout->addWidget(fileTreeWidget, 1);
    
    leftPanelTabs->addTab(filePanelWidget, "Файлы");

    terminalTabs = new QTabWidget(this);
    terminalTabs->setTabsClosable(true);
    terminalTabs->setMovable(true);
    terminalTabs->setDocumentMode(true);

    mainSplitter->addWidget(leftPanelTabs);
    mainSplitter->addWidget(terminalTabs);

    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 3);

    setCentralWidget(mainSplitter);

    statusBarWidget = statusBar();
    transferProgressBar = new QProgressBar(statusBarWidget);
    transferProgressBar->setRange(0, 100);
    transferProgressBar->setTextVisible(true);
    transferProgressBar->setMinimumWidth(360);
    transferProgressBar->hide();
    statusBarWidget->addPermanentWidget(transferProgressBar);

    serverInfoLabel = new QLabel("Сервер: не подключен", statusBarWidget);
    serverInfoLabel->setMinimumWidth(0);
    serverInfoLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    serverInfoLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    serverInfoLabel->setTextFormat(Qt::RichText);
    statusBarWidget->addPermanentWidget(serverInfoLabel, 1);
    statusBarWidget->showMessage("Готов к работе");
}

QString MainWindow::shortenName(const QString &name)
{
    if (name.length() > 10) {
        return name.left(10) + "...";
    }
    return name;
}

void MainWindow::setupTerminalWidget(CustomTermWidget *terminal)
{
    if (!terminal) return;
    
    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    terminalFont.setPointSize(11);
    terminalFont.setStyleHint(QFont::Monospace);
    terminal->setTerminalFont(terminalFont);
    terminal->setColorScheme("Linux");
    terminal->setHistorySize(10000);
}

void MainWindow::replaceTerminalInTab(int index, CustomTermWidget *newTerminal, 
                                      const QString &tabName, bool isConnected, 
                                      const QString &connectionId)
{
    if (!m_terminalTabs.contains(index)) {
        qWarning() << "replaceTerminalInTab: индекс" << index << "не найден";
        return;
    }

    TerminalInfo &oldInfo = m_terminalTabs[index];
    terminalTabs->blockSignals(true);
    
    if (oldInfo.statusTimer) {
        oldInfo.statusTimer->stop();
        oldInfo.statusTimer->deleteLater();
    }
    
    if (oldInfo.cwdMonitorTimer) {
        oldInfo.cwdMonitorTimer->stop();
        oldInfo.cwdMonitorTimer->deleteLater();
    }
    
    terminalTabs->removeTab(index);
    if (oldInfo.terminal) {
        QObject::disconnect(oldInfo.terminal, nullptr, this, nullptr);
        delete oldInfo.terminal;
    }
    
    QTimer *newTimer = new QTimer(this);
    newTimer->start(2000);
    
    oldInfo.terminal = newTerminal;
    oldInfo.connectionName = tabName;
    oldInfo.isConnected = isConnected;
    oldInfo.connectionId = connectionId;
    oldInfo.statusTimer = newTimer;
    oldInfo.cwdMonitorTimer = nullptr;
    oldInfo.sftpManager = nullptr;
    oldInfo.sftpPath.clear();
    
    terminalTabs->insertTab(index, newTerminal, shortenName(tabName));
    terminalTabs->setCurrentIndex(index);
    terminalTabs->blockSignals(false);
    connect(newTerminal, &QTermWidget::finished, this, &MainWindow::onTerminalProcessFinished);
    onTabCurrentChanged(index);
    
    QTimer::singleShot(50, newTerminal, [newTerminal]() {
        newTerminal->setFocus(Qt::OtherFocusReason);
    });
    
    LOG_INFO(QString("Вкладка %1 заменена: %2").arg(index).arg(tabName));
}

bool MainWindow::createTerminal(const QString &program, const QStringList &args, const QString &tabName,
                                bool allowLegacyHostKeys, const QString &sshpassPassword)
{
    const QString logArgs = args.join(" ");
    LOG_INFO(QString("Создание терминала: %1 %2 (вкладка: %3)").arg(program, logArgs, tabName));

    CustomTermWidget *terminal = new CustomTermWidget(0, this);
    setupTerminalWidget(terminal);

    QString terminalProgram = program;
    QStringList terminalArgs = args;
    QLocalServer *passwordServer = nullptr;
    QString passwordSocketName;
    if (program == "/usr/bin/sshpass" && !sshpassPassword.isEmpty()) {
        if (sshpassPassword.toUtf8().size() > 1024 * 1024) {
            showCopyableError(this, "Ошибка подключения", "Пароль превышает допустимую длину.");
            delete terminal;
            return false;
        }

        passwordSocketName = createSshpassProxySocketName();
        passwordServer = new QLocalServer(terminal);
        passwordServer->setSocketOptions(QLocalServer::UserAccessOption);
        if (!passwordServer->listen(passwordSocketName)) {
            showCopyableError(
                this,
                "Ошибка подключения",
                "Не удалось создать защищённый канал для передачи пароля SSH."
            );
            delete terminal;
            return false;
        }

        terminalProgram = QCoreApplication::applicationFilePath();
        QStringList sshpassArguments;
        int firstSshpassArgument = !args.isEmpty() && args.first() == "-e" ? 1 : 0;
        for (int i = firstSshpassArgument; i < args.size(); ++i) {
            sshpassArguments.append(args.at(i));
        }
        terminalArgs = makeSshpassProxyArguments(passwordSocketName, program, sshpassArguments);
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove("SSHPASS");
    if (allowLegacyHostKeys) {
        QString configError;
        const QString opensslConfig = legacySshOpenSslConfigPath(&configError);
        if (opensslConfig.isEmpty() && !configError.isEmpty()) {
            showCopyableError(this, "Ошибка настройки совместимости SSH", configError);
            delete terminal;
            return false;
        }
        if (!opensslConfig.isEmpty()) {
            environment.insert("OPENSSL_CONF", opensslConfig);
        }
    }
    if (terminalProgram != program || allowLegacyHostKeys) {
        terminal->setEnvironment(environment.toStringList());
    }

    terminal->setShellProgram(terminalProgram);
    terminal->setArgs(terminalArgs);
    connect(terminal, &QTermWidget::receivedData, this, [this, terminal](const QString &text) {
        for (auto it = m_terminalTabs.begin(); it != m_terminalTabs.end(); ++it) {
            if (it.value().terminal == terminal) {
                it.value().terminalOutput += text;
                constexpr int maxDiagnosticLength = 16000;
                if (it.value().terminalOutput.size() > maxDiagnosticLength) {
                    it.value().terminalOutput.remove(0, it.value().terminalOutput.size() - maxDiagnosticLength);
                }
                const QString lowered = text.toLower();
                if (lowered.contains("permission denied") ||
                    lowered.contains("unable to negotiate") ||
                    lowered.contains("no matching") ||
                    lowered.contains("could not resolve hostname") ||
                    lowered.contains("connection refused") ||
                    lowered.contains("connection timed out") ||
                    lowered.contains("host key verification failed")) {
                    statusBarWidget->showMessage(text.trimmed().right(500), 15000);
                }
                break;
            }
        }
    });
    terminal->startShellProgram();
    if (passwordServer) {
        QLocalSocket *client = waitForSshpassProxy(passwordServer, terminal->getShellPID(), 5000);
        if (!client || !sendSshpassProxyPassword(client, sshpassPassword)) {
            passwordServer->close();
            QLocalServer::removeServer(passwordSocketName);
            showCopyableError(
                this,
                "Ошибка подключения",
                "Не удалось передать пароль вспомогательному процессу SSH."
            );
            delete terminal;
            return false;
        }
        client->disconnectFromServer();
        passwordServer->close();
        QLocalServer::removeServer(passwordSocketName);
        passwordServer->deleteLater();
    }
    connect(terminal, &QTermWidget::finished, this, &MainWindow::onTerminalProcessFinished);

    int index = terminalTabs->addTab(terminal, shortenName(tabName));
    terminalTabs->setCurrentIndex(index);

    TerminalInfo info;
    info.terminal = terminal;
    info.connectionId = "";
    info.connectionName = tabName;
    info.isConnected = (program != "/bin/bash");
    info.sftpManager = nullptr;
    info.sftpPath.clear();
    
    info.statusTimer = new QTimer(this);
    info.statusTimer->start(2000);
    
    info.cwdMonitorTimer = nullptr;
    
    connect(info.statusTimer, &QTimer::timeout, this, [this, terminal]() {
        bool terminalIsOpen = false;
        for (const TerminalInfo &tabInfo : m_terminalTabs) {
            if (tabInfo.terminal == terminal) {
                terminalIsOpen = true;
                break;
            }
        }
        if (!terminalIsOpen) {
            QTimer *timer = qobject_cast<QTimer*>(sender());
            if (timer) {
                timer->stop();
                timer->deleteLater();
            }
            return;
        }
    });
    
    m_terminalTabs[index] = info;
    m_currentTabIndex = index;
    onTabCurrentChanged(index);

    QTimer::singleShot(50, terminal, [terminal]() {
        terminal->setFocus(Qt::OtherFocusReason);
    });

    LOG_INFO(QString("Вкладка создана: индекс %1, имя %2").arg(index).arg(tabName));
    return true;
}

void MainWindow::openConnectionInNewTab(const QString &connectionId)
{
    Connection conn = m_connectionManager->getConnection(connectionId);
    if (conn.getId().isEmpty()) {
        LOG_ERR("Подключение не найдено: " + connectionId);
        return;
    }

    if ((m_passwordManager->hasMasterPassword() && !m_passwordManager->isUnlocked())
        || m_connectionManager->hasPlaintextPasswords()) {
        if (!ensureVaultUnlocked()) {
            return;
        }
        conn = m_connectionManager->getConnection(connectionId);
    }

    LOG_INFO(QString("Открытие подключения в новой вкладке: %1").arg(conn.getName()));
    statusBarWidget->showMessage("Подключение к " + conn.getHost() + "...");

    QString password = m_passwordManager->getPassword(conn.getId());
    if (password.isEmpty() && !conn.getUseKey()) {
        bool passwordAccepted = false;
        password = QInputDialog::getText(
            this,
            "Пароль подключения",
            QString("Введите пароль для %1@%2 (он будет использован для SSH и SFTP):")
                .arg(conn.getLogin(), conn.getHost()),
            QLineEdit::Password,
            QString(),
            &passwordAccepted);
        if (!passwordAccepted || password.isEmpty()) {
            statusBarWidget->showMessage("Подключение отменено: пароль не введён", 5000);
            return;
        }
    }

    QStringList sshArgs;
    sshArgs << "-o" << "StrictHostKeyChecking=accept-new";
    if (conn.getAllowLegacyHostKeys()) {
        sshArgs << legacySshOptions();
    }

    if (conn.getPort() != 22) {
        sshArgs << "-p" << QString::number(conn.getPort());
    }

    if (conn.getUseKey() && !conn.getPrivateKeyPath().isEmpty()) {
        sshArgs << "-i" << conn.getPrivateKeyPath();
    }

    QString userHost = conn.getLogin();
    if (!userHost.isEmpty()) {
        userHost += "@";
    }
    userHost += conn.getHost();
    sshArgs << userHost;

    QString program = "/usr/bin/ssh";
    if (!password.isEmpty()) {
        program = "/usr/bin/sshpass";
        QStringList finalArgs;
        finalArgs << "-e" << "/usr/bin/ssh" << sshArgs;
        sshArgs = finalArgs;
        LOG_INFO("Используется sshpass с паролем из профиля/хранилища");
    } else {
        LOG_INFO("Пароль не найден в хранилище, будет запрошен вручную в терминале");
    }

    if (!createTerminal(program, sshArgs, conn.getName(), conn.getAllowLegacyHostKeys(), password)) {
        return;
    }

    int currentIndex = terminalTabs->currentIndex();
    if (m_terminalTabs.contains(currentIndex)) {
        TerminalInfo &info = m_terminalTabs[currentIndex];
        info.connectionId = conn.getId();
        info.connectionName = conn.getName();
        info.isConnected = true;
        info.connection = conn;
        info.password = password;
    }

    statusBarWidget->showMessage("Подключено к " + conn.getName());
    
    LOG_INFO("Инициализация SFTP подключения...");
    
    SftpManager *sessionSftpManager = new SftpManager(this);
    wireSftpManager(sessionSftpManager);
    if (m_terminalTabs.contains(currentIndex)) {
        m_terminalTabs[currentIndex].sftpManager = sessionSftpManager;
    }
    m_sftpManager = sessionSftpManager;

    bool sftpConnected = sessionSftpManager->connectToHost(
        conn.getHost(),
        conn.getPort(),
        conn.getLogin(),
        password,
        conn.getUseKey() ? conn.getPrivateKeyPath() : QString(),
        conn.getAllowLegacyHostKeys()
    );
    
    if (sftpConnected) {
        statusBarWidget->showMessage("SFTP подключено, получение домашней директории...");
        leftPanelTabs->setCurrentWidget(filePanelWidget);
        
        QString homeDir = sessionSftpManager->getHomeDirectory();
        if (!homeDir.isEmpty()) {
            m_lastKnownCwd = homeDir;
            if (m_terminalTabs.contains(currentIndex)) {
                m_terminalTabs[currentIndex].sftpPath = homeDir;
            }
            refreshSftpDirectory(homeDir);
        } else {
            refreshSftpDirectory("/");
        }
    } else {
        const QString error = QString("Не удалось подключить файловую SFTP-сессию к %1:%2.\n\nПричина: %3")
                                  .arg(conn.getHost())
                                  .arg(conn.getPort())
                                  .arg(sessionSftpManager->lastError());
        statusBarWidget->showMessage("Ошибка SFTP: " + sessionSftpManager->lastError(), 10000);
        setSftpPlaceholder("SFTP недоступно");
        showCopyableError(this, "Ошибка подключения SFTP", error);
    }
}

void MainWindow::startCwdMonitoring(int tabIndex)
{
    if (!m_terminalTabs.contains(tabIndex)) return;
    
    TerminalInfo &info = m_terminalTabs[tabIndex];
    
    if (info.cwdMonitorTimer) {
        info.cwdMonitorTimer->stop();
        delete info.cwdMonitorTimer;
    }
    
    info.cwdMonitorTimer = new QTimer(this);
    info.cwdMonitorTimer->setInterval(2000);
    
    connect(info.cwdMonitorTimer, &QTimer::timeout, this, &MainWindow::onCwdMonitorTimeout);
    info.cwdMonitorTimer->start();
    
    qDebug() << "MainWindow: запущен мониторинг директории для вкладки" << tabIndex;
}

void MainWindow::stopCwdMonitoring(int tabIndex)
{
    if (!m_terminalTabs.contains(tabIndex)) return;
    
    TerminalInfo &info = m_terminalTabs[tabIndex];
    
    if (info.cwdMonitorTimer) {
        info.cwdMonitorTimer->stop();
        delete info.cwdMonitorTimer;
        info.cwdMonitorTimer = nullptr;
    }
    
    qDebug() << "MainWindow: остановлен мониторинг директории для вкладки" << tabIndex;
}

QString MainWindow::readCwdFromFile()
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) return QString();
    
    int currentIndex = terminalTabs->currentIndex();
    if (!m_terminalTabs.contains(currentIndex)) return QString();
    
    CustomTermWidget *terminal = m_terminalTabs[currentIndex].terminal;
    if (!terminal) return QString();
    SftpManager *sftpManager = m_sftpManager;
    
    terminal->sendText("pwd > /tmp/.linuxxterm_cwd\n");
    
    QTimer::singleShot(200, this, [this, sftpManager]() {
        if (m_sftpManager != sftpManager || !sftpManager->isConnected()) {
            return;
        }
        QString tempLocalFile = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/.linuxxterm_cwd_local";
        
        if (sftpManager->downloadFile("/tmp/.linuxxterm_cwd", tempLocalFile)) {
            QFile file(tempLocalFile);
            if (file.open(QIODevice::ReadOnly)) {
                QString cwd = QString::fromUtf8(file.readAll()).trimmed();
                file.close();
                QFile::remove(tempLocalFile);
                
                qDebug() << "MainWindow::readCwdFromFile: текущая директория =" << cwd;
                
                if (!cwd.isEmpty() && cwd != m_lastKnownCwd) {
                    qDebug() << "MainWindow: директория изменилась:" << m_lastKnownCwd << "->" << cwd;
                    m_lastKnownCwd = cwd;
                    refreshSftpDirectory(cwd);
                }
            }
        }
    });
    
    return QString();
}

void MainWindow::onCwdMonitorTimeout()
{
    int currentIndex = terminalTabs->currentIndex();
    if (!m_terminalTabs.contains(currentIndex)) return;
    
    TerminalInfo &info = m_terminalTabs[currentIndex];
    if (!info.isConnected) return;
    
    if (!m_sftpManager || !m_sftpManager->isConnected()) return;
    
    readCwdFromFile();
}

void MainWindow::updateServerMetrics()
{
    if (m_metricsRequestInFlight) {
        return;
    }

    const int index = terminalTabs->currentIndex();
    if (!m_terminalTabs.contains(index)) {
        return;
    }
    const TerminalInfo info = m_terminalTabs[index];
    if (!info.isConnected || !info.sftpManager || !info.sftpManager->isConnected()) {
        return;
    }

    m_metricsRequestInFlight = true;
    const quint64 generation = m_metricsGeneration;
    CustomTermWidget *terminal = info.terminal;
    const Connection connection = info.connection;
    const QString password = info.password;
    const bool allowLegacyHostKeys = connection.getAllowLegacyHostKeys();
    serverInfoLabel->setToolTip("Обновление показателей сервера...");

    QThread *thread = QThread::create(
        [this, terminal, connection, password, generation, allowLegacyHostKeys]() {
            SftpManager metricsManager;
            QString output;
            if (metricsManager.connectToHost(connection.getHost(), connection.getPort(),
                                             connection.getLogin(), password,
                                             connection.getUseKey() ? connection.getPrivateKeyPath()
                                                                    : QString(),
                                             allowLegacyHostKeys)) {
                output = metricsManager.executeCommand(remoteMetricsCommand);
                metricsManager.disconnect();
            } else {
                output = "ERROR " + metricsManager.lastError();
            }

            QMetaObject::invokeMethod(this, [this, terminal, output, generation]() {
                if (generation != m_metricsGeneration) {
                    return;
                }
                m_metricsRequestInFlight = false;
                displayServerMetrics(terminal, output);
            }, Qt::QueuedConnection);
        });

    m_workerThreads.append(thread);
    connect(thread, &QThread::finished, this, [this, thread]() {
        m_workerThreads.removeAll(thread);
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::displayServerMetrics(CustomTermWidget *terminal, const QString &output)
{
    int index = -1;
    for (auto it = m_terminalTabs.cbegin(); it != m_terminalTabs.cend(); ++it) {
        if (it.value().terminal == terminal) {
            index = it.key();
            break;
        }
    }
    if (index < 0 || index != terminalTabs->currentIndex()) {
        return;
    }
    if (output.startsWith("ERROR ")) {
        serverInfoLabel->setText(QString("Сервер: %1 | метрики недоступны").arg(m_terminalTabs[index].connectionName));
        serverInfoLabel->setToolTip(output.mid(6));
        return;
    }

    qint64 memoryTotal = 0;
    qint64 memoryUsed = 0;
    qint64 cpuTotal = 0;
    qint64 cpuIdle = 0;
    QStringList diskSummaries;
    QStringList diskDetails;

    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.split(' ', Qt::SkipEmptyParts);
        if (fields.size() >= 3 && fields[0] == "MEM") {
            memoryTotal = fields[1].toLongLong();
            memoryUsed = fields[2].toLongLong();
        } else if (fields.size() >= 3 && fields[0] == "CPU") {
            cpuTotal = fields[1].toLongLong();
            cpuIdle = fields[2].toLongLong();
        } else if (fields.size() >= 6 && fields[0] == "DISK") {
            const qint64 total = fields[2].toLongLong();
            const qint64 used = fields[3].toLongLong();
            const qint64 available = fields[4].toLongLong();
            const QString mount = fields[5];
            const int usedPercent = total > 0
                                        ? qBound(0, static_cast<int>(used * 100 / total), 100)
                                        : 0;
            const QString color = usedPercent > 95 ? "#ff4d4d"
                                  : usedPercent > 80 ? "#ffad33"
                                                     : "#3fcf70";
            diskSummaries.append(QString("<span style=\"color:%1;font-weight:600\">%2 %3%</span>")
                                     .arg(color, mount.toHtmlEscaped())
                                     .arg(usedPercent));
            diskDetails.append(QString("%1 (%2): занято %3 из %4, свободно %5")
                                   .arg(fields[1], mount, formatBytes(used),
                                        formatBytes(total), formatBytes(available)));
        }
    }

    TerminalInfo &info = m_terminalTabs[index];
    QString cpuText = "--";
    const qint64 totalDelta = cpuTotal - info.previousCpuTotal;
    const qint64 idleDelta = cpuIdle - info.previousCpuIdle;
    if (info.previousCpuTotal > 0 && totalDelta > 0 && idleDelta >= 0) {
        const int cpuPercent = qBound(0, static_cast<int>((totalDelta - idleDelta) * 100 / totalDelta), 100);
        cpuText = QString("%1%").arg(cpuPercent);
    }
    info.previousCpuTotal = cpuTotal;
    info.previousCpuIdle = cpuIdle;

    const QString memoryText = memoryTotal > 0
                                   ? QString("%1 / %2 (%3%)")
                                         .arg(formatBytes(memoryUsed), formatBytes(memoryTotal))
                                         .arg(memoryUsed * 100 / memoryTotal)
                                   : "--";
    const QString disksText = diskSummaries.isEmpty() ? "нет данных" : diskSummaries.join("  ");
    serverInfoLabel->setText(QString("%1 | CPU %2 | RAM %3 | Диски %4")
                                 .arg(info.connectionName.toHtmlEscaped(), cpuText, memoryText, disksText));
    serverInfoLabel->setToolTip(diskDetails.isEmpty() ? "Данные о дисках не получены"
                                                       : diskDetails.join('\n'));
}

void MainWindow::setupMenuBar()
{
    menuFile = menuBar()->addMenu("Файл");
    menuConnections = menuBar()->addMenu("Подключения");
    menuHelp = menuBar()->addMenu("Справка");

    actionNewConnection = new QAction("Добавить", this);
    actionNewConnection->setShortcut(QKeySequence::New);
    menuConnections->addAction(actionNewConnection);

    actionEditConnection = new QAction("Редактировать", this);
    actionEditConnection->setShortcut(QKeySequence(Qt::Key_F2));
    menuConnections->addAction(actionEditConnection);

    actionDeleteConnection = new QAction("Удалить", this);
    actionDeleteConnection->setShortcut(QKeySequence::Delete);
    menuConnections->addAction(actionDeleteConnection);

    menuConnections->addSeparator();

    actionConnect = new QAction("Подключить", this);
    actionConnect->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    menuConnections->addAction(actionConnect);

    actionDisconnect = new QAction("Отключить", this);
    menuConnections->addAction(actionDisconnect);

    actionExit = new QAction("Выход", this);
    actionExit->setShortcut(QKeySequence::Quit);
    menuFile->addAction(actionExit);

    actionAbout = new QAction("О программе", this);
    menuHelp->addAction(actionAbout);
}

void MainWindow::setupToolBar()
{
    mainToolBar = addToolBar("Главная панель");
    mainToolBar->setMovable(false);

    mainToolBar->addAction(actionNewConnection);
    mainToolBar->addAction(actionEditConnection);
    mainToolBar->addAction(actionDeleteConnection);
    mainToolBar->addSeparator();
    mainToolBar->addAction(actionConnect);
    mainToolBar->addAction(actionDisconnect);
}

void MainWindow::setupConnections()
{
    connect(actionNewConnection, &QAction::triggered, this, &MainWindow::onNewConnection);
    connect(actionEditConnection, &QAction::triggered, this, &MainWindow::onEditConnection);
    connect(actionDeleteConnection, &QAction::triggered, this, &MainWindow::onDeleteConnection);
    connect(actionConnect, &QAction::triggered, this, &MainWindow::onConnect);
    connect(actionDisconnect, &QAction::triggered, this, &MainWindow::onDisconnect);
    connect(actionExit, &QAction::triggered, this, &QMainWindow::close);
    connect(actionAbout, &QAction::triggered, this, &MainWindow::onAbout);

    connect(m_connectionManager, &ConnectionManager::connectionsChanged, this, &MainWindow::updateConnectionsList);

    updateConnectionsList();

    connect(searchLineEdit, &QLineEdit::textChanged, this, &MainWindow::filterConnections);

    connect(connectionsTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        Q_UNUSED(column);
        if (item) {
            QString id = item->data(0, Qt::UserRole).toString();
            openConnectionInNewTab(id);
        }
    });

    connect(connectionsTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = connectionsTree->itemAt(pos);
        if (!item) return;

        QMenu contextMenu(this);
        QAction *connectAction = contextMenu.addAction("Подключиться");
        QAction *editAction = contextMenu.addAction("Редактировать");
        QAction *deleteAction = contextMenu.addAction("Удалить из списка");

        QAction *selectedAction = contextMenu.exec(connectionsTree->viewport()->mapToGlobal(pos));

        if (selectedAction == connectAction) {
            openConnectionInNewTab(item->data(0, Qt::UserRole).toString());
        } else if (selectedAction == editAction) {
            onEditConnection();
        } else if (selectedAction == deleteAction) {
            onDeleteConnection();
        }
    });

    connect(terminalTabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onCloseTab);
    connect(terminalTabs, &QTabWidget::currentChanged, this, &MainWindow::onTabCurrentChanged);
    connect(terminalTabs->tabBar(), &QTabBar::tabMoved, this, [this](int, int) {
        reindexTerminalTabs();
        onTabCurrentChanged(terminalTabs->currentIndex());
    });
    
    connect(fileTreeWidget, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onFileDoubleClicked);
    connect(fileTreeWidget, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = fileTreeWidget->itemAt(pos);
        
        QMenu menu(this);
        
        if (item && item->data(0, Qt::UserRole).toString() != "spacer") {
            QList<QTreeWidgetItem*> selectedItems = fileTreeWidget->selectedItems();
            
            selectedItems.removeIf([](QTreeWidgetItem* item) {
                return item->data(0, Qt::UserRole).toString() == "spacer" || 
                       item->data(0, Qt::UserRole).toString() == "..";
            });
            
            bool hasFiles = false;
            for (QTreeWidgetItem* selItem : selectedItems) {
                if (!selItem->data(0, Qt::UserRole + 1).toBool()) {
                    hasFiles = true;
                    break;
                }
            }
            
            if (hasFiles) {
                menu.addAction("Скачать выбранные файлы", this, &MainWindow::onSftpDownload);
            }
            
            menu.addAction("Загрузить файлы сюда...", this, &MainWindow::onSftpUpload);
            
            if (selectedItems.size() > 0) {
                menu.addAction("Удалить выбранные", this, &MainWindow::onSftpDelete);
            }
            
            menu.addSeparator();
            menu.addAction("Создать папку здесь...", this, &MainWindow::onSftpCreateDirectory);
            menu.addSeparator();
            menu.addAction("Обновить", [this]() {
                refreshSftpDirectory(m_currentSftpPath);
            });
        } else {
            menu.addAction("Загрузить файлы сюда...", this, &MainWindow::onSftpUpload);
            menu.addSeparator();
            menu.addAction("Создать папку здесь...", this, &MainWindow::onSftpCreateDirectory);
            menu.addSeparator();
            menu.addAction("Обновить", [this]() {
                refreshSftpDirectory(m_currentSftpPath);
            });
        }
        
        menu.exec(fileTreeWidget->viewport()->mapToGlobal(pos));
    });
    
    connect(fileTreeWidget, &DropTreeWidget::filesDropped, this, &MainWindow::onFilesDropped);
    connect(fileTreeWidget, &DropTreeWidget::requestDragFromServer, this, &MainWindow::onRequestDragFromServer);
    
    connect(navigateButton, &QPushButton::clicked, this, &MainWindow::onNavigateToPath);
    connect(pathLineEdit, &QLineEdit::returnPressed, this, &MainWindow::onNavigateToPath);
    
    connect(syncButton, &QPushButton::clicked, this, &MainWindow::onSyncWithTerminal);
}

void MainWindow::wireSftpManager(SftpManager *manager)
{
    connect(manager, &SftpManager::errorOccurred, this,
            [this, manager](const QString &message) {
        if (m_sftpManager == manager) {
            onSftpError(message);
        }
    });
    connect(manager, &SftpManager::directoryListed, this,
            [this, manager](const QString &path, const QList<SftpFileInfo> &files) {
        if (m_sftpManager == manager) {
            onDirectoryListed(path, files);
        }
    });
    connect(manager, &SftpManager::uploadProgress, this,
            [this, manager](const QString &fileName, qint64 sent, qint64 total) {
        if (m_sftpManager == manager) {
            setTransferProgress(fileName, sent, total, true);
        }
    });
    connect(manager, &SftpManager::downloadProgress, this,
            [this, manager](const QString &fileName, qint64 received, qint64 total) {
        if (m_sftpManager == manager) {
            setTransferProgress(fileName, received, total);
        }
    });
    connect(manager, &SftpManager::operationFinished, this,
            [this, manager](const QString &, bool) {
        if (m_sftpManager == manager && !m_dragDownloadInProgress) {
            transferProgressBar->hide();
        }
    });
}

void MainWindow::setTransferProgress(const QString &fileName, qint64 bytesTransferred,
                                     qint64 bytesTotal, bool upload)
{
    const int percent = bytesTotal > 0
                            ? static_cast<int>(qMin<qint64>(100, bytesTransferred * 100 / bytesTotal))
                            : 0;
    const qint64 remaining = qMax<qint64>(0, bytesTotal - bytesTransferred);
    transferProgressBar->setRange(0, 100);
    transferProgressBar->setValue(percent);
    transferProgressBar->setFormat(
        QString("%1 %2% | скачано/передано: %3 | осталось: %4")
            .arg(upload ? "На сервер" : "С сервера")
            .arg(percent)
            .arg(formatBytes(bytesTransferred))
            .arg(bytesTotal > 0 ? formatBytes(remaining) : QString("?")));
    transferProgressBar->setToolTip(fileName);
    transferProgressBar->show();
    transferProgressBar->repaint();
}

void MainWindow::setSftpPlaceholder(const QString &text)
{
    fileTreeWidget->clear();
    QTreeWidgetItem *placeholder = new QTreeWidgetItem();
    placeholder->setText(0, text);
    fileTreeWidget->addTopLevelItem(placeholder);
    pathLineEdit->clear();
}

void MainWindow::reindexTerminalTabs()
{
    QMap<int, TerminalInfo> reindexed;
    for (int index = 0; index < terminalTabs->count(); ++index) {
        auto *terminal = qobject_cast<CustomTermWidget *>(terminalTabs->widget(index));
        if (!terminal) {
            continue;
        }
        for (auto it = m_terminalTabs.cbegin(); it != m_terminalTabs.cend(); ++it) {
            if (it.value().terminal == terminal) {
                reindexed.insert(index, it.value());
                break;
            }
        }
    }
    m_terminalTabs = reindexed;
}

void MainWindow::filterConnections(const QString &searchText)
{
    QString search = searchText.trimmed().toLower();
    
    for (int i = 0; i < connectionsTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = connectionsTree->topLevelItem(i);
        if (!item) continue;
        
        QString name = item->text(0).toLower();
        QString host = item->text(1).toLower();
        
        bool matches = search.isEmpty() || name.contains(search) || host.contains(search);
        item->setHidden(!matches);
    }
    
    int visibleCount = 0;
    int totalCount = connectionsTree->topLevelItemCount();
    for (int i = 0; i < totalCount; ++i) {
        QTreeWidgetItem *item = connectionsTree->topLevelItem(i);
        if (item && !item->isHidden()) {
            visibleCount++;
        }
    }
    
    if (search.isEmpty()) {
        statusBarWidget->showMessage(QString("Подключения: %1").arg(totalCount), 2000);
    } else {
        statusBarWidget->showMessage(QString("Найдено: %1 из %1 подключений").arg(visibleCount).arg(totalCount), 2000);
    }
}

void MainWindow::onNavigateToPath()
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    
    QString path = pathLineEdit->text().trimmed();
    if (path.isEmpty()) {
        return;
    }
    
    if (!path.startsWith("/")) {
        if (m_currentSftpPath.endsWith("/")) {
            path = m_currentSftpPath + path;
        } else {
            path = m_currentSftpPath + "/" + path;
        }
    }
    
    refreshSftpDirectory(path);
}

void MainWindow::onSyncWithTerminal()
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    
    statusBarWidget->showMessage("Синхронизация с терминалом...");
    
    readCwdFromFile();
}

void MainWindow::onFilesDropped(const QStringList &filePaths)
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    
    if (filePaths.isEmpty()) return;
    
    statusBarWidget->showMessage(QString("Загрузка %1 файл(ов)...").arg(filePaths.size()));
    
    int successCount = 0;
    int failCount = 0;
    
    for (const QString &localPath : filePaths) {
        QFileInfo fileInfo(localPath);
        
        QString remotePath = m_currentSftpPath.endsWith("/") ? 
            m_currentSftpPath + fileInfo.fileName() : 
            m_currentSftpPath + "/" + fileInfo.fileName();
        
        if (m_sftpManager->uploadFile(localPath, remotePath)) {
            successCount++;
        } else {
            failCount++;
        }
    }
    
    if (successCount > 0) {
        statusBarWidget->showMessage(QString("Загружено: %1 файл(ов)").arg(successCount), 3000);
        refreshSftpDirectory(m_currentSftpPath);
    }
    
    if (failCount > 0) {
        QMessageBox::warning(this, "Ошибка", QString("Не удалось загрузить %1 файл(ов)").arg(failCount));
    }
}

void MainWindow::onRequestDragFromServer(const QStringList &remotePaths)
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    if (m_dragDownloadInProgress) {
        statusBarWidget->showMessage("Скачивание для перетаскивания уже выполняется");
        return;
    }
    const int tabIndex = terminalTabs->currentIndex();
    if (!m_terminalTabs.contains(tabIndex)) {
        return;
    }

    const TerminalInfo info = m_terminalTabs[tabIndex];
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (tempDir.isEmpty()) {
        tempDir = QDir::tempPath();
    }
    const QString targetDirectory = tempDir + "/linuxxterm_dragdrop";
    if (!QDir().mkpath(targetDirectory)) {
        QMessageBox::warning(this, "Ошибка", "Не удалось создать временную папку для скачивания");
        return;
    }

    const QStringList paths = remotePaths;
    const Connection connection = info.connection;
    const QString password = info.password;
    const bool allowLegacyHostKeys = connection.getAllowLegacyHostKeys();
    CustomTermWidget *originTerminal = info.terminal;
    m_dragDownloadInProgress = true;
    transferProgressBar->setRange(0, 0);
    transferProgressBar->setFormat("Подключение для скачивания...");
    transferProgressBar->show();
    statusBarWidget->showMessage("Подготовка файлов для перетаскивания — удерживайте левую кнопку мыши");

    QThread *thread = QThread::create(
        [this, paths, targetDirectory, connection, password, allowLegacyHostKeys, originTerminal]() {
            SftpManager transferManager;
            QStringList localPaths;
            QStringList failedPaths;
            QString failureReason;

            QObject::connect(&transferManager, &SftpManager::downloadProgress, this,
                             [this](const QString &fileName, qint64 received, qint64 total) {
                setTransferProgress(fileName, received, total);
            }, Qt::QueuedConnection);

            if (!transferManager.connectToHost(connection.getHost(), connection.getPort(),
                                               connection.getLogin(), password,
                                               connection.getUseKey() ? connection.getPrivateKeyPath()
                                                                      : QString(),
                                               allowLegacyHostKeys)) {
                failureReason = transferManager.lastError();
            } else {
                for (const QString &remotePath : paths) {
                    const QString localPath = targetDirectory + "/" + QFileInfo(remotePath).fileName();
                    if (transferManager.downloadFile(remotePath, localPath)) {
                        localPaths.append(localPath);
                    } else {
                        failedPaths.append(remotePath);
                    }
                }
            }

            transferManager.disconnect();
            QMetaObject::invokeMethod(this,
                                      [this, localPaths, failedPaths, failureReason, originTerminal]() {
                m_dragDownloadInProgress = false;
                transferProgressBar->hide();

                bool originStillOpen = false;
                for (const TerminalInfo &tabInfo : m_terminalTabs) {
                    if (tabInfo.terminal == originTerminal) {
                        originStillOpen = true;
                        break;
                    }
                }
                if (!originStillOpen) {
                    return;
                }
                if (!localPaths.isEmpty()) {
                    fileTreeWidget->startDragWithLocalFiles(localPaths);
                }
                if (!failureReason.isEmpty()) {
                    QMessageBox::warning(this, "Ошибка", "Не удалось подключиться для скачивания:\n" + failureReason);
                } else if (!failedPaths.isEmpty()) {
                    QMessageBox::warning(this, "Ошибка",
                                         QString("Не удалось скачать %1 из %2 файла(ов)")
                                             .arg(failedPaths.size()).arg(failedPaths.size() + localPaths.size()));
                }
            }, Qt::QueuedConnection);
        });

    m_workerThreads.append(thread);
    connect(thread, &QThread::finished, this, [this, thread]() {
        m_workerThreads.removeAll(thread);
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::onTabCurrentChanged(int index)
{
    m_currentTabIndex = index;
    if (m_terminalTabs.contains(index)) {
        const TerminalInfo &info = m_terminalTabs[index];
        m_sftpManager = info.sftpManager;
        ++m_metricsGeneration;
        m_metricsRequestInFlight = false;
        if (info.isConnected) {
            statusBarWidget->showMessage("Подключено к " + info.connectionName);
            if (info.sftpManager && info.sftpManager->isConnected()) {
                if (info.sftpPath.isEmpty()) {
                    m_currentSftpPath = info.sftpManager->getHomeDirectory();
                } else {
                    m_currentSftpPath = info.sftpPath;
                }
                leftPanelTabs->setCurrentWidget(filePanelWidget);
                if (!m_currentSftpPath.isEmpty()) {
                    refreshSftpDirectory(m_currentSftpPath);
                }
            } else {
                setSftpPlaceholder("SFTP недоступно");
            }
        } else {
            statusBarWidget->showMessage("Локальный терминал");
            setSftpPlaceholder("Подключитесь к серверу");
        }
        
        if (info.terminal) {
            info.terminal->setFocus(Qt::OtherFocusReason);
        }
        serverInfoLabel->setText(info.isConnected
                                     ? QString("Сервер: %1 | CPU -- | RAM -- | Диски --")
                                           .arg(info.connectionName.toHtmlEscaped())
                                     : "Сервер: не подключен");
        if (info.isConnected) {
            updateServerMetrics();
        }
    } else {
        m_sftpManager = nullptr;
        ++m_metricsGeneration;
        m_metricsRequestInFlight = false;
        serverInfoLabel->setText("Сервер: не подключен");
        setSftpPlaceholder("Подключитесь к серверу");
    }
}

void MainWindow::onCloseTab(int index)
{
    LOG_INFO(QString("Закрытие вкладки: %1").arg(index));

    if (!m_terminalTabs.contains(index)) return;

    TerminalInfo info = m_terminalTabs[index];
    
    if (info.isConnected) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "Подтверждение",
            "Закрыть подключение к \"" + info.connectionName + "\"?",
            QMessageBox::Yes | QMessageBox::No
        );
        if (reply != QMessageBox::Yes) return;
    }

    removeTerminalTab(index, true);
}

void MainWindow::removeTerminalTab(int index, bool createLocalTab)
{
    if (!m_terminalTabs.contains(index)) {
        return;
    }
    const TerminalInfo info = m_terminalTabs[index];

    if (info.statusTimer) {
        info.statusTimer->stop();
        info.statusTimer->deleteLater();
    }
    
    if (info.cwdMonitorTimer) {
        info.cwdMonitorTimer->stop();
        info.cwdMonitorTimer->deleteLater();
    }

    terminalTabs->blockSignals(true);
    terminalTabs->removeTab(index);
    m_terminalTabs.remove(index);
    reindexTerminalTabs();
    terminalTabs->blockSignals(false);

    if (info.terminal) {
        QObject::disconnect(info.terminal, nullptr, this, nullptr);
        delete info.terminal;
    }

    if (info.sftpManager) {
        if (m_sftpManager == info.sftpManager) {
            m_sftpManager = nullptr;
        }
        info.sftpManager->disconnect();
        info.sftpManager->deleteLater();
    }

    if (terminalTabs->count() > 0) {
        onTabCurrentChanged(terminalTabs->currentIndex());
    } else {
        onTabCurrentChanged(-1);
    }

    if (createLocalTab && terminalTabs->count() == 0) {
        createTerminal("/bin/bash", QStringList(), "Локальный терминал");
    }
}

void MainWindow::updateConnectionsList()
{
    connectionsTree->clear();
    QList<Connection> connections = m_connectionManager->getConnections();
    for (const Connection &conn : connections) {
        QTreeWidgetItem *item = new QTreeWidgetItem();
        item->setText(0, conn.getName());
        item->setText(1, conn.getHost());
        item->setData(0, Qt::UserRole, conn.getId());
        connectionsTree->addTopLevelItem(item);
    }
    
    filterConnections(searchLineEdit->text());
}

void MainWindow::onNewConnection()
{
    LOG_INFO("Открыт диалог добавления подключения");
    ConnectionDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        Connection newConn = dialog.getConnection();
        if (!newConn.getPassword().isEmpty()
            || m_connectionManager->hasPlaintextPasswords()) {
            if (!ensureVaultUnlocked()) {
                return;
            }
        }

        if (!newConn.getPassword().isEmpty()
            && !m_passwordManager->savePassword(newConn.getId(), newConn.getPassword())) {
            QMessageBox::critical(this, "Ошибка", "Не удалось безопасно сохранить пароль подключения.");
            return;
        }
        newConn.setPassword(QString());
        m_connectionManager->addConnection(newConn);
        LOG_INFO(QString("Добавлено подключение: %1").arg(newConn.getName()));
    }
}

void MainWindow::onEditConnection()
{
    QTreeWidgetItem *item = connectionsTree->currentItem();
    if (!item) {
        QMessageBox::warning(this, "Внимание", "Выберите подключение для редактирования");
        return;
    }

    QString id = item->data(0, Qt::UserRole).toString();
    Connection conn = m_connectionManager->getConnection(id);

    if (((m_passwordManager->hasMasterPassword() && !m_passwordManager->isUnlocked())
         || m_connectionManager->hasPlaintextPasswords())
        && !ensureVaultUnlocked()) {
        return;
    }
    conn = m_connectionManager->getConnection(id);
    conn.setPassword(m_passwordManager->getPassword(id));

    ConnectionDialog dialog(this);
    dialog.setConnection(conn);
    
    if (dialog.exec() == QDialog::Accepted) {
        Connection updatedConn = dialog.getConnection();
        updatedConn.setId(id);
        if (!updatedConn.getPassword().isEmpty()
            && !m_passwordManager->isUnlocked()
            && !ensureVaultUnlocked()) {
            return;
        }
        if (!updatedConn.getPassword().isEmpty()
            && !m_passwordManager->savePassword(id, updatedConn.getPassword())) {
            QMessageBox::critical(this, "Ошибка", "Не удалось безопасно сохранить пароль подключения.");
            return;
        }
        updatedConn.setPassword(QString());
        m_connectionManager->updateConnection(updatedConn);
        LOG_INFO(QString("Обновлено подключение: %1").arg(updatedConn.getName()));
    }
}

void MainWindow::onDeleteConnection()
{
    QTreeWidgetItem *item = connectionsTree->currentItem();
    if (!item) {
        QMessageBox::warning(this, "Внимание", "Выберите подключение для удаления");
        return;
    }
    
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Подтверждение", "Удалить подключение \"" + item->text(0) + "\"?",
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        QString id = item->data(0, Qt::UserRole).toString();
        QString name = item->text(0);
        if ((m_passwordManager->hasMasterPassword() && !m_passwordManager->isUnlocked())
            || m_connectionManager->hasPlaintextPasswords()) {
            if (!ensureVaultUnlocked()) {
                return;
            }
        }
        m_connectionManager->removeConnection(id);
        m_passwordManager->removePassword(id);
        LOG_INFO(QString("Удалено подключение: %1").arg(name));
    }
}

bool MainWindow::ensureVaultUnlocked()
{
    if (!m_passwordManager->isUnlocked()) {
        MasterPasswordDialog dialog(this);
        dialog.setMode(m_passwordManager->hasMasterPassword() ? false : true);

        if (dialog.exec() != QDialog::Accepted) {
            return false;
        }

        const QString pass = dialog.getMasterPassword();
        const bool unlocked = m_passwordManager->hasMasterPassword()
            ? m_passwordManager->unlock(pass)
            : m_passwordManager->setMasterPassword(pass);
        if (!unlocked) {
            QMessageBox::warning(this, "Ошибка", "Не удалось проверить мастер-пароль или открыть хранилище.");
            return false;
        }
    }

    if (m_connectionManager->hasPlaintextPasswords()) {
        QMap<QString, QString> legacyPasswords;
        for (const Connection &connection : m_connectionManager->getConnections()) {
            if (!connection.getPassword().isEmpty()) {
                legacyPasswords.insert(connection.getId(), connection.getPassword());
            }
        }
        if (!m_passwordManager->savePasswords(legacyPasswords)
            || !m_connectionManager->clearPlaintextPasswords()) {
            QMessageBox::critical(
                this,
                "Ошибка миграции",
                "Не удалось безопасно перенести все сохранённые пароли. "
                "Исходные профили оставлены без изменений."
            );
            return false;
        }
    }

    return true;
}

void MainWindow::checkSshpassInstalled()
{
    QFile sshpass("/usr/bin/sshpass");
    if (!sshpass.exists()) {
        QMessageBox::warning(this, "Отсутствует sshpass",
            "Для автоматического ввода пароля при SSH-подключении необходима утилита sshpass.\n\n"
            "Установите её командой:\n  Fedora: sudo dnf install sshpass\n  Ubuntu/Debian: sudo apt install sshpass\n\n"
            "Без sshpass пароль придётся вводить вручную в терминале.");
        LOG_ERR("sshpass не установлен в системе");
    } else {
        LOG_INFO("sshpass найден в системе");
    }
}

void MainWindow::onConnect()
{
    QTreeWidgetItem *item = connectionsTree->currentItem();
    if (!item) {
        QMessageBox::warning(this, "Внимание", "Выберите подключение для подключения");
        return;
    }
    openConnectionInNewTab(item->data(0, Qt::UserRole).toString());
}

void MainWindow::onDisconnect()
{
    int currentIndex = terminalTabs->currentIndex();
    if (!m_terminalTabs.contains(currentIndex)) return;

    TerminalInfo info = m_terminalTabs[currentIndex];
    if (!info.isConnected) {
        QMessageBox::information(this, "Внимание", "Текущая вкладка не является SSH-подключением");
        return;
    }

    LOG_INFO(QString("Отключение от сервера: %1").arg(info.connectionName));

    removeTerminalTab(currentIndex, false);
    createTerminal("/bin/bash", QStringList(), "Локальный терминал");
    statusBarWidget->showMessage("Отключено");
}

void MainWindow::onTerminalProcessFinished()
{
    CustomTermWidget *finishedTerminal = qobject_cast<CustomTermWidget*>(sender());
    if (!finishedTerminal) return;

    int finishedIndex = -1;
    for (auto it = m_terminalTabs.begin(); it != m_terminalTabs.end(); ++it) {
        if (it.value().terminal == finishedTerminal) {
            finishedIndex = it.key();
            break;
        }
    }

    if (finishedIndex == -1) return;

    TerminalInfo &info = m_terminalTabs[finishedIndex];
    
    if (!info.isConnected) return;

    LOG_INFO(QString("SSH-процесс завершён (разрыв соединения): %1").arg(info.connectionName));
    info.isConnected = false;

    if (info.statusTimer) {
        info.statusTimer->stop();
        info.statusTimer->deleteLater();
    }
    
    if (info.cwdMonitorTimer) {
        info.cwdMonitorTimer->stop();
        info.cwdMonitorTimer->deleteLater();
    }

    if (info.sftpManager) {
        info.sftpManager->disconnect();
        info.sftpManager->deleteLater();
        if (m_sftpManager == info.sftpManager) {
            m_sftpManager = nullptr;
            ++m_metricsGeneration;
            m_metricsRequestInFlight = false;
            setSftpPlaceholder("SFTP отключено (разрыв соединения)");
            serverInfoLabel->setText("Сервер: " + info.connectionName + " | отключен");
        }
        info.sftpManager = nullptr;
    }

    const QString connectionName = info.connectionName;
    const QString connectionId = info.connectionId;
    statusBarWidget->showMessage("Соединение разорвано: " + connectionName, 5000);

    const QString diagnostic = m_terminalTabs[finishedIndex].terminalOutput.trimmed();
    const QString details = diagnostic.isEmpty()
                                ? QString("SSH-клиент завершился без диагностического текста.\n"
                                          "Проверьте адрес, порт, доступность сервера и параметры SSH.")
                                : diagnostic;
    QMessageBox msgBox(QMessageBox::Warning, "Ошибка SSH-подключения",
                       "SSH-сессия с сервером \"" + connectionName + "\" завершилась.\n"
                       "Подробности доступны ниже. Хотите переподключиться?",
                       QMessageBox::Yes | QMessageBox::No, this);
    msgBox.setDetailedText(details);
    msgBox.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    msgBox.setDefaultButton(QMessageBox::No);
    QPushButton *copyButton = msgBox.addButton("Копировать текст ошибки", QMessageBox::ActionRole);

    int ret;
    do {
        ret = msgBox.exec();
        if (msgBox.clickedButton() == copyButton) {
            QApplication::clipboard()->setText(details);
            statusBarWidget->showMessage("Текст ошибки скопирован в буфер обмена", 5000);
        }
    } while (msgBox.clickedButton() == copyButton);
    if (ret == QMessageBox::Yes) {
        LOG_INFO("Пользователь выбрал переподключение");
        if (!connectionId.isEmpty()) {
            openConnectionInNewTab(connectionId);
        }
        int oldIndex = -1;
        for (auto it = m_terminalTabs.cbegin(); it != m_terminalTabs.cend(); ++it) {
            if (it.value().terminal == finishedTerminal) {
                oldIndex = it.key();
                break;
            }
        }
        if (oldIndex >= 0) {
            removeTerminalTab(oldIndex, false);
        }
    } else {
        LOG_INFO("Замена на локальный терминал");
        
        CustomTermWidget *localTerminal = new CustomTermWidget(0, this);
        setupTerminalWidget(localTerminal);
        localTerminal->setShellProgram("/bin/bash");
        localTerminal->setArgs(QStringList());
        localTerminal->startShellProgram();
        connect(localTerminal, &QTermWidget::finished, this, &MainWindow::onTerminalProcessFinished);

        replaceTerminalInTab(finishedIndex, localTerminal, "Локальный терминал", false, "");
    }
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, "О программе", 
        "<h2>LinuxXterm</h2>"
        "<p>Версия 1.0.3</p>"
        "<p>SSH-терминал и файловый менеджер SFTP для Linux</p>"
        "<p>Автор и издатель: Hellhost</p>"
        "<p>Помощь с разработкой: AI assistant using Copilot SDK in VS Code</p>"
        "<p>© 2026 Hellhost</p>"
    );
}


void MainWindow::onSftpError(const QString &message)
{
    qWarning() << "SFTP ошибка:" << message;
    statusBarWidget->showMessage("SFTP ошибка: " + message, 5000);
}

void MainWindow::onDirectoryListed(const QString &path, const QList<SftpFileInfo> &files)
{
    m_currentSftpPath = path;
    pathLineEdit->setText(path);
    const int currentIndex = terminalTabs->currentIndex();
    if (m_terminalTabs.contains(currentIndex)) {
        m_terminalTabs[currentIndex].sftpPath = path;
    }
    
    fileTreeWidget->clear();
    
    QTreeWidgetItem *spacerItem = new QTreeWidgetItem();
    spacerItem->setText(0, " ");
    spacerItem->setData(0, Qt::UserRole, "spacer");
    spacerItem->setData(0, Qt::UserRole + 1, false);
    fileTreeWidget->addTopLevelItem(spacerItem);
    
    if (path != "/" && path != "~") {
        QTreeWidgetItem *parentItem = new QTreeWidgetItem();
        parentItem->setText(0, "..");
        parentItem->setData(0, Qt::UserRole, "..");
        parentItem->setData(0, Qt::UserRole + 1, true);
        parentItem->setIcon(0, style()->standardIcon(QStyle::SP_ArrowUp));
        fileTreeWidget->addTopLevelItem(parentItem);
    }
    
    for (const SftpFileInfo &file : files) {
        QTreeWidgetItem *item = new QTreeWidgetItem();
        
        if (file.isDir) {
            item->setText(0, file.name);
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            item->setText(1, "-");
        } else {
            item->setText(0, file.name);
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            
            QString sizeStr;
            qint64 size = file.size;
            if (size < 1024) {
                sizeStr = QString("%1 Б").arg(size);
            } else if (size < 1024 * 1024) {
                sizeStr = QString("%1 КБ").arg(size / 1024.0, 0, 'f', 1);
            } else {
                sizeStr = QString("%1 МБ").arg(size / (1024.0 * 1024.0), 0, 'f', 1);
            }
            item->setText(1, sizeStr);
        }
        
        item->setText(2, file.modifiedTime.toString("yyyy-MM-dd hh:mm"));
        item->setData(0, Qt::UserRole, file.path);
        item->setData(0, Qt::UserRole + 1, file.isDir);
        
        fileTreeWidget->addTopLevelItem(item);
    }
    
    fileTreeWidget->resizeColumnToContents(0);
    fileTreeWidget->resizeColumnToContents(1);
    fileTreeWidget->resizeColumnToContents(2);
    
    statusBarWidget->showMessage("SFTP: " + path + " (" + QString::number(files.size()) + " элементов)", 3000);
}

void MainWindow::onFileDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item || !m_sftpManager || !m_sftpManager->isConnected()) return;
    
    QString path = item->data(0, Qt::UserRole).toString();
    bool isDir = item->data(0, Qt::UserRole + 1).toBool();
    
    if (path == "..") {
        QString parentPath = m_currentSftpPath;
        if (parentPath.endsWith("/")) {
            parentPath.chop(1);
        }
        int lastSlash = parentPath.lastIndexOf('/');
        if (lastSlash > 0) {
            parentPath = parentPath.left(lastSlash);
        } else {
            parentPath = "/";
        }
        refreshSftpDirectory(parentPath);
        return;
    }
    
    if (isDir) {
        refreshSftpDirectory(path);
    } else {
        QString fileName = item->text(0);
        QString localPath = QFileDialog::getSaveFileName(
            this,
            "Сохранить файл",
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + fileName,
            "Все файлы (*)"
        );
        
        if (!localPath.isEmpty()) {
            statusBarWidget->showMessage("Скачивание: " + path);
            bool success = m_sftpManager->downloadFile(path, localPath);
            if (success) {
                statusBarWidget->showMessage("Файл скачан: " + localPath, 3000);
            } else {
                QMessageBox::warning(this, "Ошибка", "Не удалось скачать файл:\n" + m_sftpManager->lastError());
            }
        }
    }
}

void MainWindow::onSftpDownload()
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    QList<QTreeWidgetItem*> selectedItems = fileTreeWidget->selectedItems();
    
    selectedItems.removeIf([](QTreeWidgetItem* item) {
        return item->data(0, Qt::UserRole).toString() == "spacer" || 
               item->data(0, Qt::UserRole).toString() == "..";
    });
    
    if (selectedItems.isEmpty()) {
        QMessageBox::information(this, "Внимание", "Выберите файлы для скачивания");
        return;
    }
    
    bool hasFiles = false;
    for (QTreeWidgetItem* item : selectedItems) {
        if (!item->data(0, Qt::UserRole + 1).toBool()) {
            hasFiles = true;
            break;
        }
    }
    
    if (!hasFiles) {
        QMessageBox::information(this, "Внимание", "Нельзя скачать папки (только файлы)");
        return;
    }
    
    QString saveDir = QFileDialog::getExistingDirectory(
        this,
        "Выберите папку для сохранения",
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
    );
    
    if (saveDir.isEmpty()) return;
    
    statusBarWidget->showMessage(QString("Скачивание %1 файл(ов)...").arg(selectedItems.size()));
    
    int successCount = 0;
    int failCount = 0;
    
    for (QTreeWidgetItem* item : selectedItems) {
        bool isDir = item->data(0, Qt::UserRole + 1).toBool();
        if (isDir) continue;
        
        QString path = item->data(0, Qt::UserRole).toString();
        QString fileName = item->text(0);
        QString localPath = saveDir + "/" + fileName;
        
        if (m_sftpManager->downloadFile(path, localPath)) {
            successCount++;
        } else {
            failCount++;
        }
    }
    
    if (successCount > 0) {
        statusBarWidget->showMessage(QString("Скачано: %1 файл(ов)").arg(successCount), 3000);
    }
    
    if (failCount > 0) {
        QMessageBox::warning(this, "Ошибка", QString("Не удалось скачать %1 файл(ов)").arg(failCount));
    }
}

void MainWindow::onSftpUpload()
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    
    QStringList localPaths = QFileDialog::getOpenFileNames(
        this,
        "Выберите файлы для загрузки",
        QDir::homePath(),
        "Все файлы (*)"
    );
    
    if (localPaths.isEmpty()) return;
    
    statusBarWidget->showMessage(QString("Загрузка %1 файл(ов)...").arg(localPaths.size()));
    
    int successCount = 0;
    int failCount = 0;
    
    for (const QString &localPath : localPaths) {
        QFileInfo fileInfo(localPath);
        QString remotePath = m_currentSftpPath.endsWith("/") ? 
            m_currentSftpPath + fileInfo.fileName() : 
            m_currentSftpPath + "/" + fileInfo.fileName();
        
        if (m_sftpManager->uploadFile(localPath, remotePath)) {
            successCount++;
        } else {
            failCount++;
        }
    }
    
    if (successCount > 0) {
        statusBarWidget->showMessage(QString("Загружено: %1 файл(ов)").arg(successCount), 3000);
        refreshSftpDirectory(m_currentSftpPath);
    }
    
    if (failCount > 0) {
        QMessageBox::warning(this, "Ошибка", QString("Не удалось загрузить %1 файл(ов)").arg(failCount));
    }
}

void MainWindow::onSftpDelete()
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    QList<QTreeWidgetItem*> selectedItems = fileTreeWidget->selectedItems();
    
    selectedItems.removeIf([](QTreeWidgetItem* item) {
        return item->data(0, Qt::UserRole).toString() == "spacer" || 
               item->data(0, Qt::UserRole).toString() == "..";
    });
    
    if (selectedItems.isEmpty()) {
        QMessageBox::information(this, "Внимание", "Выберите файлы для удаления");
        return;
    }
    
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "Подтверждение",
        QString("Удалить выбранные элементы (%1)?").arg(selectedItems.size()),
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply != QMessageBox::Yes) return;
    
    int successCount = 0;
    int failCount = 0;
    
    for (QTreeWidgetItem* item : selectedItems) {
        QString path = item->data(0, Qt::UserRole).toString();
        
        if (m_sftpManager->deleteFile(path)) {
            successCount++;
        } else {
            failCount++;
        }
    }
    
    if (successCount > 0) {
        statusBarWidget->showMessage(QString("Удалено: %1 элемент(ов)").arg(successCount), 3000);
        refreshSftpDirectory(m_currentSftpPath);
    }
    
    if (failCount > 0) {
        QMessageBox::warning(this, "Ошибка", QString("Не удалось удалить %1 элемент(ов)").arg(failCount));
    }
}

void MainWindow::onSftpCreateDirectory()
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    
    bool ok;
    QString dirName = QInputDialog::getText(this, "Создать папку", "Имя папки:", QLineEdit::Normal, "", &ok);
    
    if (ok && !dirName.isEmpty()) {
        QString remotePath = m_currentSftpPath.endsWith("/") ? 
            m_currentSftpPath + dirName : 
            m_currentSftpPath + "/" + dirName;
        
        bool success = m_sftpManager->createDirectory(remotePath);
        if (success) {
            statusBarWidget->showMessage("Папка создана: " + remotePath, 3000);
            refreshSftpDirectory(m_currentSftpPath);
        } else {
            QMessageBox::warning(this, "Ошибка", "Не удалось создать папку:\n" + m_sftpManager->lastError());
        }
    }
}

void MainWindow::refreshSftpDirectory(const QString &path)
{
    if (!m_sftpManager || !m_sftpManager->isConnected()) {
        statusBarWidget->showMessage("SFTP не подключен");
        return;
    }
    
    statusBarWidget->showMessage("Загрузка: " + path);
    m_sftpManager->listDirectory(path);
}