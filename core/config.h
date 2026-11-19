// The command line's config file, the same file the Python version (legacy branch) used:
// ~/.config/resindeck/config with HOST=..., PASSWORD=..., DRIVER=... lines.
#pragma once

#include "printers/status.h"

#include <QString>

namespace resindeck {

struct CliConfig {
    QString host;
    QString password;
    QString driver;
};

QString configPath();
// The config file, overridden by RESINDECK_HOST, RESINDECK_PASSWORD and RESINDECK_DRIVER.
CliConfig loadConfig();
// Copies ~/.config/halot-send (the name before the rename) once, when there is no config yet.
void migrateConfig();

// For `resindeck status`:
//   State:     printing (PRINT_PROCESSING)
//   File:      part.cxdlpv4
//   Layer:     50/200 (25%)
//   Remaining: 1h02min
QString formatStatus(const Status &st);

}  // namespace resindeck
