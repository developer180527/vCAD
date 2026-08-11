//! Raw FFI declarations for `core/abi/include/cad/abi/cad_plugin_abi.h`.
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
pub const CAD_ABI_VERSION_MINOR: u32 = 2;

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
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct CadRecomputeReport {
    pub computed: u64,
    pub cached: u64,
    pub skipped: u64,
    pub failed: u64,
    pub blocked: u64,
}

extern "C" {
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

    pub fn cad_box_edge_between(
        s: CadSession,
        b: CadObject,
        a: i32,
        c: i32,
    ) -> *const c_char;
    pub fn cad_box_face_name(s: CadSession, b: CadObject, face: i32) -> *const c_char;

    pub fn cad_recompute(s: CadSession, out: *mut CadRecomputeReport) -> CadStatus;
    pub fn cad_cache_stats(s: CadSession, hits: *mut u64, misses: *mut u64) -> CadStatus;
    pub fn cad_cache_reset_stats(s: CadSession) -> CadStatus;

    pub fn cad_undo(s: CadSession, out: *mut i32) -> CadStatus;
    pub fn cad_redo(s: CadSession, out: *mut i32) -> CadStatus;
    pub fn cad_document_digest(s: CadSession, out: *mut u64) -> CadStatus;
    pub fn cad_object_count(s: CadSession, out: *mut u64) -> CadStatus;

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

    pub fn cad_parse_length(text: *const c_char, system: c_int, out: *mut f64) -> CadStatus;
}
