#include "remote/qr.h"

#include "qrcodegen.hpp"

#include <cstring>

namespace resindeck::qr {

namespace {
qrcodegen::QrCode encode(const QString &text) {
    return qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::MEDIUM);
}
}  // namespace

QByteArray svg(const QString &text, int scale, int border) {
    const qrcodegen::QrCode qr = encode(text);
    const int size = (qr.getSize() + 2 * border) * scale;
    QByteArray path;
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            if (qr.getModule(x, y))
                path += QString("M%1 %2h%3v%3h-%3z").arg((x + border) * scale).arg((y + border) * scale).arg(scale).toLatin1();
    return QString("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1\" height=\"%1\" viewBox=\"0 0 %1 %1\">"
                   "<rect width=\"%1\" height=\"%1\" fill=\"#fff\"/><path fill=\"#000\" d=\"")
               .arg(size)
               .toLatin1() +
           path + "\"/></svg>";
}

QImage image(const QString &text, int scale, int border) {
    const qrcodegen::QrCode qr = encode(text);
    const int size = (qr.getSize() + 2 * border) * scale;
    QImage img(size, size, QImage::Format_Grayscale8);
    img.fill(255);
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            if (qr.getModule(x, y))
                for (int dy = 0; dy < scale; ++dy)
                    std::memset(img.scanLine((y + border) * scale + dy) + (x + border) * scale, 0, static_cast<std::size_t>(scale));
    return img;
}

}  // namespace resindeck::qr
