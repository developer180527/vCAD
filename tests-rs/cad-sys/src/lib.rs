//! Raw FFI declarations for `abi/include/cad/abi/cad_plugin_abi.h`.
//!
//! Hand-written, not bindgen-generated. Against a deliberately frozen ABI that is the right
//! trade: an accidental change to the C header becomes a link error or a test failure here,
//! rather than being silently absorbed by a regeneration step. It also keeps libclang out
//! of the CI image.
//!
//! The rule when editing: this file and the C header change together, in the same commit,
//! and `CAD_ABI_VERSION_MINOR` goes up. `abi_version_matches()` below is the tripwire.

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int};

pub const CAD_ABI_VERSION_MAJOR: u32 = 1;
pub const CAD_ABI_VERSION_MINOR: u32 = 14;

pub type CadStatus = i32;

pub const CAD_OK: CadStatus = 0;
pub const CAD_ERR_INVALID_INPUT: CadStatus = 1;
pub const CAD_ERR_NOT_DONE: CadStatus = 2;
pub const CAD_ERR_INVALID_RESULT: CadStatus = 3;
pub const CAD_ERR_BOOLEAN_FAILED: CadStatus = 4;
pub const CAD_ERR_UNSUPPORTED: CadStatus = 5;
pub const CAD_ERR_NAMING_LOST: CadStatus = 6;
pub const CAD_ERR_KERNEL_EXC: CadStatus = 7;
pub const CAD_ERR_CANCELLED: CadStatus = 8;
pub const CAD_ERR_BAD_HANDLE: CadStatus = 9;
pub const CAD_ERR_NO_PERMISSION: CadStatus = 10;
pub const CAD_ERR_INTERNAL: CadStatus = 99;

pub type CadSession = u64;
pub type CadObject = u64;
pub type CadSketch = u64;

/// Mirrors the header's `CadStr`: a borrowed (pointer, length) pair owned by the session and valid
/// only until the next call on it. Returned by value, so it must be declared here rather than
/// approximated as `*const c_char` — a struct return and a pointer return are different ABIs.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CadStr {
    pub data: *const c_char,
    pub len: usize,
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy)]
pub struct CadSolveReport {
    pub solved: i32,
    pub dofs: i32,
    pub conflicting: u64,
    pub redundant: u64,
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy)]
pub struct CadSketchGeo {
    pub kind: i32,
    pub construction: i32,
    pub p: [f64; 5],
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy)]
pub struct CadInferReport {
    pub coincident: u64,
    pub horizontal: u64,
    pub vertical: u64,
    pub parallel: u64,
    pub perpendicular: u64,
    pub dofs_before: i32,
    pub dofs_after: i32,
    pub conflicting: u64,
}

/// Compares the constants above against the version the linked library reports.
///
/// The module comment has claimed since M1 that this function was "the tripwire". It did not exist,
/// and the constant sat at 4 while the C header reached 6 -- exactly the silent drift it was
/// supposed to prevent. Now it is real and `abi_version_is_current` asserts it.
pub fn abi_version_matches() -> Result<(), (u32, u32)> {
    let mut major = 0u32;
    let mut minor = 0u32;
    unsafe { cad_abi_version(&mut major, &mut minor) };
    if major == CAD_ABI_VERSION_MAJOR && minor == CAD_ABI_VERSION_MINOR {
        Ok(())
    } else {
        Err((major, minor))
    }
}

/// Mirrors `cad::document::ObjectState`. The numeric values are part of the ABI.
pub const CAD_STATE_CLEAN: i32 = 0;
pub const CAD_STATE_DIRTY: i32 = 1;
pub const CAD_STATE_FAILED: i32 = 2;
pub const CAD_STATE_BLOCKED: i32 = 3;

