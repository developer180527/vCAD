//! Safe, ergonomic Rust API over the CAD core's C ABI.
//!
//! Two jobs:
//!   1. Make the test suite pleasant to write, so tests actually get written.
//!   2. Be the reference for how a language binding should consume the ABI. If something is
//!      awkward here, the ABI is wrong and should be fixed rather than papered over.
//!
//! Safety contract with the C side: handles are validated by the core, every fallible call
//! returns a status, and returned strings are copied immediately (they are only valid until
//! the next call on the same session — see the header).

use std::ffi::{CStr, CString};
use std::fmt;

pub use cad_sys as sys;

// --- errors ------------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    pub code: ErrorCode,
    pub message: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorCode {
    InvalidInput,
    NotDone,
    InvalidResult,
    BooleanFailed,
    Unsupported,
    NamingLost,
    KernelException,
    Cancelled,
    BadHandle,
    NoPermission,
    Internal,
    Unknown(i32),
}

impl ErrorCode {
    fn from_raw(v: sys::CadStatus) -> Self {
        match v {
            sys::CAD_ERR_INVALID_INPUT => Self::InvalidInput,
            sys::CAD_ERR_NOT_DONE => Self::NotDone,
            sys::CAD_ERR_INVALID_RESULT => Self::InvalidResult,
            sys::CAD_ERR_BOOLEAN_FAILED => Self::BooleanFailed,
            sys::CAD_ERR_UNSUPPORTED => Self::Unsupported,
            sys::CAD_ERR_NAMING_LOST => Self::NamingLost,
            sys::CAD_ERR_KERNEL_EXC => Self::KernelException,
            sys::CAD_ERR_CANCELLED => Self::Cancelled,
            sys::CAD_ERR_BAD_HANDLE => Self::BadHandle,
            sys::CAD_ERR_NO_PERMISSION => Self::NoPermission,
            sys::CAD_ERR_INTERNAL => Self::Internal,
            other => Self::Unknown(other),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.code, self.message)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

// --- enums mirroring the core ------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    Clean,
    Dirty,
    Failed,
    Blocked,
}

/// Axis-named on purpose: OCCT's `FrontFace()` is the x-max face, not y-min.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BoxFace {
    ZMin,
    ZMax,
    XMax,
    XMin,
    YMin,
    YMax,
}

