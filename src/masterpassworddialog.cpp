#include "masterpassworddialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>

MasterPasswordDialog::MasterPasswordDialog(QWidget *parent)
    : QDialog(parent)
    , m_isNew(false)
{
    setupUi();
}

MasterPasswordDialog::~MasterPasswordDialog()
{
}

void MasterPasswordDialog::setupUi()
{
    setWindowTitle("Мастер-пароль");
    resize(400, 200);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    messageLabel = new QLabel(this);
    mainLayout->addWidget(messageLabel);

    QFormLayout *formLayout = new QFormLayout();

    passwordEdit = new QLineEdit(this);
    passwordEdit->setEchoMode(QLineEdit::Password);
    formLayout->addRow("Мастер-пароль:", passwordEdit);

    confirmEdit = new QLineEdit(this);
    confirmEdit->setEchoMode(QLineEdit::Password);
    confirmEdit->setVisible(false);
    formLayout->addRow("Подтверждение:", confirmEdit);

    mainLayout->addLayout(formLayout);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    okButton = buttonBox->button(QDialogButtonBox::Ok);
    okButton->setEnabled(false);

    mainLayout->addWidget(buttonBox);

    connect(passwordEdit, &QLineEdit::textChanged, this, [this]() {
        if (m_isNew) {
            okButton->setEnabled(!passwordEdit->text().isEmpty() && 
                                 passwordEdit->text() == confirmEdit->text());
        } else {
            okButton->setEnabled(!passwordEdit->text().isEmpty());
        }
    });

    connect(confirmEdit, &QLineEdit::textChanged, this, [this]() {
        okButton->setEnabled(!passwordEdit->text().isEmpty() && 
                             passwordEdit->text() == confirmEdit->text());
    });

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void MasterPasswordDialog::setMode(bool isNew)
{
    m_isNew = isNew;
    
    if (isNew) {
        setWindowTitle("Создание мастер-пароля");
        messageLabel->setText("Создайте мастер-пароль для хранилища паролей.\n"
                              "Он будет использоваться для шифрования всех сохранённых паролей.");
        confirmEdit->setVisible(true);
    } else {
        setWindowTitle("Ввод мастер-пароля");
        messageLabel->setText("Введите мастер-пароль для разблокировки хранилища.");
        confirmEdit->setVisible(false);
    }
}

QString MasterPasswordDialog::getMasterPassword() const
{
    return passwordEdit->text();
}