/// Mirrors `cad::kernel::BoxFace`. Axis-named because OCCT's `FrontFace()` is the x-max
/// face, not y-min — see core/kernel/include/cad/kernel/Primitives.h.
pub const BOX_Z_MIN: i32 = 0;
pub const BOX_Z_MAX: i32 = 1;
pub const BOX_X_MAX: i32 = 2;
pub const BOX_X_MIN: i32 = 3;
pub const BOX_Y_MIN: i32 = 4;
pub const BOX_Y_MAX: i32 = 5;

/// Mirrors `cad::units::UnitSystem`.
pub const UNIT_MM: i32 = 0;
pub const UNIT_CM: i32 = 1;
pub const UNIT_M: i32 = 2;
pub const UNIT_IN: i32 = 3;
pub const UNIT_FT: i32 = 4;

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct CadImportInfo {
    pub solids: u64,
    pub faces: u64,
    pub units_were_assumed: i32,
    pub unsupported_count: i32,
    pub warning_count: i32,
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct CadMeshInfo {
    pub triangles: u64,
    pub vertices: u64,
    pub edge_polylines: u64,
    pub edge_points: u64,
    pub elements: u64,
    pub bounds_min: [f32; 3],
    pub bounds_max: [f32; 3],
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct CadSceneStats {
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
    pub orthographic: i32,
}

/// Mirrors `cad::render::Drag`.
pub const DRAG_NONE: i32 = 0;
pub const DRAG_ORBIT: i32 = 1;
pub const DRAG_PAN: i32 = 2;
pub const DRAG_ZOOM: i32 = 3;

/// Mirrors `cad::render::NavigationPreset`.
pub const NAV_CAD: i32 = 0;
pub const NAV_FUSION: i32 = 1;
pub const NAV_BLENDER: i32 = 2;

/// Mirrors `cad::render::Highlight`.
pub const HL_NONE: i32 = 0;
pub const HL_HOVERED: i32 = 1;
pub const HL_SELECTED: i32 = 2;
pub const HL_ERROR: i32 = 3;

#[repr(C)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct CadRecomputeReport {
    pub computed: u64,
    pub cached: u64,
    pub skipped: u64,
    pub failed: u64,
    pub blocked: u64,
}

extern "C" {
    pub fn cad_abi_version(major: *mut u32, minor: *mut u32);
    pub fn cad_session_create() -> CadSession;
    pub fn cad_session_create_cached(dir: *const c_char) -> CadSession;
    pub fn cad_session_release(s: CadSession);
    pub fn cad_session_last_error(s: CadSession) -> *const c_char;

    pub fn cad_object_add(s: CadSession, ty: *const c_char, out: *mut CadObject) -> CadStatus;
    pub fn cad_object_remove(s: CadSession, id: CadObject) -> CadStatus;

    pub fn cad_object_set_length(
        s: CadSession,
        id: CadObject,
        prop: *const c_char,
        mm: f64,
    ) -> CadStatus;
    pub fn cad_object_set_real(
        s: CadSession,
        id: CadObject,
        prop: *const c_char,
        v: f64,
    ) -> CadStatus;
    pub fn cad_object_set_int(
        s: CadSession,
        id: CadObject,
        prop: *const c_char,
        v: i64,
    ) -> CadStatus;
    pub fn cad_object_set_bool(
        s: CadSession,
        id: CadObject,
        prop: *const c_char,
        v: i32,
    ) -> CadStatus;
    pub fn cad_object_set_text(
        s: CadSession,
        id: CadObject,
        prop: *const c_char,
        v: *const c_char,
    ) -> CadStatus;
    pub fn cad_object_set_object(
        s: CadSession,
        id: CadObject,
        prop: *const c_char,
        target: CadObject,
    ) -> CadStatus;
    pub fn cad_object_set_element(
        s: CadSession,
        id: CadObject,
        prop: *const c_char,
        element: *const c_char,
    ) -> CadStatus;
    pub fn cad_object_set_cosmetic(
        s: CadSession,
        id: CadObject,
        prop: *const c_char,
        cosmetic: i32,
    ) -> CadStatus;

    pub fn cad_object_state(s: CadSession, id: CadObject, out: *mut i32) -> CadStatus;
    pub fn cad_object_error(s: CadSession, id: CadObject) -> *const c_char;
    pub fn cad_object_face_count(s: CadSession, id: CadObject, out: *mut u64) -> CadStatus;
    pub fn cad_object_edge_count(s: CadSession, id: CadObject, out: *mut u64) -> CadStatus;
    pub fn cad_object_cache_key(s: CadSession, id: CadObject, out: *mut u64) -> CadStatus;
    pub fn cad_object_is_valid_shape(s: CadSession, id: CadObject, out: *mut i32) -> CadStatus;
    pub fn cad_object_volume(s: CadSession, id: CadObject, out: *mut f64) -> CadStatus;
    pub fn cad_object_content_hash(s: CadSession, id: CadObject) -> *const c_char;

    pub fn cad_box_edge_between(s: CadSession, b: CadObject, a: i32, c: i32) -> *const c_char;
    pub fn cad_box_face_name(s: CadSession, b: CadObject, face: i32) -> *const c_char;

    pub fn cad_recompute(s: CadSession, out: *mut CadRecomputeReport) -> CadStatus;
    pub fn cad_cache_stats(s: CadSession, hits: *mut u64, misses: *mut u64) -> CadStatus;
    pub fn cad_cache_reset_stats(s: CadSession) -> CadStatus;

    pub fn cad_undo(s: CadSession, out: *mut i32) -> CadStatus;
    pub fn cad_redo(s: CadSession, out: *mut i32) -> CadStatus;
    pub fn cad_document_digest(s: CadSession, out: *mut u64) -> CadStatus;
    pub fn cad_object_count(s: CadSession, out: *mut u64) -> CadStatus;

    // --- sketches ---
    pub fn cad_sketch_create(s: CadSession, plane: i32, out: *mut CadSketch) -> CadStatus;
    pub fn cad_sketch_release(s: CadSession, sk: CadSketch);
    pub fn cad_sketch_add_line(s: CadSession, sk: CadSketch, x1: f64, y1: f64, x2: f64, y2: f64,
                               construction: i32, out: *mut u32) -> CadStatus;
    pub fn cad_sketch_add_circle(s: CadSession, sk: CadSketch, cx: f64, cy: f64, radius: f64,
                                 construction: i32, out: *mut u32) -> CadStatus;
    pub fn cad_sketch_add_arc(s: CadSession, sk: CadSketch, cx: f64, cy: f64, radius: f64,
                              start: f64, end: f64, construction: i32, out: *mut u32) -> CadStatus;
    pub fn cad_sketch_constrain(s: CadSession, sk: CadSketch, kind: i32, a: u32, a_point: i32,
                                b: u32, b_point: i32, value: f64, out: *mut u64) -> CadStatus;
    pub fn cad_sketch_solve(s: CadSession, sk: CadSketch, out: *mut CadSolveReport) -> CadStatus;
    pub fn cad_sketch_geometry_count(s: CadSession, sk: CadSketch, out: *mut u64) -> CadStatus;
    pub fn cad_sketch_geometry(s: CadSession, sk: CadSketch, index: u64,
                               out: *mut CadSketchGeo) -> CadStatus;
    pub fn cad_sketch_constraint_count(s: CadSession, sk: CadSketch, out: *mut u64) -> CadStatus;
    pub fn cad_sketch_serialize(s: CadSession, sk: CadSketch) -> CadStr;
    pub fn cad_sketch_deserialize(s: CadSession, text: *const c_char,
                                  out: *mut CadSketch) -> CadStatus;
    pub fn cad_sketch_import_dxf(s: CadSession, path: *const c_char, plane: i32, scale: f64,
                                 out: *mut CadSketch) -> CadStatus;
    pub fn cad_sketch_export_dxf(s: CadSession, sk: CadSketch, path: *const c_char,
                                 scale: f64) -> CadStatus;
    pub fn cad_sketch_infer(s: CadSession, sk: CadSketch, point_tol: f64, angle_tol: f64,
                            parallel_perp: i32, out: *mut CadInferReport) -> CadStatus;

    pub fn cad_rollback_set(s: CadSession, id: CadObject) -> CadStatus;
    pub fn cad_rollback_get(s: CadSession, out: *mut CadObject) -> CadStatus;

    pub fn cad_document_save(s: CadSession, path: *const c_char) -> CadStatus;
    pub fn cad_document_open(s: CadSession, path: *const c_char) -> CadStatus;

    pub fn cad_object_export(s: CadSession, id: CadObject, path: *const c_char) -> CadStatus;
    pub fn cad_import_probe(
        s: CadSession,
        path: *const c_char,
        assumed: i32,
        out: *mut CadImportInfo,
    ) -> CadStatus;
    pub fn cad_import_summary(s: CadSession) -> *const c_char;
    pub fn cad_readable_extensions(s: CadSession) -> *const c_char;
    pub fn cad_writable_extensions(s: CadSession) -> *const c_char;

    pub fn cad_object_tessellate(
        s: CadSession,
        id: CadObject,
        deflection: f64,
        angular: f64,
        out: *mut CadMeshInfo,
    ) -> CadStatus;
    pub fn cad_mesh_element_name(s: CadSession, id: CadObject, slot: u32) -> *const c_char;
    pub fn cad_mesh_cache_stats(s: CadSession, hits: *mut u64, misses: *mut u64) -> CadStatus;
    pub fn cad_mesh_cache_reset_stats(s: CadSession) -> CadStatus;

    pub fn cad_scene_add_placement(
        s: CadSession,
        id: CadObject,
        transform12: *const f32,
    ) -> CadStatus;
    pub fn cad_scene_clear_placements(s: CadSession) -> CadStatus;
    pub fn cad_scene_update(s: CadSession, deflection: f64, angular: f64) -> CadStatus;
    pub fn cad_scene_submit(s: CadSession) -> CadStatus;
    pub fn cad_scene_stats(s: CadSession, out: *mut CadSceneStats) -> CadStatus;
    pub fn cad_scene_reset_stats(s: CadSession) -> CadStatus;

    pub fn cad_camera_orbit(s: CadSession, dx: f32, dy: f32) -> CadStatus;
    pub fn cad_camera_pan(s: CadSession, dx: f32, dy: f32) -> CadStatus;
    pub fn cad_camera_zoom(s: CadSession, ticks: f32) -> CadStatus;
    pub fn cad_camera_fit(s: CadSession) -> CadStatus;
    pub fn cad_camera_set_orthographic(s: CadSession, ortho: i32) -> CadStatus;
    pub fn cad_camera_set_viewport(s: CadSession, w: u32, h: u32) -> CadStatus;
    pub fn cad_camera_distance(s: CadSession, out: *mut f32) -> CadStatus;
    pub fn cad_camera_set_preset(s: CadSession, preset: i32) -> CadStatus;
    pub fn cad_camera_drag_for(
        s: CadSession,
        button: i32,
        shift: i32,
        ctrl: i32,
        out: *mut i32,
    ) -> CadStatus;

    pub fn cad_scene_set_highlight(
        s: CadSession,
        element: *const c_char,
        highlight: i32,
    ) -> CadStatus;
    pub fn cad_scene_clear_highlights(s: CadSession) -> CadStatus;
    pub fn cad_scene_set_next_hit(
        s: CadSession,
        instance: u32,
        element: u32,
        valid: i32,
    ) -> CadStatus;
    pub fn cad_scene_pick(s: CadSession, x: u32, y: u32) -> *const c_char;
    pub fn cad_scene_pick_owner(s: CadSession, x: u32, y: u32, out: *mut CadObject) -> CadStatus;

    pub fn cad_parse_length(text: *const c_char, system: c_int, out: *mut f64) -> CadStatus;
}
