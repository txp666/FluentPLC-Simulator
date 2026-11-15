#pragma once

#include "core/address.h"
#include "core/codec.h"
#include "core/project.h"

namespace fplc {

// Parsed once when a project is installed. The server never reparses addresses
// while clients use its registered buffers.
struct PreparedTag {
    Address address;
    DataType type;
    QByteArray initialBytes;
    int sourceIndex = -1;
};

struct ProjectPlan {
    QList<PreparedTag> tags;
    QList<int> evaluationOrder; // A follow source always precedes its dependent.
};

bool prepareProject(const Project &project, ProjectPlan *plan, QString *error = nullptr);

} // namespace fplc
