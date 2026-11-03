#include "sandbox.h"
#include <iostream>
using namespace KateAi;
using namespace Qt::Literals::StringLiterals;
int main() {
    Sandbox box(u"/tmp"_s, SandboxProfile::Workspace);
    QString err;
    auto w = box.wrapCommand(u"git show HEAD:.env"_s, &err);
    std::cout << "wrapCommand('git show HEAD:.env') size=" << w.size()
              << " err=[" << err.toStdString() << "]\n";
    std::cout << "isReadOnlyCommand('git show HEAD:.env') = "
              << box.isReadOnlyCommand(u"git show HEAD:.env"_s) << "\n";
    std::cout << "isReadOnlyCommand('git log -p') = "
              << box.isReadOnlyCommand(u"git log -p"_s) << "\n";
    return 0;
}
