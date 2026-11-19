// Creality HALOT resin printers (tested on the HALOT-R6): WebSocket on port 18188 with JSON
// commands. Based on the MIT-licensed https://github.com/mrtska/HalotMageProSharp plus what
// HALOT BOX 4.7 does on the HALOT-R6. Details in docs/PROTOCOL.md.
#pragma once

#include "printers/status.h"

#include <QJsonObject>
#include <QString>

namespace resindeck::halot {

inline constexpr quint16 kPort = 18188;
inline constexpr int kPasswordMax = 8;
inline constexpr qsizetype kChunk = 1 << 20;  // upload chunk size, before compression

// The token the printer expects in every command: the password, PKCS#7-padded and encrypted with
// single DES under a fixed key, in base64. HalotMageProSharp appends a second fixed block for
// passwords over 4 characters; HALOT BOX 4.7 (R6) was seen sending only the 8 DES bytes, so both
// forms are tried. Throws PrinterError("password_length") unless the password is 1–8 ASCII characters.
QString token(const QString &password, bool lowerKey = true);

// The printer's GET_PRINT_STATUS reply in the normalized form. Numbers may come as strings.
Status parseStatus(const QJsonObject &reply);

}  // namespace resindeck::halot
