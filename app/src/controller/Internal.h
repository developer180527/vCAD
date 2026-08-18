#pragma once

/// Shared by Controller's implementation files, and by nothing else.
///
/// Controller was one 2574-line translation unit; splitting it by concern left a handful of names
/// that every part needs — the document aliases it is written in terms of, and the identity
/// placement transform. They live here rather than being repeated eight times, and here rather than
/// in the public header because no caller has any business with them.
///
/// Deliberately NOT a "Controller internals" grab bag. Anything that belongs to one concern stays
/// in that concern's file; this holds only what genuinely spans all of them.

#include "cad/app/Controller.h"

namespace cad::app {

using document::ObjectId;
using document::ObjectState;

/// Naming serial for a placement's transform. Identity for now: assemblies come later, and a
/// placement that lies about its transform is worse than one that has none.
inline constexpr float kIdentity[12]{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

}  // namespace cad::app
