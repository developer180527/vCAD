# Code Review Findings & Issues Report

## 1. 🔴 CRITICAL: Qt UI State Desynchronization & Memory Leak on Tab Close

- **Files**: `shell_qt/src/MainWindow.cpp`, `shell_qt/src/MainWindow.h`
- **Impact**: UI crash / Segfault / Memory corruption upon closing document tabs.

### Description

In `MainWindow::createDocument()`, each new document creates both a 3D viewport
placeholder and a 2D sketch canvas:

`cpp`

`sketchCanvases_.push_back(canvas);`

`editors_.push_back(editor);`

However, in `MainWindow::buildWorkspaces()` inside `documentTabs_`'s `
tabCloseRequested` signal handler:

`cpp`

`connect(documentTabs_, \&QTabBar::tabCloseRequested, this, \[this](<int index>)
{`

`    if (index <= 0) return;`

`    const auto docIndex = static_cast\<std::size_t>(index - 1);`

`    auto* editor = editors_\[docIndex];`

`    editors_.erase(editors_.begin() + static_cast\<std::ptrdiff_t>(docIndex));`

`    workspaces_->removeWidget(editor);`

`    editor->deleteLater();`

`    session_.close(docIndex);`

`});`

Notice that `sketchCanvases_` is **never updated or cleaned up** when a tab is
closed!

1.  `sketchCanvases_\[docIndex]` remains dangling in `workspaces_` and inside
    the `sketchCanvases_` vector.
2.  The index mapping between `editors_`, `sketchCanvases_`, and `
    session_.documents()` is destroyed as soon as any tab other than the last
    tab is closed.
3.  Subsequent workspace switching in `syncWorkspace()` (e.g. `
    sketchCanvases_\[index]`) accesses out-of-bounds or wrong canvas widgets,
    resulting in memory leaks, visual rendering bugs, or application crashes.

### Recommendation

In `tabCloseRequested`, remove and delete `sketchCanvases_\[docIndex]` alongside `
editors_\[docIndex]`:

`cpp`

`auto* canvas = sketchCanvases_\[docIndex];`

`sketchCanvases_.erase(sketchCanvases_.begin() +
static_cast\<std::ptrdiff_t>(docIndex));`

`workspaces_->removeWidget(canvas);`

`canvas->deleteLater();`

- - -
## 2. 🔴 CRITICAL: Geometric Solver Desynchronization on Arc Angles

- **File**: `core/sketch/src/Sketch.cpp`
- **Impact**: Corrupted, distorted, or failing 3D extrusion solids built from
  solved 2D sketches containing arcs.

### Description

In `Sketch::solve()`, planegcs constraints update arc parameters (`center.x`, `
center.y`, `rad`), as well as arc endpoint coordinates (`a.start.x/y` and `
a.end.x/y`). However, `a.startAngle` (`g.p\[3]`) and `a.endAngle` (`g.p\[4]`)
are NOT solver variables that planegcs dynamically updates when endpoint
constraints move:

`cpp`

`case GeoKind::Arc:`

`    g.p\[0] = \*arcs[m.index].center.x;`

`    g.p\[1] = \*arcs[m.index].center.y;`

`    g.p\[2] = \*arcs[m.index].rad;`

`    g.p\[3] = \*arcs[m.index].startAngle;  // STALE!`

`    g.p\[4] = \*arcs[m.index].endAngle;    // STALE!`

`    break;`

When building 3D wires in `toWire()`:

`cpp`

`const double mid = (g.p\[3] + g.p\[4]) * 0.5;`

`const gp_Pnt through = point(g.p\[0] + g.p\[2] * std::cos(mid),`

`                             g.p\[1] + g.p\[2] * std::sin(mid));`

`GC_MakeArcOfCircle arc(start, through, end);`

Because `g.p\[3]` and `g.p\[4]` remain un-updated from their initial placement
values, `through` is computed at an obsolete angle relative to the solved `start`
and `end` points. `GC_MakeArcOfCircle` produces an inverted arc or fails `
toWire()`, causing extrusion features to fail unexpectedly.

### Recommendation

After solving, recompute `startAngle` and `endAngle` from the solved endpoint
positions `a.start` and `a.end` relative to `a.center`:

`cpp`

`const double dx1 = \*arcs[m.index].start.x - \*arcs[m.index].center.x;`

`const double dy1 = \*arcs[m.index].start.y - \*arcs[m.index].center.y;`

`const double dx2 = \*arcs[m.index].end.x - \*arcs[m.index].center.x;`

`const double dy2 = \*arcs[m.index].end.y - \*arcs[m.index].center.y;`

`g.p\[3] = std::atan2(dy1, dx1);`

`g.p\[4] = std::atan2(dy2, dx2);`

- - -
## 3. 🟠 MAJOR: Unhandled `std::stoull` Exception in Element Name Parser

- **Files**: `core/naming/src/ElementName.cpp`, `abi/src/Session.cpp`
- **Impact**: Severe error-handling contract violation in C ABI and document
  loading.

### Description

`ElementName::parse(std::string_view text)` is documented to safely return a
null `ElementName` (`isNull() == true`) when parsing invalid text formats.

However, line 148 calls `std::stoull` directly:

`cpp`

`step.parents.push_back(std::stoull(item, nullptr, 16));`

If `item` contains invalid characters or overflows 64-bit integer limits, `
std::stoull` throws `std::invalid_argument` or `std::out_of_range`.