impl BoxFace {
    fn raw(self) -> i32 {
        match self {
            Self::ZMin => sys::BOX_Z_MIN,
            Self::ZMax => sys::BOX_Z_MAX,
            Self::XMax => sys::BOX_X_MAX,
            Self::XMin => sys::BOX_X_MIN,
            Self::YMin => sys::BOX_Y_MIN,
            Self::YMax => sys::BOX_Y_MAX,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnitSystem {
    Millimetre,
    Centimetre,
    Metre,
    Inch,
    Foot,
}

impl UnitSystem {
    fn raw(self) -> i32 {
        match self {
            Self::Millimetre => sys::UNIT_MM,
            Self::Centimetre => sys::UNIT_CM,
            Self::Metre => sys::UNIT_M,
            Self::Inch => sys::UNIT_IN,
            Self::Foot => sys::UNIT_FT,
        }
    }
}

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct RecomputeReport {
    pub computed: u64,
    pub cached: u64,
    pub skipped: u64,
    pub failed: u64,
    pub blocked: u64,
}

impl RecomputeReport {
    pub fn all_succeeded(&self) -> bool {
        self.failed == 0 && self.blocked == 0
    }
}

/// What tessellating one object produced. Counts, not data: the mesh itself is consumed
/// in-process by the renderer.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct MeshInfo {
    pub triangles: u64,
    pub vertices: u64,
    pub edge_polylines: u64,
    pub edge_points: u64,
    pub elements: u64,
    pub bounds_min: [f32; 3],
    pub bounds_max: [f32; 3],
}

/// What the scene layer and the null backend recorded. Counts, because counts are what say
/// whether dedupe, diffing and instancing are working.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct SceneStats {
    pub rebuilds: u64,
    pub uploads: u64,
    pub gpu_uploads: u64,
    pub gpu_deduped: u64,
    pub unique_meshes: u64,
    pub instances: u64,
    pub element_slots: u64,
    pub draw_calls: u64,
    pub frame_instances: u64,
    pub frame_triangles: u64,
    pub frames: u64,
    pub highlighted: u64,
    pub orthographic: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Drag {
    None,
    Orbit,
    Pan,
    Zoom,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NavPreset {
    Cad,
    Fusion,
    Blender,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Highlight {
    None,
    Hovered,
    Selected,
    Error,
}

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct CacheStats {
    pub hits: u64,
    pub misses: u64,
}

// --- object handle ------------------------------------------------------------------------

/// An object id within a session. Copyable and inert — it does not own anything, which
/// matches the C ABI: the session owns every object.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Object(pub sys::CadObject);

// --- session -------------------------------------------------------------------------------

/// Owns a document, a feature registry and a recompute cache. Released on drop.
pub struct Session {
    handle: sys::CadSession,
}

/// What an import found, without committing it to a document.
#[derive(Debug, Clone, Default)]
pub struct ImportReport {
    pub solids: u64,
    pub faces: u64,
    pub units_were_assumed: bool,
    pub unsupported: Vec<String>,
    pub warnings: Vec<String>,
    summary: String,
}

impl ImportReport {
    pub fn summary(&self) -> &str {
        &self.summary
    }
    pub fn lossless(&self) -> bool {
        self.unsupported.is_empty()
    }
}

impl Session {
    pub fn new() -> Result<Self> {
        let handle = unsafe { sys::cad_session_create() };
        if handle == 0 {
            return Err(Error {
                code: ErrorCode::Internal,
                message: "could not create a session".into(),
            });
        }
        Ok(Self { handle })
    }

    /// A session with the on-disk DDC tier enabled. Results computed here are available to
    /// any later session pointed at the same directory — which is what makes a team or CI
    /// cache work.
    pub fn with_cache(dir: &str) -> Result<Self> {
        let c = cstr(dir)?;
        let handle = unsafe { sys::cad_session_create_cached(c.as_ptr()) };
        if handle == 0 {
            return Err(Error {
                code: ErrorCode::Internal,
                message: "could not create a cached session".into(),
            });
        }
        Ok(Self { handle })
    }

    pub fn export_file(&self, o: Object, path: &str) -> Result<()> {
        let c = cstr(path)?;
        let st = unsafe { sys::cad_object_export(self.handle, o.0, c.as_ptr()) };
        self.check(st)
    }

    /// Reads a file and reports on it without adding anything to the document.
    pub fn probe_import(&self, path: &str, assumed: UnitSystem) -> Result<ImportReport> {
        let c = cstr(path)?;
        let mut info = sys::CadImportInfo::default();
        let st =
            unsafe { sys::cad_import_probe(self.handle, c.as_ptr(), assumed.raw(), &mut info) };
        self.check(st)?;

        // The summary is session-scoped and only valid until the next call, so copy it now.
        let summary = self.take_str(unsafe { sys::cad_import_summary(self.handle) });
        Ok(ImportReport {
            solids: info.solids,
            faces: info.faces,
            units_were_assumed: info.units_were_assumed != 0,
            // The ABI reports counts rather than the strings themselves; the strings are in
            // the summary. Placeholders keep `lossless()` meaningful without a second call.
            unsupported: vec![String::new(); info.unsupported_count.max(0) as usize],
            warnings: vec![String::new(); info.warning_count.max(0) as usize],
            summary,
        })
    }

    /// Tessellates through the mesh cache. Tolerances are part of the cache key, so changing
    /// either is a miss by construction — a mesh at the wrong fidelity cannot be served.
    pub fn tessellate(&mut self, o: Object, deflection: f64, angular: f64) -> Result<MeshInfo> {
        let mut info = sys::CadMeshInfo::default();
        let st =
            unsafe { sys::cad_object_tessellate(self.handle, o.0, deflection, angular, &mut info) };
        self.check(st)?;
        Ok(MeshInfo {
            triangles: info.triangles,
            vertices: info.vertices,
            edge_polylines: info.edge_polylines,
            edge_points: info.edge_points,
            elements: info.elements,
            bounds_min: info.bounds_min,
            bounds_max: info.bounds_max,
        })
    }

    /// Name of a mesh element slot. Empty if out of range — this is what a GPU pick would
    /// resolve through, so an empty result for a valid slot means picking is broken.
    pub fn mesh_element_name(&self, o: Object, slot: u32) -> String {
        self.take_str(unsafe { sys::cad_mesh_element_name(self.handle, o.0, slot) })
    }

    pub fn mesh_cache_stats(&self) -> Result<CacheStats> {
        let (mut hits, mut misses) = (0u64, 0u64);
        let st = unsafe { sys::cad_mesh_cache_stats(self.handle, &mut hits, &mut misses) };
        self.check(st)?;
        Ok(CacheStats { hits, misses })
    }

    pub fn reset_mesh_cache_stats(&mut self) -> Result<()> {
        let st = unsafe { sys::cad_mesh_cache_reset_stats(self.handle) };
        self.check(st)
    }

    // ── scene ─────────────────────────────────────────────────────────────────────────

    /// Adds a placement. `transform` is a column-major 4x3 affine; None means identity.
    /// Placing one object many times is the normal case in an assembly.
    pub fn add_placement(&mut self, o: Object, transform: Option<&[f32; 12]>) -> Result<()> {
        let ptr = transform.map_or(std::ptr::null(), |t| t.as_ptr());
        let st = unsafe { sys::cad_scene_add_placement(self.handle, o.0, ptr) };
        self.check(st)
    }

    pub fn clear_placements(&mut self) -> Result<()> {
        let st = unsafe { sys::cad_scene_clear_placements(self.handle) };
        self.check(st)
    }

    pub fn scene_update(&mut self, deflection: f64, angular: f64) -> Result<()> {
        let st = unsafe { sys::cad_scene_update(self.handle, deflection, angular) };
        self.check(st)
    }

    pub fn scene_submit(&mut self) -> Result<()> {
        let st = unsafe { sys::cad_scene_submit(self.handle) };
        self.check(st)
    }

    pub fn scene_stats(&self) -> Result<SceneStats> {
        let mut raw = sys::CadSceneStats::default();
        let st = unsafe { sys::cad_scene_stats(self.handle, &mut raw) };
        self.check(st)?;
        Ok(SceneStats {
            rebuilds: raw.rebuilds,
            uploads: raw.uploads,
            gpu_uploads: raw.gpu_uploads,
            gpu_deduped: raw.gpu_deduped,
            unique_meshes: raw.unique_meshes,
            instances: raw.instances,
            element_slots: raw.element_slots,
            draw_calls: raw.draw_calls,
            frame_instances: raw.frame_instances,
            frame_triangles: raw.frame_triangles,
            frames: raw.frames,
            highlighted: raw.highlighted,
            orthographic: raw.orthographic != 0,
        })
    }

    pub fn scene_reset_stats(&mut self) -> Result<()> {
        let st = unsafe { sys::cad_scene_reset_stats(self.handle) };
        self.check(st)
    }

    pub fn orbit(&mut self, dx: f32, dy: f32) -> Result<()> {
        self.check(unsafe { sys::cad_camera_orbit(self.handle, dx, dy) })
    }
    pub fn pan(&mut self, dx: f32, dy: f32) -> Result<()> {
        self.check(unsafe { sys::cad_camera_pan(self.handle, dx, dy) })
    }
    pub fn zoom(&mut self, ticks: f32) -> Result<()> {
        self.check(unsafe { sys::cad_camera_zoom(self.handle, ticks) })
    }
    pub fn fit(&mut self) -> Result<()> {
        self.check(unsafe { sys::cad_camera_fit(self.handle) })
    }
    pub fn set_orthographic(&mut self, ortho: bool) -> Result<()> {
        self.check(unsafe { sys::cad_camera_set_orthographic(self.handle, i32::from(ortho)) })
    }
    pub fn set_viewport(&mut self, w: u32, h: u32) -> Result<()> {
        self.check(unsafe { sys::cad_camera_set_viewport(self.handle, w, h) })
    }
    pub fn camera_distance(&self) -> Result<f32> {
        let mut out = 0f32;
        self.check(unsafe { sys::cad_camera_distance(self.handle, &mut out) })?;
        Ok(out)
    }
    pub fn set_nav_preset(&mut self, p: NavPreset) -> Result<()> {
        let raw = match p {
            NavPreset::Cad => sys::NAV_CAD,
            NavPreset::Fusion => sys::NAV_FUSION,
            NavPreset::Blender => sys::NAV_BLENDER,
        };
        self.check(unsafe { sys::cad_camera_set_preset(self.handle, raw) })
    }

    /// What a gesture means under the active preset. Lives in the core so every shell behaves
    /// identically instead of each reimplementing the table.
    pub fn drag_for(&self, button: i32, shift: bool, ctrl: bool) -> Result<Drag> {
        let mut out = 0i32;
        self.check(unsafe {
            sys::cad_camera_drag_for(
                self.handle,
                button,
                i32::from(shift),
                i32::from(ctrl),
                &mut out,
            )
        })?;
        Ok(match out {
            sys::DRAG_ORBIT => Drag::Orbit,
            sys::DRAG_PAN => Drag::Pan,
            sys::DRAG_ZOOM => Drag::Zoom,
            _ => Drag::None,
        })
    }

    pub fn set_highlight(&mut self, element: &str, h: Highlight) -> Result<()> {
        let c = cstr(element)?;
        let raw = match h {
            Highlight::None => sys::HL_NONE,
            Highlight::Hovered => sys::HL_HOVERED,
            Highlight::Selected => sys::HL_SELECTED,
            Highlight::Error => sys::HL_ERROR,
        };
        self.check(unsafe { sys::cad_scene_set_highlight(self.handle, c.as_ptr(), raw) })
    }

    pub fn clear_highlights(&mut self) -> Result<()> {
        self.check(unsafe { sys::cad_scene_clear_highlights(self.handle) })
    }

    /// Scripts the null picker's next answer. What is under test is our (instance, slot) ->
    /// ElementName mapping, not whether a GPU writes the right ids — that needs a GPU.
    pub fn set_next_hit(&mut self, instance: u32, element: u32, valid: bool) -> Result<()> {
        self.check(unsafe {
            sys::cad_scene_set_next_hit(self.handle, instance, element, i32::from(valid))
        })
    }

    /// Element name under a point. Empty when nothing is hit.
    pub fn pick(&self, x: u32, y: u32) -> String {
        self.take_str(unsafe { sys::cad_scene_pick(self.handle, x, y) })
    }

    /// Which document object owns the element under a point — what a shell needs to select a
    /// part in the tree from a click in the viewport.
    pub fn pick_owner(&self, x: u32, y: u32) -> Result<Object> {
        let mut out: sys::CadObject = 0;
        self.check(unsafe { sys::cad_scene_pick_owner(self.handle, x, y, &mut out) })?;
        Ok(Object(out))
    }

    pub fn readable_extensions(&self) -> Vec<String> {
        self.take_str(unsafe { sys::cad_readable_extensions(self.handle) })
            .split(',')
            .filter(|s| !s.is_empty())
            .map(str::to_owned)
            .collect()
    }

    fn last_error(&self) -> String {
        unsafe {
            let p = sys::cad_session_last_error(self.handle);
            if p.is_null() {
                String::new()
            } else {
                CStr::from_ptr(p).to_string_lossy().into_owned()
            }
        }
    }

    fn check(&self, status: sys::CadStatus) -> Result<()> {
        if status == sys::CAD_OK {
            Ok(())
        } else {
            Err(Error {
                code: ErrorCode::from_raw(status),
                message: self.last_error(),
            })
        }
    }

    /// Copies a session-owned string immediately, as the header requires.
    fn take_str(&self, p: *const std::os::raw::c_char) -> String {
        unsafe {
            if p.is_null() {
                String::new()
            } else {
                CStr::from_ptr(p).to_string_lossy().into_owned()
            }
        }
    }

    // --- editing --------------------------------------------------------------------------

    pub fn add(&mut self, ty: &str) -> Result<Object> {
        let c = CString::new(ty).map_err(|_| Error {
            code: ErrorCode::InvalidInput,
            message: "type name contains a NUL byte".into(),
        })?;
        let mut out: sys::CadObject = 0;
        let st = unsafe { sys::cad_object_add(self.handle, c.as_ptr(), &mut out) };
        self.check(st)?;
        Ok(Object(out))
    }

    pub fn remove(&mut self, o: Object) -> Result<()> {
        let st = unsafe { sys::cad_object_remove(self.handle, o.0) };
        self.check(st)
    }

    /// Sets a length property, in millimetres — the core's base unit.
    pub fn set_length(&mut self, o: Object, prop: &str, mm: f64) -> Result<()> {
        let c = cstr(prop)?;
        let st = unsafe { sys::cad_object_set_length(self.handle, o.0, c.as_ptr(), mm) };
        self.check(st)
    }

    pub fn set_real(&mut self, o: Object, prop: &str, v: f64) -> Result<()> {
        let c = cstr(prop)?;
        let st = unsafe { sys::cad_object_set_real(self.handle, o.0, c.as_ptr(), v) };
        self.check(st)
    }

    pub fn set_int(&mut self, o: Object, prop: &str, v: i64) -> Result<()> {
        let c = cstr(prop)?;
        let st = unsafe { sys::cad_object_set_int(self.handle, o.0, c.as_ptr(), v) };
        self.check(st)
    }

    pub fn set_bool(&mut self, o: Object, prop: &str, v: bool) -> Result<()> {
        let c = cstr(prop)?;
        let st = unsafe {
            sys::cad_object_set_bool(self.handle, o.0, c.as_ptr(), if v { 1 } else { 0 })
        };
        self.check(st)
    }

    pub fn set_text(&mut self, o: Object, prop: &str, v: &str) -> Result<()> {
        let c = cstr(prop)?;
        let val = cstr(v)?;
        let st = unsafe { sys::cad_object_set_text(self.handle, o.0, c.as_ptr(), val.as_ptr()) };
        self.check(st)
    }

    pub fn set_input(&mut self, o: Object, prop: &str, target: Object) -> Result<()> {
        let c = cstr(prop)?;
        let st = unsafe { sys::cad_object_set_object(self.handle, o.0, c.as_ptr(), target.0) };
        self.check(st)
    }

    /// Sets a geometric reference from its stable text form. This is how a caller holds on
    /// to "that edge" across rebuilds — the whole point of the naming layer.
    pub fn set_element(&mut self, o: Object, prop: &str, element: &str) -> Result<()> {
        let c = cstr(prop)?;
        let e = cstr(element)?;
        let st = unsafe { sys::cad_object_set_element(self.handle, o.0, c.as_ptr(), e.as_ptr()) };
        self.check(st)
    }

    /// Marks a property as excluded from the recompute cache key.
    pub fn set_cosmetic(&mut self, o: Object, prop: &str, cosmetic: bool) -> Result<()> {
        let c = cstr(prop)?;
        let st = unsafe {
            sys::cad_object_set_cosmetic(self.handle, o.0, c.as_ptr(), i32::from(cosmetic))
        };
        self.check(st)
    }

    // --- queries --------------------------------------------------------------------------

    pub fn state(&self, o: Object) -> Result<State> {
        let mut raw = 0i32;
        let st = unsafe { sys::cad_object_state(self.handle, o.0, &mut raw) };
        self.check(st)?;
        Ok(match raw {
            sys::CAD_STATE_CLEAN => State::Clean,
            sys::CAD_STATE_DIRTY => State::Dirty,
            sys::CAD_STATE_FAILED => State::Failed,
            _ => State::Blocked,
        })
    }

    pub fn object_error(&self, o: Object) -> String {
        self.take_str(unsafe { sys::cad_object_error(self.handle, o.0) })
    }

    pub fn face_count(&self, o: Object) -> Result<u64> {
        let mut out = 0u64;
        let st = unsafe { sys::cad_object_face_count(self.handle, o.0, &mut out) };
        self.check(st)?;
        Ok(out)
    }

    pub fn edge_count(&self, o: Object) -> Result<u64> {
        let mut out = 0u64;
        let st = unsafe { sys::cad_object_edge_count(self.handle, o.0, &mut out) };
        self.check(st)?;
        Ok(out)
    }

    pub fn cache_key(&self, o: Object) -> Result<u64> {
        let mut out = 0u64;
        let st = unsafe { sys::cad_object_cache_key(self.handle, o.0, &mut out) };
        self.check(st)?;
        Ok(out)
    }

    pub fn is_valid_shape(&self, o: Object) -> Result<bool> {
        let mut out = 0i32;
        let st = unsafe { sys::cad_object_is_valid_shape(self.handle, o.0, &mut out) };
        self.check(st)?;
        Ok(out != 0)
    }

    pub fn volume(&self, o: Object) -> Result<f64> {
        let mut out = 0f64;
        let st = unsafe { sys::cad_object_volume(self.handle, o.0, &mut out) };
        self.check(st)?;
        Ok(out)
    }

    pub fn content_hash(&self, o: Object) -> String {
        self.take_str(unsafe { sys::cad_object_content_hash(self.handle, o.0) })
    }

    /// Stable name of the edge shared by two of a box's faces. Empty if no such edge exists
    /// — which is exactly what happens after an operation removes it, and is a result the
    /// caller must handle rather than a failure.
    pub fn box_edge_between(&self, b: Object, a: BoxFace, c: BoxFace) -> String {
        self.take_str(unsafe { sys::cad_box_edge_between(self.handle, b.0, a.raw(), c.raw()) })
    }

    pub fn box_face_name(&self, b: Object, face: BoxFace) -> String {
        self.take_str(unsafe { sys::cad_box_face_name(self.handle, b.0, face.raw()) })
    }

    pub fn object_count(&self) -> Result<u64> {
        let mut out = 0u64;
        let st = unsafe { sys::cad_object_count(self.handle, &mut out) };
        self.check(st)?;
        Ok(out)
    }

    pub fn document_digest(&self) -> Result<u64> {
        let mut out = 0u64;
        let st = unsafe { sys::cad_document_digest(self.handle, &mut out) };
        self.check(st)?;
        Ok(out)
    }

    // --- recompute ------------------------------------------------------------------------

    pub fn recompute(&mut self) -> Result<RecomputeReport> {
        let mut raw = sys::CadRecomputeReport::default();
        let st = unsafe { sys::cad_recompute(self.handle, &mut raw) };
        self.check(st)?;
        Ok(RecomputeReport {
            computed: raw.computed,
            cached: raw.cached,
            skipped: raw.skipped,
            failed: raw.failed,
            blocked: raw.blocked,
        })
    }

    pub fn cache_stats(&self) -> Result<CacheStats> {
        let (mut hits, mut misses) = (0u64, 0u64);
        let st = unsafe { sys::cad_cache_stats(self.handle, &mut hits, &mut misses) };
        self.check(st)?;
        Ok(CacheStats { hits, misses })
    }

    pub fn reset_cache_stats(&mut self) -> Result<()> {
        let st = unsafe { sys::cad_cache_reset_stats(self.handle) };
        self.check(st)
    }

    // --- history --------------------------------------------------------------------------

    pub fn undo(&mut self) -> Result<bool> {
        let mut out = 0i32;
        let st = unsafe { sys::cad_undo(self.handle, &mut out) };
        self.check(st)?;
        Ok(out != 0)
    }

    pub fn redo(&mut self) -> Result<bool> {
        let mut out = 0i32;
        let st = unsafe { sys::cad_redo(self.handle, &mut out) };
        self.check(st)?;
        Ok(out != 0)
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        unsafe { sys::cad_session_release(self.handle) };
    }
}

// A Session is a handle into a mutex-guarded registry on the C side, so it is safe to move
// between threads. It is deliberately NOT Sync: the returned-string contract ("valid until
// the next call on this session") cannot survive concurrent readers.
unsafe impl Send for Session {}

// --- free functions -------------------------------------------------------------------------

/// Parses a length through the core's own parser, so bindings never reimplement it.
pub fn parse_length(text: &str, assumed: UnitSystem) -> Result<f64> {
    let c = cstr(text)?;
    let mut out = 0f64;
    let st = unsafe { sys::cad_parse_length(c.as_ptr(), assumed.raw(), &mut out) };
    if st == sys::CAD_OK {
        Ok(out)
    } else {
        Err(Error {
            code: ErrorCode::from_raw(st),
            // No session, so no session-scoped message. The code is the whole signal here.
            message: format!("could not parse {text:?} as a length"),
        })
    }
}

fn cstr(s: &str) -> Result<CString> {
    CString::new(s).map_err(|_| Error {
        code: ErrorCode::InvalidInput,
        message: format!("{s:?} contains a NUL byte"),
    })
}

// --- convenience builders used throughout the suite -------------------------------------------

impl Session {
    /// Adds a box and sets its three dimensions in one call.
    pub fn add_box(&mut self, dx: f64, dy: f64, dz: f64) -> Result<Object> {
        let o = self.add("Box")?;
        self.set_length(o, "dx", dx)?;
        self.set_length(o, "dy", dy)?;
        self.set_length(o, "dz", dz)?;
        Ok(o)
    }

    pub fn add_fillet(&mut self, base: Object, edge: &str, radius: f64) -> Result<Object> {
        let o = self.add("Fillet")?;
        self.set_input(o, "base", base)?;
        self.set_element(o, "edges", edge)?;
        self.set_length(o, "radius", radius)?;
        Ok(o)
    }

    pub fn add_chamfer(&mut self, base: Object, edge: &str, distance: f64) -> Result<Object> {
        let o = self.add("Chamfer")?;
        self.set_input(o, "base", base)?;
        self.set_element(o, "edges", edge)?;
        self.set_length(o, "distance", distance)?;
        Ok(o)
    }

    /// Boolean cut. Property names are ordered so `base` sorts before `tool`, which is what
    /// determines the order the engine passes them to the feature.
    pub fn add_cut(&mut self, base: Object, tool: Object) -> Result<Object> {
        let o = self.add("Cut")?;
        self.set_input(o, "a_base", base)?;
        self.set_input(o, "b_tool", tool)?;
        Ok(o)
    }

    /// An imported file as a document feature: it has a cache key like anything else, so a
    /// re-import is cached and downstream features survive it.
    pub fn add_import(&mut self, path: &str) -> Result<Object> {
        let o = self.add("Import")?;
        self.set_text(o, "path", path)?;
        Ok(o)
    }

    pub fn add_translate(&mut self, base: Object, dx: f64, dy: f64, dz: f64) -> Result<Object> {
        let o = self.add("Translate")?;
        self.set_input(o, "base", base)?;
        self.set_length(o, "dx", dx)?;
        self.set_length(o, "dy", dy)?;
        self.set_length(o, "dz", dz)?;
        Ok(o)
    }
}
