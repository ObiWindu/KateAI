#include <QString>
#include <iostream>
using namespace Qt::Literals::StringLiterals;
int main() {
    QString s = u"aaaaa"_s;
    std::cout << "\"aaaaa\".count(\"aa\") = " << s.count(u"aa"_s) << "\n";
    QString t = u"aaaa"_s;
    std::cout << "\"aaaa\".count(\"aa\") = " << t.count(u"aa"_s) << "\n";
    return 0;
}
