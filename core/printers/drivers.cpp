#include "printers/drivers.h"

#include "printers/halot.h"

namespace resindeck {

const QList<DriverType> &driverTypes() {
    static const QList<DriverType> types{
        {halot::HalotDriver::driverInfo(),
         [](const QString &host, const QString &password) { return std::make_unique<halot::HalotDriver>(host, password); },
         [](const QString &host, int timeoutMs) { return halot::HalotDriver::probe(host, timeoutMs); }},
    };
    return types;
}

DriverType driverType(const QString &id) {
    const QString wanted = id.isEmpty() ? QString(kDefaultDriver) : id;
    for (const DriverType &t : driverTypes())
        if (t.info.id == wanted) return t;
    throw PrinterError(QString("Unknown printer type: %1").arg(id), "unknown_driver", {{"driver", id}});
}

}  // namespace resindeck
