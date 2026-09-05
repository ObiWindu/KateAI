/****************************************************************************
** Meta object code from reading C++ file 'agentloop.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../src/agentloop.h"
#include <QtNetwork/QSslError>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'agentloop.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN6KateAi9AgentLoopE_t {};
} // unnamed namespace

template <> constexpr inline auto KateAi::AgentLoop::qt_create_metaobjectdata<qt_meta_tag_ZN6KateAi9AgentLoopE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "KateAi::AgentLoop",
        "userMessage",
        "",
        "text",
        "assistantDelta",
        "delta",
        "assistantFinished",
        "toolStarted",
        "PermissionRequest",
        "request",
        "toolFinished",
        "ToolResult",
        "result",
        "permissionNeeded",
        "statusChanged",
        "status",
        "failed",
        "error",
        "turnFinished"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'userMessage'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'assistantDelta'
        QtMocHelpers::SignalData<void(const QString &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 5 },
        }}),
        // Signal 'assistantFinished'
        QtMocHelpers::SignalData<void(const QString &)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'toolStarted'
        QtMocHelpers::SignalData<void(const PermissionRequest &)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 8, 9 },
        }}),
        // Signal 'toolFinished'
        QtMocHelpers::SignalData<void(const ToolResult &)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 11, 12 },
        }}),
        // Signal 'permissionNeeded'
        QtMocHelpers::SignalData<void(const PermissionRequest &)>(13, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 8, 9 },
        }}),
        // Signal 'statusChanged'
        QtMocHelpers::SignalData<void(const QString &)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 15 },
        }}),
        // Signal 'failed'
        QtMocHelpers::SignalData<void(const QString &)>(16, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 17 },
        }}),
        // Signal 'turnFinished'
        QtMocHelpers::SignalData<void()>(18, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AgentLoop, qt_meta_tag_ZN6KateAi9AgentLoopE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject KateAi::AgentLoop::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6KateAi9AgentLoopE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6KateAi9AgentLoopE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN6KateAi9AgentLoopE_t>.metaTypes,
    nullptr
} };

void KateAi::AgentLoop::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AgentLoop *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->userMessage((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->assistantDelta((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->assistantFinished((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 3: _t->toolStarted((*reinterpret_cast<std::add_pointer_t<PermissionRequest>>(_a[1]))); break;
        case 4: _t->toolFinished((*reinterpret_cast<std::add_pointer_t<ToolResult>>(_a[1]))); break;
        case 5: _t->permissionNeeded((*reinterpret_cast<std::add_pointer_t<PermissionRequest>>(_a[1]))); break;
        case 6: _t->statusChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 7: _t->failed((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 8: _t->turnFinished(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)(const QString & )>(_a, &AgentLoop::userMessage, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)(const QString & )>(_a, &AgentLoop::assistantDelta, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)(const QString & )>(_a, &AgentLoop::assistantFinished, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)(const PermissionRequest & )>(_a, &AgentLoop::toolStarted, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)(const ToolResult & )>(_a, &AgentLoop::toolFinished, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)(const PermissionRequest & )>(_a, &AgentLoop::permissionNeeded, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)(const QString & )>(_a, &AgentLoop::statusChanged, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)(const QString & )>(_a, &AgentLoop::failed, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (AgentLoop::*)()>(_a, &AgentLoop::turnFinished, 8))
            return;
    }
}

const QMetaObject *KateAi::AgentLoop::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *KateAi::AgentLoop::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6KateAi9AgentLoopE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int KateAi::AgentLoop::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void KateAi::AgentLoop::userMessage(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void KateAi::AgentLoop::assistantDelta(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void KateAi::AgentLoop::assistantFinished(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void KateAi::AgentLoop::toolStarted(const PermissionRequest & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void KateAi::AgentLoop::toolFinished(const ToolResult & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void KateAi::AgentLoop::permissionNeeded(const PermissionRequest & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}

// SIGNAL 6
void KateAi::AgentLoop::statusChanged(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void KateAi::AgentLoop::failed(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void KateAi::AgentLoop::turnFinished()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}
QT_WARNING_POP
