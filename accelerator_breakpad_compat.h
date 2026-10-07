// AcceleratorLocal compatibility with Google Breakpad 6598c9c33fc02da7805401f3b0f1a733e6a24071.
// SVAROG fork changes under the original GNU GPL version 3.0.
#pragma once

#include <memory>

// Modern Breakpad removed scoped_ptr; preserve the original writer's owning
// local variable spelling without modifying Breakpad or the writer body.
namespace google_breakpad {
template<class T>
using scoped_ptr = std::unique_ptr<T>;
}
