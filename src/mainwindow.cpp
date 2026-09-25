#include "mainwindow.h"
#include "logger.h"
#include <QVBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QMenu>
#include <QFont>
#include <QInputDialog>
#include <QFile>
#include <QTimer>
#include <QFileDialog>
#include <QTreeWidgetItem>
#include <QStandardPaths>
#include <QDir>
#include <QStyle>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_connectionManager(new ConnectionManager(this))
    , m_sshSession(new SshSession(this))
    , m_passwordManager(new PasswordManager(this))
    , m_sftpManager(new SftpManager(this))
    , m_currentTabIndex(-1)
{
    LOG_INFO("Запуск приложения LinuxXterm");
    
    setupUi();
    setupMenuBar();
    setupToolBar();
    setupConnections();
    checkSshpassInstalled();
    
    connect(m_sftpManager, &SftpManager::errorOccurred, this, &MainWindow::onSftpError);
    connect(m_sftpManager, &SftpManager::directoryListed, this, &MainWindow::onDirectoryListed);
    
    if (m_passwordManager->hasMasterPassword()) {
        ensureVaultUnlocked();
    }
    
    createTerminal("/bin/bash", QStringList(), "Локальный терминал");
}

MainWindow::~MainWindow()
{
    LOG_INFO("Закрытие приложения");
    if (m_sftpManager->isConnected()) {
        m_sftpManager->disconnect();
    }
}

void MainWindow::setupUi()
{
    setWindowTitle("LinuxXterm");
    resize(1200, 700);

    mainSplitter = new QSplitter(Qt::Horizontal, this);

    // Левая панель с вкладками
    leftPanelTabs = new QTabWidget(this);
    leftPanelTabs->setMinimumWidth(250);
    leftPanelTabs->setMaximumWidth(400);

    // Вкладка "Подключения"
    connectionsTree = new QTreeWidget(this);
    connectionsTree->setHeaderLabels({"Имя", "Хост"});
    connectionsTree->setContextMenuPolicy(Qt::CustomContextMenu);
    leftPanelTabs->addTab(connectionsTree, "Подключения");

    // Вкладка "Файлы"
    fileTreeWidget = new QTreeWidget(this);
    fileTreeWidget->setHeaderLabels({"Имя", "Размер", "Дата"});
    fileTreeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    fileTreeWidget->setRootIsDecorated(false);
    fileTreeWidget->setAlternatingRowColors(true);
    
    QTreeWidgetItem *placeholder = new QTreeWidgetItem();
    placeholder->setText(0, "Подключитесь к серверу");
    fileTreeWidget->addTopLevelItem(placeholder);
    leftPanelTabs->addTab(fileTreeWidget, "Файлы");

    // Вкладки терминалов
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
    
    QFont terminalFont("Monospace", 11);
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
    
    if (oldInfo.statusTimer) {
        oldInfo.statusTimer->stop();
        oldInfo.statusTimer->deleteLater();
    }
    
    if (oldInfo.terminal) {
        delete oldInfo.terminal;
    }
    
    QTimer *newTimer = new QTimer(this);
    newTimer->start(2000);
    
    oldInfo.terminal = newTerminal;
    oldInfo.connectionName = tabName;
    oldInfo.isConnected = isConnected;
    oldInfo.connectionId = connectionId;
    oldInfo.statusTimer = newTimer;
    
    terminalTabs->removeTab(index);
    terminalTabs->insertTab(index, newTerminal, shortenName(tabName));
    terminalTabs->setCurrentIndex(index);
    
    QTimer::singleShot(50, newTerminal, [newTerminal]() {
        newTerminal->setFocus(Qt::OtherFocusReason);
    });
    
    LOG_INFO(QString("Вкладка %1 заменена: %2").arg(index).arg(tabName));
}

