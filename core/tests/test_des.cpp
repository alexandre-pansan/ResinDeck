#include "printers/des.h"

#include <QTest>

using resindeck::des::Block;
using resindeck::des::encrypt;

class TestDes : public QObject {
    Q_OBJECT
private slots:
    void knownVectors_data() {
        QTest::addColumn<QByteArray>("key");
        QTest::addColumn<QByteArray>("plain");
        QTest::addColumn<QByteArray>("cipher");
        // The worked example from "The DES Algorithm Illustrated" (J. Orlin Grabbe)
        QTest::newRow("grabbe") << QByteArray::fromHex("133457799BBCDFF1") << QByteArray::fromHex("0123456789ABCDEF")
                                << QByteArray::fromHex("85E813540F0AB405");
        // NIST SP 800-17 variable plaintext known answer test, first entry
        QTest::newRow("nist-vp1") << QByteArray::fromHex("0101010101010101") << QByteArray::fromHex("8000000000000000")
                                  << QByteArray::fromHex("95F8A5E5DD31D900");
        // NIST SP 800-17 variable key known answer test, first entry
        QTest::newRow("nist-vk1") << QByteArray::fromHex("8001010101010101") << QByteArray::fromHex("0000000000000000")
                                  << QByteArray::fromHex("95A8D72813DAA94D");
    }

    void knownVectors() {
        QFETCH(QByteArray, key);
        QFETCH(QByteArray, plain);
        QFETCH(QByteArray, cipher);
        Block k{}, p{};
        std::copy(key.begin(), key.end(), k.begin());
        std::copy(plain.begin(), plain.end(), p.begin());
        const Block c = encrypt(k, p);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(c.data()), 8).toHex(), cipher.toHex());
    }
};

QTEST_APPLESS_MAIN(TestDes)
#include "test_des.moc"
