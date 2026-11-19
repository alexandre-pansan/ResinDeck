#include "printers/halot.h"

#include "printers/des.h"

#include <QByteArray>
#include <QHash>
#include <QJsonValue>

namespace resindeck::halot {

namespace {

constexpr des::Block kDesKey{'a', '8', '5', 'e', '9', 'd', '6', '8'};
constexpr des::Block kLowerKey{0xAC, 0x47, 0xB0, 0x02, 0x88, 0x15, 0x6E, 0x5C};

const QHash<QString, State> &states() {
    static const QHash<QString, State> map{
        {"", State::Idle},
        {"PRINT_GENERAL", State::Idle},
        {"PRINT_PROCESSING", State::Printing},
        {"PRINT_PROGRESS", State::Printing},
        {"PRINT_PRINTING", State::Printing},
        {"PRINT_PAUSING", State::Pausing},
        {"PRINT_PAUSE", State::Paused},
        {"PRINT_STOPING", State::Stopping},  // sic, as the firmware spells it
        {"PRINT_STOPPING", State::Stopping},
        {"PRINT_STOP", State::Stopped},
        {"PRINT_COMPLETING", State::Finishing},
        {"PRINT_COMPLETE", State::Finished},
        {"PRINT_END", State::Finished},
        {"PRINT_FAIL", State::Failed},
    };
    return map;
}

int toInt(const QJsonValue &v) {
    if (v.isDouble()) return v.toInt();
    bool ok = false;
    const int n = v.toString().toInt(&ok);
    return ok ? n : 0;
}

}  // namespace

QString token(const QString &password, bool lowerKey) {
    const QByteArray ascii = password.toLatin1();
    bool isAscii = true;
    for (QChar ch : password) isAscii = isAscii && ch.unicode() < 128;
    if (password.isEmpty() || password.size() > kPasswordMax || !isAscii)
        throw PrinterError("The printer password must be 1 to 8 characters long.", "password_length",
                           {{"max", kPasswordMax}});

    // PKCS#7 padding to 8 bytes. Only the first block is sent, so an 8-character password is
    // encrypted as it is (its padding is a whole second block).
    des::Block block;
    block.fill(static_cast<std::uint8_t>(8 - ascii.size() % 8));
    for (qsizetype i = 0; i < ascii.size(); ++i) block[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(ascii[i]);

    const des::Block encrypted = des::encrypt(kDesKey, block);
    QByteArray out(reinterpret_cast<const char *>(encrypted.data()), 8);
    if (lowerKey && password.size() > 4) out.append(reinterpret_cast<const char *>(kLowerKey.data()), 8);
    return QString::fromLatin1(out.toBase64());
}

Status parseStatus(const QJsonObject &reply) {
    Status st;
    st.code = reply.value("printStatus").toString();
    st.state = states().value(st.code, State::Unknown);
    st.file = reply.value("filename").toString();
    st.layer = toInt(reply.value("curSliceLayer"));
    st.layers = toInt(reply.value("sliceLayerCount"));
    st.remaining = toInt(reply.value("printRemainTime"));
    st.raw = reply;
    return st;
}

}  // namespace resindeck::halot