void MainWindow::createTerminal(const QString &program, const QStringList &args, const QString &tabName)
{
    QString logArgs = args.join(" ");
    if (program == "/usr/bin/sshpass" && args.size() >= 2 && args[0] == "-p") {
        logArgs = "-p *** " + args.mid(2).join(" ");
    }
    
    LOG_INFO(QString("Создание терминала: %1 %2 (вкладка: %3)").arg(program, logArgs, tabName));

    CustomTermWidget *terminal = new CustomTermWidget(0, this);
    setupTerminalWidget(terminal);

    terminal->setShellProgram(program);
    terminal->setArgs(args);
    terminal->startShellProgram();

    int index = terminalTabs->addTab(terminal, shortenName(tabName));
    terminalTabs->setCurrentIndex(index);

    TerminalInfo info;
    info.terminal = terminal;
    info.connectionId = "";
    info.connectionName = tabName;
    info.isConnected = (program != "/bin/bash");
    
    info.statusTimer = new QTimer(this);
    info.statusTimer->start(2000);
    
    connect(info.statusTimer, &QTimer::timeout, this, [this, index]() {
        if (!m_terminalTabs.contains(index)) {
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

    QTimer::singleShot(50, terminal, [terminal]() {
        terminal->setFocus(Qt::OtherFocusReason);
    });

    LOG_INFO(QString("Вкладка создана: индекс %1, имя %2").arg(index).arg(tabName));
}

void MainWindow::openConnectionInNewTab(const QString &connectionId)
{
    Connection conn = m_connectionManager->getConnection(connectionId);
    if (conn.getId().isEmpty()) {
        LOG_ERR("Подключение не найдено: " + connectionId);
        return;
    }

    LOG_INFO(QString("Открытие подключения в новой вкладке: %1").arg(conn.getName()));
    statusBarWidget->showMessage("Подключение к " + conn.getHost() + "...");

    QString password = m_passwordManager->getPassword(conn.getId());

    QStringList sshArgs;
    sshArgs << "-o" << "StrictHostKeyChecking=accept-new";

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
        finalArgs << "-p" << password << "/usr/bin/ssh" << sshArgs;
        sshArgs = finalArgs;
        LOG_INFO("Используется sshpass с паролем из хранилища");
    } else {
        LOG_INFO("Пароль не найден в хранилище, будет запрошен вручную в терминале");
    }

    createTerminal(program, sshArgs, conn.getName());

    int currentIndex = terminalTabs->currentIndex();
    if (m_terminalTabs.contains(currentIndex)) {
        m_terminalTabs[currentIndex].connectionId = conn.getId();
        m_terminalTabs[currentIndex].connectionName = conn.getName();
        m_terminalTabs[currentIndex].isConnected = true;
    }

    statusBarWidget->showMessage("Подключено к " + conn.getName());
    
    // Инициализируем SFTP
    LOG_INFO("Инициализация SFTP подключения...");
    
    if (m_sftpManager->isConnected()) {
        m_sftpManager->disconnect();
    }
    
    bool sftpConnected = m_sftpManager->connectToHost(
        conn.getHost(),
        conn.getPort(),
        conn.getLogin(),
        password,
        conn.getUseKey() ? conn.getPrivateKeyPath() : QString()
    );
    
    if (sftpConnected) {
        statusBarWidget->showMessage("SFTP подключено, получение домашней директории...");
        // Переключаемся на вкладку "Файлы"
        leftPanelTabs->setCurrentWidget(fileTreeWidget);
        
        // Получаем реальную домашнюю директорию через SSH
        QString homeDir = m_sftpManager->getHomeDirectory();
        if (!homeDir.isEmpty()) {
            refreshSftpDirectory(homeDir);
        } else {
            refreshSftpDirectory("/");
        }
    } else {
        statusBarWidget->showMessage("SFTP недоступно: " + m_sftpManager->lastError(), 5000);
    }
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
    
    // Файловый менеджер - контекстное меню работает и на файлах, и на пустой области
    connect(fileTreeWidget, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onFileDoubleClicked);
    connect(fileTreeWidget, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        // Проверяем, есть ли элемент под курсором
        QTreeWidgetItem *item = fileTreeWidget->itemAt(pos);
        
        // Создаём меню на стеке (безопасно, без вылетов)
        QMenu menu(this);
        
        if (item) {
            // Если кликнули на файл/папку - показываем полное меню
            bool isDir = item->data(0, Qt::UserRole + 1).toBool();
            QString path = item->data(0, Qt::UserRole).toString();
            
            // Пункт "Скачать" только для файлов
            if (!isDir && path != "..") {
                menu.addAction("Скачать", this, &MainWindow::onSftpDownload);
            }
            
            menu.addAction("Загрузить файл в эту папку...", this, &MainWindow::onSftpUpload);
            
            // Пункт "Удалить" только для файлов/папок (не для "..")
            if (path != "..") {
                menu.addAction("Удалить", this, &MainWindow::onSftpDelete);
            }
            
            menu.addSeparator();
            menu.addAction("Создать папку здесь...", this, &MainWindow::onSftpCreateDirectory);
            menu.addSeparator();
            menu.addAction("Обновить", [this]() {
                refreshSftpDirectory(m_currentSftpPath);
            });
        } else {
            // Если кликнули на пустую область - показываем только общие действия
            menu.addAction("Загрузить файл в текущую папку...", this, &MainWindow::onSftpUpload);
            menu.addSeparator();
            menu.addAction("Создать папку здесь...", this, &MainWindow::onSftpCreateDirectory);
            menu.addSeparator();
            menu.addAction("Обновить", [this]() {
                refreshSftpDirectory(m_currentSftpPath);
            });
        }
        
        menu.exec(fileTreeWidget->viewport()->mapToGlobal(pos));
    });
}

void MainWindow::onTabCurrentChanged(int index)
{
    m_currentTabIndex = index;
    if (m_terminalTabs.contains(index)) {
        const TerminalInfo &info = m_terminalTabs[index];
        if (info.isConnected) {
            statusBarWidget->showMessage("Подключено к " + info.connectionName);
        } else {
            statusBarWidget->showMessage("Локальный терминал");
        }
        
        if (info.terminal) {
            info.terminal->setFocus(Qt::OtherFocusReason);
        }
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

    if (info.statusTimer) {
        info.statusTimer->stop();
        info.statusTimer->deleteLater();
    }

    terminalTabs->removeTab(index);
    
    if (info.terminal) {
        delete info.terminal;
    }
    
    m_terminalTabs.remove(index);

    if (info.isConnected && m_sftpManager->isConnected()) {
        m_sftpManager->disconnect();
        fileTreeWidget->clear();
        QTreeWidgetItem *placeholder = new QTreeWidgetItem();
        placeholder->setText(0, "SFTP отключено");
        fileTreeWidget->addTopLevelItem(placeholder);
    }

    if (terminalTabs->count() == 0) {
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
}

void MainWindow::onNewConnection()
{
    LOG_INFO("Открыт диалог добавления подключения");
    ConnectionDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        Connection newConn = dialog.getConnection();
        m_connectionManager->addConnection(newConn);
        
        if (!newConn.getPassword().isEmpty()) {
            if (!m_passwordManager->isUnlocked()) ensureVaultUnlocked();
            if (m_passwordManager->isUnlocked()) {
                m_passwordManager->savePassword(newConn.getId(), newConn.getPassword());
                LOG_INFO("Пароль сохранен в хранилище при создании подключения");
            }
        }
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

    ConnectionDialog dialog(this);
    dialog.setConnection(conn);
    
    if (dialog.exec() == QDialog::Accepted) {
        Connection updatedConn = dialog.getConnection();
        updatedConn.setId(id);
        m_connectionManager->updateConnection(updatedConn);
        
        if (!updatedConn.getPassword().isEmpty()) {
            if (!m_passwordManager->isUnlocked()) ensureVaultUnlocked();
            if (m_passwordManager->isUnlocked()) {
                m_passwordManager->savePassword(id, updatedConn.getPassword());
                LOG_INFO("Пароль обновлен в хранилище");
            }
        }
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
        m_connectionManager->removeConnection(id);
        m_passwordManager->removePassword(id);
        LOG_INFO(QString("Удалено подключение: %1").arg(name));
    }
}

void MainWindow::ensureVaultUnlocked()
{
    if (m_passwordManager->isUnlocked()) return;

    MasterPasswordDialog dialog(this);
    if (m_passwordManager->hasMasterPassword()) {
        dialog.setMode(false);
    } else {
        dialog.setMode(true);
    }

    if (dialog.exec() == QDialog::Accepted) {
        QString pass = dialog.getMasterPassword();
        if (m_passwordManager->hasMasterPassword()) {
            if (!m_passwordManager->unlock(pass)) {
                QMessageBox::warning(this, "Ошибка", "Неверный мастер-пароль");
            }
        } else {
            m_passwordManager->setMasterPassword(pass);
        }
    }
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

    if (info.statusTimer) {
        info.statusTimer->stop();
        info.statusTimer->deleteLater();
    }

    terminalTabs->removeTab(currentIndex);
    
    if (info.terminal) {
        delete info.terminal;
    }
    
    m_terminalTabs.remove(currentIndex);

    if (m_sftpManager->isConnected()) {
        m_sftpManager->disconnect();
        fileTreeWidget->clear();
        QTreeWidgetItem *placeholder = new QTreeWidgetItem();
        placeholder->setText(0, "SFTP отключено");
        fileTreeWidget->addTopLevelItem(placeholder);
    }

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

    if (m_sftpManager->isConnected()) {
        m_sftpManager->disconnect();
        fileTreeWidget->clear();
        QTreeWidgetItem *placeholder = new QTreeWidgetItem();
        placeholder->setText(0, "SFTP отключено (разрыв соединения)");
        fileTreeWidget->addTopLevelItem(placeholder);
    }

    statusBarWidget->showMessage("Соединение разорвано: " + info.connectionName, 5000);

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Соединение разорвано");
    msgBox.setText("SSH-сессия завершена: " + info.connectionName);
    msgBox.setInformativeText("Соединение с сервером было разорвано. Хотите переподключиться?");
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::No);
    msgBox.setIcon(QMessageBox::Warning);

    int ret = msgBox.exec();
    if (ret == QMessageBox::Yes) {
        LOG_INFO("Пользователь выбрал переподключение");
        if (!info.connectionId.isEmpty()) {
            openConnectionInNewTab(info.connectionId);
        }
        if (m_terminalTabs.contains(finishedIndex)) {
            if (info.terminal) delete info.terminal;
            m_terminalTabs.remove(finishedIndex);
        }
    } else {
        LOG_INFO("Замена на локальный терминал");
        
        CustomTermWidget *localTerminal = new CustomTermWidget(0, this);
        setupTerminalWidget(localTerminal);
        localTerminal->setShellProgram("/bin/bash");
        localTerminal->setArgs(QStringList());
        localTerminal->startShellProgram();

        replaceTerminalInTab(finishedIndex, localTerminal, "Локальный терминал", false, "");
    }
}

void MainWindow::onAbout()
{
    QMessageBox::about(this, "О программе", 
        "<h2>LinuxXterm</h2>"
        "<p>Версия 1.0.0</p>"
        "<p>Удобный SSH-клиент для Linux</p>"
        "<p>© 2026</p>"
    );
}

// ===== SFTP МЕТОДЫ =====

void MainWindow::onSftpError(const QString &message)
{
    qWarning() << "SFTP ошибка:" << message;
    statusBarWidget->showMessage("SFTP ошибка: " + message, 5000);
}

void MainWindow::onDirectoryListed(const QString &path, const QList<SftpFileInfo> &files)
{
    m_currentSftpPath = path;
    fileTreeWidget->clear();
    
    // Добавляем пустой элемент для удобного клика правой кнопкой мыши
    QTreeWidgetItem *spacerItem = new QTreeWidgetItem();
    spacerItem->setText(0, " ");
    spacerItem->setData(0, Qt::UserRole, "spacer");
    spacerItem->setData(0, Qt::UserRole + 1, false);
    fileTreeWidget->addTopLevelItem(spacerItem);
    
    // Добавляем ".." для перехода в родительскую директорию (если не в корне)
    if (path != "/" && path != "~") {
        QTreeWidgetItem *parentItem = new QTreeWidgetItem();
        parentItem->setText(0, "..");
        parentItem->setData(0, Qt::UserRole, "..");
        parentItem->setData(0, Qt::UserRole + 1, true); // isDir
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
    if (!item) return;
    
    QString path = item->data(0, Qt::UserRole).toString();
    bool isDir = item->data(0, Qt::UserRole + 1).toBool();
    
    // Обработка ".." - переход в родительскую директорию
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
    QTreeWidgetItem *item = fileTreeWidget->currentItem();
    if (!item) return;
    
    bool isDir = item->data(0, Qt::UserRole + 1).toBool();
    if (isDir) {
        QMessageBox::information(this, "Внимание", "Нельзя скачать папку (только файлы)");
        return;
    }
    
    onFileDoubleClicked(item, 0);
}

void MainWindow::onSftpUpload()
{
    if (!m_sftpManager->isConnected()) {
        QMessageBox::warning(this, "Внимание", "SFTP не подключен");
        return;
    }
    
    QString localPath = QFileDialog::getOpenFileName(
        this,
        "Выберите файл для загрузки",
        QDir::homePath(),
        "Все файлы (*)"
    );
    
    if (localPath.isEmpty()) return;
    
    QFileInfo fileInfo(localPath);
    QString remotePath = m_currentSftpPath.endsWith("/") ? 
        m_currentSftpPath + fileInfo.fileName() : 
        m_currentSftpPath + "/" + fileInfo.fileName();
    
    statusBarWidget->showMessage("Загрузка: " + fileInfo.fileName());
    bool success = m_sftpManager->uploadFile(localPath, remotePath);
    
    if (success) {
        statusBarWidget->showMessage("Файл загружен: " + remotePath, 3000);
        refreshSftpDirectory(m_currentSftpPath);
    } else {
        QMessageBox::warning(this, "Ошибка", "Не удалось загрузить файл:\n" + m_sftpManager->lastError());
    }
}

void MainWindow::onSftpDelete()
{
    QTreeWidgetItem *item = fileTreeWidget->currentItem();
    if (!item) return;
    
    QString path = item->data(0, Qt::UserRole).toString();
    if (path == "..") return; // Нельзя удалить ".."
    
    bool isDir = item->data(0, Qt::UserRole + 1).toBool();
    
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "Подтверждение",
        QString("Удалить %1 \"%2\"?").arg(isDir ? "папку" : "файл").arg(item->text(0)),
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        bool success = m_sftpManager->deleteFile(path);
        if (success) {
            statusBarWidget->showMessage("Удалено: " + path, 3000);
            refreshSftpDirectory(m_currentSftpPath);
        } else {
            QMessageBox::warning(this, "Ошибка", "Не удалось удалить:\n" + m_sftpManager->lastError());
        }
    }
}

void MainWindow::onSftpCreateDirectory()
{
    if (!m_sftpManager->isConnected()) {
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
    if (!m_sftpManager->isConnected()) {
        statusBarWidget->showMessage("SFTP не подключен");
        return;
    }
    
    statusBarWidget->showMessage("Загрузка: " + path);
    m_sftpManager->listDirectory(path);
}