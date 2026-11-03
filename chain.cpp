#include "sandbox.h"
#include <iostream>
using namespace KateAi;
using namespace Qt::Literals::StringLiterals;
int main() {
    Sandbox box(u"/tmp/proj"_s, SandboxProfile::Workspace);
    QString err;
    // Attaching the value to -o: both guards must fail for this to auto-approve
    // a write to a deny-protected path.
    const QString cmd = u"sort -o.kateai/mcp.json list.txt"_s;
    std::cout << "isReadOnlyCommand  = " << box.isReadOnlyCommand(cmd) << "  (1 = auto-approved in Ask mode)\n";
    std::cout << "commandTouchesDeniedPath = " << box.commandTouchesDeniedPath(cmd) << "  (1 = refused)\n";
    auto w = box.wrapCommand(cmd, &err);
    std::cout << "wrapCommand size = " << w.size() << " err=[" << err.toStdString() << "]\n";
    // Same write with the space-separated form is caught by both guards:
    const QString cmd2 = u"sort -o .kateai/mcp.json list.txt"_s;
    std::cout << "space form: isReadOnlyCommand=" << box.isReadOnlyCommand(cmd2)
              << " touchesDenied=" << box.commandTouchesDeniedPath(cmd2) << "\n";
    return 0;
}
