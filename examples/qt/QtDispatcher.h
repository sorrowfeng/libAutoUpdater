#pragma once

#include "libAutoUpdater/interfaces/IEventDispatcher.h"

#include <QObject>

class QtDispatcher final : public QObject, public autoupdater::IEventDispatcher {
    Q_OBJECT

public:
    explicit QtDispatcher(QObject* parent = nullptr);
    void post(std::function<void()> fn) noexcept override;
};

