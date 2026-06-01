#include "QtDispatcher.h"

#include <QMetaObject>

QtDispatcher::QtDispatcher(QObject* parent) : QObject(parent) {}

void QtDispatcher::post(std::function<void()> fn) noexcept {
    QMetaObject::invokeMethod(this, [fn = std::move(fn)] {
        if (fn) {
            fn();
        }
    }, Qt::QueuedConnection);
}