When `cad_object_set_element` calls `ElementName::parse(elementName)` via the C
ABI, passing invalid or truncated text triggers an unhandled `std::exception`
caught by `withSession`, turning what should be a clean `CAD_ERR_INVALID_INPUT`
validation error into a generic `CAD_ERR_INTERNAL` system exception.

### Recommendation

Wrap `std::stoull` in a `try/catch` or use `std::from_chars` (`\<charconv>`) for
non-throwing hex parsing:

`cpp`

`std::uint64_t val = 0;`

`auto \[ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), val,
16);`

`if (ec != std::errc{}) return {};`

`step.parents.push_back(val);`

- - -
## 4. 🟠 MAJOR: DXF Importer Missing Bulge Counter & Silent Curvature Loss

- **File**: `core/sketch/src/Dxf.cpp`
- **Impact**: Loss of diagnostic feedback during file interchange.

### Description

The comment in `Dxf.cpp` for `LWPOLYLINE` entities states:

> *"Bulges make a polyline segment an arc... Flattened and COUNTED, so the
> report can say the profile lost curvature."*

However, when processing vertices in `LWPOLYLINE`, `ctx.report.flattenedBulges`
is **never incremented**! `ctx.report.flattenedBulges` remains `0`, so `
DxfImportReport::summary()` never warns the user that curved polyline segments
were converted to straight lines.

### Recommendation

Check `poly->getBulge(i) != 0.0` when reading polyline vertices and increment `
ctx.report.flattenedBulges++`.

- - -
## 5. 🟡 MEDIUM: Recompute Cache Key Collisions for Uncomputed Dependency References

- **File**: `core/recompute/src/Engine.cpp`
- **Impact**: Potential stale cache hits in complex feature DAGs.

### Description

In `Engine::cacheKeyOf()`, property values of type `ObjectId` are resolved and
mixed into the cache key:

`cpp`

`if (const auto* ref = std::get_if\<document::ObjectId>(&p.value)) {`

`    const auto target = doc.find(\*ref);`

`    if (\!target) return Error{...};`

`    mix(h, target->cacheKey());`

`}`

If `target` is in `Dirty` state or hasn't had output computed yet, `
target->cacheKey()` is `0`. If feature `A` points to target `B` (dirty, `
cacheKey() == 0`), and later points to target `C` (dirty, `cacheKey() == 0`), `
cacheKeyOf(A)` yields the **same cache key**. If `B` changes state, `A` may
incorrectly hit the cache with outdated output.

### Recommendation

Mix the target object's ID into the hash alongside its cache key:

`cpp`

`mix(h, ref->value);`

`mix(h, target->cacheKey());`

- - -
## 6. 🟡 MEDIUM: Global Mutex Lock Granularity in C ABI

- **File**: `abi/src/Session.cpp`
- **Impact**: Poor scaling under multi-session / multi-threaded workloads.

### Description

`withSession` and `withSessionStr` use a single global mutex:

`cpp`

`std::mutex g_mutex;`

Every exported API function locks `g_mutex`. If session 1 executes a
long-running CPU computation (e.g. `cad_recompute`, `cad_sketch_solve`, or `
cad_object_tessellate`), session 2 on another thread is completely blocked
waiting for `g_mutex`.

### Recommendation

Use `g_mutex` only to protect insertion/removal in `g_sessions`, and store a
per-session `std::mutex` inside each `Session` instance.

- - -
## 7. 🟡 MEDIUM: Const-Cast Mutation of Shared OCCT Shapes

- **Files**: `core/kernel/src/Booleans.cpp`, `core/kernel/src/Fillet.cpp`
- **Impact**: Potential data race when sharing `Shape` across threads.

### Description

Kernel operations take `const Shape&` but cast away constness: `
occt(const_cast\<Shape&>(a))`. OCCT algorithm methods like `Build()` mutate
internal flags, bound caches, and location attributes on `TopoDS_Shape`. Because `
Document` objects are persistent and shared by `std::shared_ptr` across undo
steps and worker threads, mutating `TopoDS_Shape` via `const_cast` introduces
data races.

### Recommendation

Avoid `const_cast` on shared shapes when invoking OCCT builders, or ensure `Shape`
creates a local copy (`a.occtShape()`) before mutating operations.

- - -
## 8. 🔵 MINOR: Duplicate Document Recompute Execution on File Open

- **Files**: `abi/src/Session.cpp`, `app/src/Controller.cpp`, `
  shell_qt/src/MainWindow.cpp`
- **Impact**: Double execution latency when opening large CAD documents.

### Description

When a document is loaded:

1.  `cad_document_open()` in `Session.cpp` runs `Engine::recompute()` to
    validate the document before installing it.
2.  `Controller::loadFrom()` calls `refresh()`, which runs `Engine::recompute()`
    a **second time**.

For heavy models with hundreds of features, opening the file takes twice as
long as necessary.

- - -
# Verification & Test Suite Status

All project test tiers were verified cleanly:

`bash`

`\# 1. Structural Layering Check`

`python3 tools/check_layering.py .`

`\-> Layering OK`

`\# 2. C++ Unit & Kernel Acceptance Tests (Catch2)`

`ctest --test-dir build --output-on-failure`

`\-> 22/22 Tests Passed (100%)`

`\# 3. Rust Acceptance & Property Test Suite (C ABI)`

`CAD_BUILD_DIR=/Users/venugopal/Developer/CAD/build cargo test --workspace`

`\-> 64/64 Acceptance & Property Tests Passed (100%)`

