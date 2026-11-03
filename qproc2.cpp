#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QProcess p;
    p.setWorkingDirectory("/definitely/does/not/exist-xyz");
    bool finishedFired = false;
    QObject::connect(&p, &QProcess::finished, [&](int, QProcess::ExitStatus) {
        finishedFired = true;
        std::cout << "FINISHED fired\n";
    });
    QObject::connect(&p, &QProcess::errorOccurred, [&](QProcess::ProcessError e) {
        std::cout << "ERROROCCURRED fired err=" << e << "\n";
    });
    p.start("/bin/sh", QStringList() << "-c" << "echo hi");
    bool started = p.waitForStarted(3000);
    std::cout << "waitForStarted=" << started << " error=" << p.errorString().toStdString() << "\n";
    QTimer::singleShot(1200, [&]() {
        std::cout << "after wait: finishedFired=" << finishedFired << "\n";
        app.quit();
    });
    return app.exec();
}
