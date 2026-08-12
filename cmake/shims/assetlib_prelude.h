// Force-included into assetlib's translation units. See the comment in the root
// CMakeLists.txt for why this exists and when to delete it.
//
// A real header rather than `-include atomic`: the bare form asks the compiler to find a
// file literally named "atomic" on the include path, which AppleClang does not resolve
// ("fatal error: 'atomic' file not found"). Including it normally from a real header does.
#pragma once
#include <atomic>
