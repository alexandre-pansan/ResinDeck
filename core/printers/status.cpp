#include "printers/status.h"

#include <array>
#include <utility>

namespace resindeck {

namespace {
constexpr std::array<std::pair<State, const char *>, 10> kNames{{
    {State::Idle, "idle"},         {State::Printing, "printing"}, {State::Pausing, "pausing"},
    {State::Paused, "paused"},     {State::Stopping, "stopping"}, {State::Stopped, "stopped"},
    {State::Finishing, "finishing"}, {State::Finished, "finished"}, {State::Failed, "failed"},
    {State::Unknown, "unknown"},
}};
}  // namespace

QString stateName(State state) {
    for (const auto &[s, name] : kNames)
        if (s == state) return QString::fromLatin1(name);
    return QStringLiteral("unknown");
}

State stateFromName(const QString &name) {
    for (const auto &[s, n] : kNames)
        if (name == QLatin1String(n)) return s;
    return State::Unknown;
}

bool Status::ready() const {
    return state == State::Idle || state == State::Stopped || state == State::Finished || state == State::Failed;
}

QJsonObject Status::toJson() const {
    return {{"state", stateName(state)}, {"file", file},     {"layer", layer},
            {"layers", layers},          {"remaining", remaining}, {"code", code}};
}

PrinterError::PrinterError(const QString &message, QString code, QVariantMap params)
    : std::runtime_error(message.toStdString()), code_(std::move(code)), params_(std::move(params)) {}

}  // namespace resindeck
