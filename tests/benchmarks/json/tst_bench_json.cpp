// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include <QTest>
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <json.h>

using namespace Json;

class BenchmarkJson: public QObject
{
    Q_OBJECT

public:
    BenchmarkJson() {}

private Q_SLOTS:
    void jsonObjectInsertQt();
    void jsonObjectInsertStd();

    void createBinaryMessageQt();
    void createBinaryMessageStd();

    void readBinaryMessageQt();
    void readBinaryMessageStd();

    void createTextMessageQt();
    void createTextMessageStd();

    void readTextMessageQt();
    void readTextMessageStd();

    void parseJsonQt();
    void parseJsonStd();

    void parseNumbersQt();
    void parseNumbersStd();
};

void BenchmarkJson::parseNumbersQt()
{
    QString testFile = QFINDTESTDATA("numbers.json");
    QVERIFY2(!testFile.isEmpty(), "cannot find test file numbers.json!");
    QFile file(testFile);
    file.open(QFile::ReadOnly);
    QByteArray testJson = file.readAll();

    QBENCHMARK {
        QJsonDocument doc = QJsonDocument::fromJson(testJson);
        doc.object();
    }
}

void BenchmarkJson::parseNumbersStd()
{
    QString testFile = QFINDTESTDATA("numbers.json");
    QVERIFY2(!testFile.isEmpty(), "cannot find test file numbers.json!");
    QFile file(testFile);
    file.open(QFile::ReadOnly);
    std::string testJson = file.readAll().toStdString();

    QBENCHMARK {
        JsonDocument doc = JsonDocument::fromJson(testJson);
        doc.object();
    }
}

void BenchmarkJson::parseJsonQt()
{
    QString testFile = QFINDTESTDATA("test.json");
    QVERIFY2(!testFile.isEmpty(), "cannot find test file test.json!");
    QFile file(testFile);
    file.open(QFile::ReadOnly);
    QByteArray testJson = file.readAll();

    QBENCHMARK {
        QJsonDocument doc = QJsonDocument::fromJson(testJson);
        doc.object();
    }
}

void BenchmarkJson::parseJsonStd()
{
    QString testFile = QFINDTESTDATA("test.json");
    QVERIFY2(!testFile.isEmpty(), "cannot find test file test.json!");
    QFile file(testFile);
    file.open(QFile::ReadOnly);
    std::string testJson = file.readAll().toStdString();

    QBENCHMARK {
        JsonDocument doc = JsonDocument::fromJson(testJson);
        doc.object();
    }
}

void BenchmarkJson::createBinaryMessageQt()
{
    // Example: send information over a datastream to another process
    // Measure performance of creating and processing data into bytearray
    QBENCHMARK {
        QJsonObject ob;
        ob.insert(QStringLiteral("command"), 1);
        ob.insert(QStringLiteral("key"), "some information");
        ob.insert(QStringLiteral("env"), "some environment variables");
        QJsonDocument(ob).toBinaryData();
    }
}

void BenchmarkJson::createBinaryMessageStd()
{
    // Example: send information over a datastream to another process
    // Measure performance of creating and processing data into bytearray
    QBENCHMARK {
        JsonObject ob;
        ob.insert("command", 1);
        ob.insert("key", "some information");
        ob.insert("env", "some environment variables");
        JsonDocument(ob).toBinaryData();
    }
}

void BenchmarkJson::readBinaryMessageQt()
{
    // Example: receive information over a datastream from another process
    // Measure performance of converting content back to QVariantMap
    // We need to recreate the bytearray but here we only want to measure the latter
    QJsonObject ob;
    ob.insert(QStringLiteral("command"), 1);
    ob.insert(QStringLiteral("key"), "some information");
    ob.insert(QStringLiteral("env"), "some environment variables");
    QByteArray msg = QJsonDocument(ob).toBinaryData();

    QBENCHMARK {
        QJsonDocument::fromBinaryData(msg, QJsonDocument::Validate).object();
    }
}

void BenchmarkJson::readBinaryMessageStd()
{
    // Example: receive information over a datastream from another process
    // Measure performance of converting content back to QVariantMap
    // We need to recreate the bytearray but here we only want to measure the latter
    JsonObject ob;
    ob.insert("command", 1);
    ob.insert("key", "some information");
    ob.insert("env", "some environment variables");
    std::string msg = JsonDocument(ob).toBinaryData();

    QBENCHMARK {
        JsonDocument::fromBinaryData(msg, JsonDocument::Validate).object();
    }
}

void BenchmarkJson::createTextMessageQt()
{
    // Example: send information over a datastream to another process
    // Measure performance of creating and processing data into bytearray
    QBENCHMARK {
        QJsonObject ob;
        ob.insert(QStringLiteral("command"), 1);
        ob.insert(QStringLiteral("key"), "some information");
        ob.insert(QStringLiteral("env"), "some environment variables");
        QByteArray msg = QJsonDocument(ob).toJson();
    }
}

void BenchmarkJson::createTextMessageStd()
{
    // Example: send information over a datastream to another process
    // Measure performance of creating and processing data into bytearray
    QBENCHMARK {
        JsonObject ob;
        ob.insert("command", 1);
        ob.insert("key", "some information");
        ob.insert("env", "some environment variables");
        std::string msg = JsonDocument(ob).toJson();
    }
}

void BenchmarkJson::readTextMessageQt()
{
    // Example: receive information over a datastream from another process
    // Measure performance of converting content back to QVariantMap
    // We need to recreate the bytearray but here we only want to measure the latter
    QJsonObject ob;
    ob.insert(QStringLiteral("command"), 1);
    ob.insert(QStringLiteral("key"), "some information");
    ob.insert(QStringLiteral("env"), "some environment variables");
    QByteArray msg = QJsonDocument(ob).toJson();

    QBENCHMARK {
        QJsonDocument::fromJson(msg).object();
    }
}

void BenchmarkJson::readTextMessageStd()
{
    // Example: receive information over a datastream from another process
    // Measure performance of converting content back to QVariantMap
    // We need to recreate the bytearray but here we only want to measure the latter
    JsonObject ob;
    ob.insert("command", 1);
    ob.insert("key", "some information");
    ob.insert("env", "some environment variables");
    std::string msg = JsonDocument(ob).toJson();

    QBENCHMARK {
        JsonDocument::fromJson(msg).object();
    }
}

void BenchmarkJson::jsonObjectInsertQt()
{
    QJsonObject object;
    QJsonValue value(1.5);

    QBENCHMARK {
        for (int i = 0; i < 1000; i++)
            object.insert("testkey_" + QString::number(i), value);
    }
}

void BenchmarkJson::jsonObjectInsertStd()
{
    JsonObject object;
    JsonValue value(1.5);

    QBENCHMARK {
        for (int i = 0; i < 1000; i++)
            object.insert("testkey_" + std::to_string(i), value);
    }
}

QTEST_MAIN(BenchmarkJson)

#include "tst_bench_json.moc"

