#ifndef MASTERPASSWORDDIALOG_H
#define MASTERPASSWORDDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

class MasterPasswordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MasterPasswordDialog(QWidget *parent = nullptr);
    ~MasterPasswordDialog();

    QString getMasterPassword() const;
    void setMode(bool isNew); // true = создание, false = ввод существующего

private:
    void setupUi();

    QLineEdit *passwordEdit;
    QLineEdit *confirmEdit;
    QPushButton *okButton;
    QLabel *messageLabel;
    bool m_isNew;
};

#endif // MASTERPASSWORDDIALOG_H
