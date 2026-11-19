#include "printers/driver.h"

#include <QJsonArray>

namespace resindeck {

void DriverInfo::checkPassword(const QString &pw) const {
    if (passwordMax && pw.size() > passwordMax)
        throw PrinterError(QString("The password has at most %1 characters.").arg(passwordMax), "password_length",
                           {{"max", passwordMax}});
}

QJsonObject DriverInfo::toJson() const {
    return {{"id", id},
            {"name", name},
            {"models", models},
            {"extensions", QJsonArray::fromStringList(extensions)},
            {"password", password},
            {"passwordMax", passwordMax}};
}

void Driver::needPassword() const {
    if (info().password && password_.isEmpty())
        throw PrinterError("The printer password is not set.", "no_password");
}

}  // namespace resindeck
