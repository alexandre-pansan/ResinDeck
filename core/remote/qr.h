// QR codes for the phone pairing dialog, with the vendored qrcodegen library.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace resindeck::qr {

// Medium error correction, `scale` pixels per module, `border` modules of white around it.
QByteArray svg(const QString &text, int scale = 6, int border = 2);
QImage image(const QString &text, int scale = 6, int border = 2);

}  // namespace resindeck::qr
