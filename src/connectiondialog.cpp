#include "connectiondialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    setupConnections();
}

ConnectionDialog::~ConnectionDialog()
{
}

void ConnectionDialog::setupUi()
{
    setWindowTitle("Настройки подключения");
    resize(440, 330);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    QFormLayout *formLayout = new QFormLayout();

    nameEdit = new QLineEdit(this);
    nameEdit->setPlaceholderText("Например: Мой сервер");
    formLayout->addRow("Имя:", nameEdit);

    hostEdit = new QLineEdit(this);
    hostEdit->setPlaceholderText("IP-адрес или домен");
    formLayout->addRow("Хост:", hostEdit);

    portSpinBox = new QSpinBox(this);
    portSpinBox->setRange(1, 65535);
    portSpinBox->setValue(22);
    formLayout->addRow("Порт:", portSpinBox);

    loginEdit = new QLineEdit(this);
    loginEdit->setPlaceholderText("Имя пользователя");
    formLayout->addRow("Логин:", loginEdit);

    passwordEdit = new QLineEdit(this);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText("Пароль (сохранится в защищенном хранилище)");
    formLayout->addRow("Пароль:", passwordEdit);

    useKeyCheckBox = new QCheckBox("Использовать приватный ключ SSH", this);
    formLayout->addRow(useKeyCheckBox);

    QHBoxLayout *keyLayout = new QHBoxLayout();
    keyPathEdit = new QLineEdit(this);
    keyPathEdit->setPlaceholderText("Путь к файлу ключа");
    keyPathEdit->setEnabled(false);
    
    browseKeyButton = new QPushButton("Обзор", this);
    browseKeyButton->setEnabled(false);
    
    keyLayout->addWidget(keyPathEdit);
    keyLayout->addWidget(browseKeyButton);
    
    formLayout->addRow("Ключ:", keyLayout);

    allowLegacyHostKeysCheckBox =
        new QCheckBox("Совместимость со старым SSH-сервером (например, RHEL 6)", this);
    allowLegacyHostKeysCheckBox->setToolTip(
        "Только для этого профиля разрешаются старые алгоритмы SSH (ssh-rsa, SHA-1 KEX/MAC и CBC). "
        "Используйте только для доверенных серверов. ssh-dss не поддерживается современным OpenSSH.");
    formLayout->addRow(allowLegacyHostKeysCheckBox);

    mainLayout->addLayout(formLayout);

    buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttonBox);
}

void ConnectionDialog::setupConnections()
{
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(useKeyCheckBox, &QCheckBox::toggled, passwordEdit, &QLineEdit::setDisabled);
    connect(useKeyCheckBox, &QCheckBox::toggled, keyPathEdit, &QLineEdit::setEnabled);
    connect(useKeyCheckBox, &QCheckBox::toggled, browseKeyButton, &QPushButton::setEnabled);

    connect(browseKeyButton, &QPushButton::clicked, this, &ConnectionDialog::onBrowseKey);
}

void ConnectionDialog::onBrowseKey()
{
    QString fileName = QFileDialog::getOpenFileName(
        this,
        "Выберите приватный ключ SSH",
        QDir::homePath() + "/.ssh",
        "Все файлы (*)"
    );

    if (!fileName.isEmpty()) {
        keyPathEdit->setText(fileName);
    }
}

void ConnectionDialog::setConnection(const Connection &connection)
{
    nameEdit->setText(connection.getName());
    hostEdit->setText(connection.getHost());
    portSpinBox->setValue(connection.getPort());
    loginEdit->setText(connection.getLogin());
    passwordEdit->setText(connection.getPassword());
    useKeyCheckBox->setChecked(connection.getUseKey());
    allowLegacyHostKeysCheckBox->setChecked(connection.getAllowLegacyHostKeys());
    keyPathEdit->setText(connection.getPrivateKeyPath());
}

Connection ConnectionDialog::getConnection() const
{
    Connection conn;
    conn.setName(nameEdit->text());
    conn.setHost(hostEdit->text());
    conn.setPort(portSpinBox->value());
    conn.setLogin(loginEdit->text());
    conn.setPassword(passwordEdit->text());
    conn.setUseKey(useKeyCheckBox->isChecked());
    conn.setPrivateKeyPath(keyPathEdit->text());
    conn.setAllowLegacyHostKeys(allowLegacyHostKeysCheckBox->isChecked());
    return conn;
}