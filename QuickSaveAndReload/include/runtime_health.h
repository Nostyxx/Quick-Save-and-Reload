#pragma once

#include <string>

namespace qsr::health {

enum class FeatureId {
    CoreSave = 0,
    CoreLoad,
    LoadUi,
    Toast,
    Input,
    Count
};

enum class State {
    Unknown = 0,
    Disabled,
    Candidate,
    Ready
};

struct Entry {
    State state = State::Unknown;
    std::string note;
};

void Reset();
void Set(FeatureId feature, State state, const char* note);
const Entry& Get(FeatureId feature);
const char* FeatureName(FeatureId feature);
const char* StateName(State state);
void LogSummary();

}  // namespace qsr::health

