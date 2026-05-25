#include "pch.h"

#include "include/log.h"
#include "include/runtime_health.h"

namespace qsr::health {
namespace {

std::array<Entry, static_cast<std::size_t>(FeatureId::Count)> g_features{};

}  // namespace

void Reset() {
    for (Entry& entry : g_features) {
        entry = {};
    }
}

void Set(FeatureId feature, State state, const char* note) {
    Entry& entry = g_features[static_cast<std::size_t>(feature)];
    entry.state = state;
    entry.note = note != nullptr ? note : "";
}

const Entry& Get(FeatureId feature) {
    return g_features[static_cast<std::size_t>(feature)];
}

const char* FeatureName(FeatureId feature) {
    switch (feature) {
    case FeatureId::CoreSave:
        return "CoreSave";
    case FeatureId::CoreLoad:
        return "CoreLoad";
    case FeatureId::LoadUi:
        return "LoadUi";
    case FeatureId::Toast:
        return "Toast";
    case FeatureId::Input:
        return "Input";
    default:
        return "Unknown";
    }
}

const char* StateName(State state) {
    switch (state) {
    case State::Unknown:
        return "Unknown";
    case State::Disabled:
        return "Disabled";
    case State::Candidate:
        return "Candidate";
    case State::Ready:
        return "Ready";
    default:
        return "Unknown";
    }
}

void LogSummary() {
    for (std::size_t i = 0; i < g_features.size(); ++i) {
        const FeatureId feature = static_cast<FeatureId>(i);
        const Entry& entry = g_features[i];
        log::Write("[health] feature=%-10s state=%-9s note=%s\n",
            FeatureName(feature),
            StateName(entry.state),
            entry.note.c_str());
    }
}

}  // namespace qsr::health

