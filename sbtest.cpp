#include "sandbox.h"
#include <iostream>
using namespace KateAi;
using namespace Qt::Literals::StringLiterals;

int main() {
    // Empty workspace root, default Workspace profile — as when Kate has no project open.
    Sandbox box(QString(), SandboxProfile::Workspace);
    QString err;
    bool r = box.allowsRead(u"/etc/hostname"_s, &err);
    std::cout << "allowsRead(/etc/hostname) with EMPTY root = " << r
              << " err=[" << err.toStdString() << "]\n";
    QString resolved = box.resolve(u"/etc/hostname"_s, &err);
    std::cout << "resolve() = [" << resolved.toStdString() << "] err=[" << err.toStdString() << "]\n";

    // Non-empty root for contrast.
    Sandbox box2(u"/tmp"_s, SandboxProfile::Workspace);
    QString err2;
    std::cout << "allowsRead(/etc/hostname) with root=/tmp = "
              << box2.allowsRead(u"/etc/hostname"_s, &err2)
              << " err=[" << err2.toStdString() << "]\n";

    // hasWriteFlag long-form bypass check
    Sandbox box3(u"/tmp"_s, SandboxProfile::Workspace);
    std::cout << "isReadOnlyCommand('sort --output=out.txt list.txt') = "
              << box3.isReadOnlyCommand(u"sort --output=out.txt list.txt"_s) << "\n";
    std::cout << "isReadOnlyCommand('sort -o out.txt list.txt') = "
              << box3.isReadOnlyCommand(u"sort -o out.txt list.txt"_s) << "\n";
    std::cout << "isReadOnlyCommand('date --set=12:00') = "
              << box3.isReadOnlyCommand(u"date --set=12:00"_s) << "\n";
    std::cout << "isReadOnlyCommand('tail --follow=name log') = "
              << box3.isReadOnlyCommand(u"tail --follow=name log"_s) << "\n";

    // commandTouchesDeniedPath attached short-flag value
    std::cout << "commandTouchesDeniedPath('tool -I/home/u/.env') = "
              << box3.commandTouchesDeniedPath(u"tool -I/home/u/.env"_s) << "\n";
    std::cout << "commandTouchesDeniedPath('git show HEAD:.env') = "
              << box3.commandTouchesDeniedPath(u"git show HEAD:.env"_s) << "\n";

    // isAlwaysDeniedCommand $HOME spelling
    std::cout << "isAlwaysDeniedCommand('rm -rf $HOME') = "
              << box3.isAlwaysDeniedCommand(u"rm -rf $HOME"_s) << "\n";
    std::cout << "isAlwaysDeniedCommand('rm -rf \"$HOME\"') = "
              << box3.isAlwaysDeniedCommand(u"rm -rf \"$HOME\""_s) << "\n";
    std::cout << "isAlwaysDeniedCommand('rm -rf ~') = "
              << box3.isAlwaysDeniedCommand(u"rm -rf ~"_s) << "\n";
    return 0;
}
