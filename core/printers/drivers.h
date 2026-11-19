// The printer families ResinDeck supports. Adding a driver is one entry in drivers.cpp.
#pragma once

#include "printers/driver.h"

#include <QList>
#include <QString>

#include <functional>
#include <memory>

namespace resindeck {

struct DriverType {
    const DriverInfo &info;
    std::function<std::unique_ptr<Driver>(const QString &host, const QString &password)> create;
    // True if a printer of this family answers at `host`. Quick and without a password,
    // for "Search the network".
    std::function<bool(const QString &host, int timeoutMs)> probe;
};

inline constexpr auto kDefaultDriver = "halot";

const QList<DriverType> &driverTypes();
// The driver with this id; an empty id gives the default. Throws PrinterError("unknown_driver").
DriverType driverType(const QString &id);

}  // namespace resindeck
