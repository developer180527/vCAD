#pragma once

#include "cad/document/Document.h"
#include "cad/kernel/Result.h"

#include <string>

namespace cad::io {

/// Serialises a computed feature output — shape plus element map — to a byte string, and
/// back.
///
/// This is what makes the DDC's disk and shared tiers possible: an in-memory `Output` is a
/// live OCCT handle graph and cannot be written anywhere.
///
/// The element map travels WITH the shape, and that is not optional. A cached shape without
/// its names is worse than a cache miss: every downstream reference into it would fail to
/// resolve, so the cache would silently break exactly the guarantee M1 exists to provide.
///
/// Format is versioned. An older blob whose version we no longer read is treated as a cache
/// miss, never as an error — a stale cache entry must never be able to fail a build.
kernel::Result<std::string> serialize(const document::Output&);
kernel::Result<document::Output> deserialize(const std::string& bytes);

/// Bumped whenever the encoding changes. Folded into every DDC key, so old blobs become
/// unreachable rather than misread.
constexpr std::uint32_t kSerializationVersion = 1;

}  // namespace cad::io
