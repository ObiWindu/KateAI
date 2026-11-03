#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QProcess p;
    bool finishedFired = false;
    QObject::connect(&p, &QProcess::finished, [&](int code, QProcess::ExitStatus st) {
        finishedFired = true;
        std::cout << "FINISHED fired code=" << code << " status=" << st << "\n";
    });
    QObject::connect(&p, &QProcess::errorOccurred, [&](QProcess::ProcessError e) {
        std::cout << "ERROROCCURRED fired err=" << e << "\n";
    });
    p.start("/nonexistent/program", QStringList());
    p.waitForStarted(3000);
    QTimer::singleShot(1500, [&]() {
        std::cout << "after wait: finishedFired=" << finishedFired
                  << " state=" << p.state() << "\n";
        app.quit();
    });
    return app.exec();
}
