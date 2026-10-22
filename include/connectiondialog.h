#ifndef CONNECTIONDIALOG_H
#define CONNECTIONDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include "connection.h"

class ConnectionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConnectionDialog(QWidget *parent = nullptr);
    ~ConnectionDialog();

    void setConnection(const Connection &connection);
    Connection getConnection() const;

private slots:
    void onBrowseKey();

private:
    void setupUi();
    void setupConnections();

    QLineEdit *nameEdit;
    QLineEdit *hostEdit;
    QSpinBox *portSpinBox;
    QLineEdit *loginEdit;
    QLineEdit *passwordEdit;
    QCheckBox *useKeyCheckBox;
    QCheckBox *allowLegacyHostKeysCheckBox;
    QLineEdit *keyPathEdit;
    QPushButton *browseKeyButton;

    QDialogButtonBox *buttonBox;
};

#endif