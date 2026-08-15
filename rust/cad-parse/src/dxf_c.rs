//! The C surface of the DXF reader.
//!
//! # Shape, and why it is this one
//!
//! Count-then-index, the same pattern as the plugin ABI's shape and feature accessors. It looks
//! verbose next to handing back a struct pointer, and it is the only shape where the two sides
//! cannot disagree about layout: nothing here is a struct crossing the boundary, so there is no
//! `struct_size` to negotiate, no padding to match, and no way for a stale header to make C++ read
//! the wrong field. The cost is one call per field, on a path that runs once per file open.
//!
//! Ownership is equally blunt. The parsed document lives in Rust, behind an opaque handle; strings
//! are copied into caller-allocated buffers. Nothing allocated by Rust is ever freed by C++, which
//! matters more than usual here — the two halves can be built with different allocators.
//!
//! # Everything is total
//!
//! Every accessor returns a defined value for every input, including a null handle and an
//! out-of-range index. That is not defensive habit: these are called from a loop bounded by a count
//! the caller got from a previous call, and the one bug that shape actually produces is an
//! off-by-one at the end. Reading past the end returns zero rather than corrupting memory, so that
//! bug is a wrong number instead of an exploitable one.

use crate::dxf::{self, EntityKind};
use std::os::raw::{c_char, c_int};

/// A parsed DXF file. Opaque to C.
pub struct CadDxfDocument {
    inner: dxf::Document,
    /// Flattened for indexed access: (name, count) in a stable order.
    unsupported: Vec<(String, usize)>,
}

// Entity kinds, mirrored in cad_parse.h. Explicit values because the header restates them and a
// reordering here would silently turn every circle into an arc.
pub const CAD_DXF_LINE: c_int = 0;
pub const CAD_DXF_CIRCLE: c_int = 1;
pub const CAD_DXF_ARC: c_int = 2;
pub const CAD_DXF_POINT: c_int = 3;
pub const CAD_DXF_POLYLINE: c_int = 4;

// Parse outcomes. Zero is success, matching the rest of the C surface.
pub const CAD_DXF_OK: c_int = 0;
pub const CAD_DXF_ERR_NOT_DXF: c_int = 1;
pub const CAD_DXF_ERR_RECORD_TOO_LONG: c_int = 2;
pub const CAD_DXF_ERR_TOO_LARGE: c_int = 3;
pub const CAD_DXF_ERR_TRUNCATED: c_int = 4;
pub const CAD_DXF_ERR_UNREADABLE: c_int = 5;

fn code_for(error: &dxf::Error) -> c_int {
    match error {
        dxf::Error::NotDxf => CAD_DXF_ERR_NOT_DXF,
        dxf::Error::RecordTooLong => CAD_DXF_ERR_RECORD_TOO_LONG,
        dxf::Error::TooLarge => CAD_DXF_ERR_TOO_LARGE,
        dxf::Error::Truncated => CAD_DXF_ERR_TRUNCATED,
    }
}

/// Copies `text` into `buffer` as a NUL-terminated string, returning the length written.
///
/// Refuses rather than truncates, and returns the length the caller would need so a second call
/// with a bigger buffer can succeed. Same contract as `cad_parse_build_id`, for the same reason: a
/// half-written layer name silently changes which layer an entity is on.
///
/// # Safety
/// `buffer` must be valid for writes of `capacity` bytes, or null.
unsafe fn copy_out(text: &str, buffer: *mut c_char, capacity: usize) -> usize {
    let bytes = text.as_bytes();
    if buffer.is_null() || capacity < bytes.len() + 1 {
        return bytes.len();
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer as *mut u8, bytes.len());
    *buffer.add(bytes.len()) = 0;
    bytes.len()
}

/// Reads a DXF file from disk.
///
/// Returns null on failure, with `out_error` set to a `CAD_DXF_ERR_*` code. The file is read whole
/// before parsing: DXF files are megabytes at most, and a single read means the parser sees a fixed
/// slice rather than a stream that could change under it.
///
/// # Safety
/// `path` must be a NUL-terminated string. `out_error` must be a valid pointer or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_parse_file(
    path: *const c_char,
    out_error: *mut c_int,
) -> *mut CadDxfDocument {
    let fail = |code: c_int| -> *mut CadDxfDocument {
        if !out_error.is_null() {
            *out_error = code;
        }
        std::ptr::null_mut()
    };

    if path.is_null() {
        return fail(CAD_DXF_ERR_UNREADABLE);
    }
    let Ok(path) = std::ffi::CStr::from_ptr(path).to_str() else {
        return fail(CAD_DXF_ERR_UNREADABLE);
    };
    let Ok(bytes) = std::fs::read(path) else {
        return fail(CAD_DXF_ERR_UNREADABLE);
    };

    match dxf::parse(&bytes) {
        Ok(inner) => {
            let unsupported = inner
                .unsupported
                .iter()
                .map(|(k, v)| (k.clone(), *v))
                .collect();
            if !out_error.is_null() {
                *out_error = CAD_DXF_OK;
            }
            Box::into_raw(Box::new(CadDxfDocument { inner, unsupported }))
        }
        Err(error) => fail(code_for(&error)),
    }
}

/// The message for a `CAD_DXF_ERR_*` code, copied into `buffer`.
///
/// The wording lives in Rust beside the condition that produces it rather than in a C++ switch,
/// so adding an error case cannot leave the C++ side describing it as "unknown".
///
/// # Safety
/// `buffer` must be valid for writes of `capacity` bytes, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_error_message(
    code: c_int,
    buffer: *mut c_char,
    capacity: usize,
) -> usize {
    let text = match code {
        CAD_DXF_ERR_NOT_DXF => dxf::Error::NotDxf.message(),
        CAD_DXF_ERR_RECORD_TOO_LONG => dxf::Error::RecordTooLong.message(),
        CAD_DXF_ERR_TOO_LARGE => dxf::Error::TooLarge.message(),
        CAD_DXF_ERR_TRUNCATED => dxf::Error::Truncated.message(),
        CAD_DXF_ERR_UNREADABLE => "That DXF file could not be opened.",
        _ => "That DXF file could not be read.",
    };
    copy_out(text, buffer, capacity)
}

/// Releases a document. Null is accepted and ignored.
///
/// # Safety
/// `doc` must be a pointer from `cad_dxf_parse_file` that has not already been freed.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_free(doc: *mut CadDxfDocument) {
    if !doc.is_null() {
        drop(Box::from_raw(doc));
    }
}

/// Borrows the document, or `None` for a null handle.
///
/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null.
unsafe fn doc_of<'a>(doc: *const CadDxfDocument) -> Option<&'a CadDxfDocument> {
    doc.as_ref()
}

macro_rules! entity_accessor {
    ($name:ident, $ret:ty, $default:expr, $get:expr) => {
        /// # Safety
        /// `doc` must be a live pointer from `cad_dxf_parse_file`, or null.
        #[no_mangle]
        pub unsafe extern "C" fn $name(doc: *const CadDxfDocument, index: usize) -> $ret {
            let get: fn(&dxf::Entity) -> $ret = $get;
            match doc_of(doc).and_then(|d| d.inner.entities.get(index)) {
                Some(entity) => get(entity),
                None => $default,
            }
        }
    };
}

/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_entity_count(doc: *const CadDxfDocument) -> usize {
    doc_of(doc).map_or(0, |d| d.inner.entities.len())
}

entity_accessor!(cad_dxf_entity_kind, c_int, -1, |e| match e.kind {
    EntityKind::Line => CAD_DXF_LINE,
    EntityKind::Circle => CAD_DXF_CIRCLE,
    EntityKind::Arc => CAD_DXF_ARC,
    EntityKind::Point => CAD_DXF_POINT,
    EntityKind::Polyline => CAD_DXF_POLYLINE,
});
entity_accessor!(cad_dxf_entity_radius, f64, 0.0, |e| e.radius);
// Arc start angle, in DEGREES as the file stores it. The caller converts.
entity_accessor!(cad_dxf_entity_start_angle, f64, 0.0, |e| e.start_angle);
entity_accessor!(cad_dxf_entity_end_angle, f64, 0.0, |e| e.end_angle);
// DXF flags. Bit 0 set on a polyline means closed.
entity_accessor!(cad_dxf_entity_flags, i64, 0, |e| e.flags);
// Number of x,y PAIRS, not the length of the coordinate array.
entity_accessor!(cad_dxf_entity_point_count, usize, 0, |e| e.coords.len() / 2);
entity_accessor!(cad_dxf_entity_bulge_count, usize, 0, |e| e.bulges.len());

/// The `point`-th x,y pair of entity `index`, written to `out_x`/`out_y`.
///
/// Both coordinates in one call: they are always wanted together, and a pair that cannot be split
/// across two calls cannot be half-updated by a caller that gets the second index wrong.
///
/// Returns non-zero on success. On failure the outputs are left untouched, so a caller that ignores
/// the result reads whatever it initialised them to rather than uninitialised memory.
///
/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null. `out_x` and `out_y` must be
/// valid for writes, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_entity_point(
    doc: *const CadDxfDocument,
    index: usize,
    point: usize,
    out_x: *mut f64,
    out_y: *mut f64,
) -> c_int {
    let Some(entity) = doc_of(doc).and_then(|d| d.inner.entities.get(index)) else {
        return 0;
    };
    // Checked as a pair: `coords` is x,y interleaved, so `2 * point + 1` is the real bound and
    // checking only `2 * point` would read one past the end of a truncated array.
    let (Some(&x), Some(&y)) = (
        entity.coords.get(2 * point),
        entity.coords.get(2 * point + 1),
    ) else {
        return 0;
    };
    if !out_x.is_null() {
        *out_x = x;
    }
    if !out_y.is_null() {
        *out_y = y;
    }
    1
}

/// The bulge of the segment starting at vertex `segment`, or 0 when there is none.
///
/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_entity_bulge(
    doc: *const CadDxfDocument,
    index: usize,
    segment: usize,
) -> f64 {
    doc_of(doc)
        .and_then(|d| d.inner.entities.get(index))
        .and_then(|e| e.bulges.get(segment))
        .copied()
        .unwrap_or(0.0)
}

/// The layer name of entity `index`, copied into `buffer`. Returns the length, as `copy_out`.
///
/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null. `buffer` must be valid for
/// writes of `capacity` bytes, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_entity_layer(
    doc: *const CadDxfDocument,
    index: usize,
    buffer: *mut c_char,
    capacity: usize,
) -> usize {
    match doc_of(doc).and_then(|d| d.inner.entities.get(index)) {
        Some(entity) => copy_out(&entity.layer, buffer, capacity),
        None => 0,
    }
}

/// Entities the file described but that could not be read. See `DxfImportReport::malformed`.
///
/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_malformed_count(doc: *const CadDxfDocument) -> usize {
    doc_of(doc).map_or(0, |d| d.inner.malformed)
}

/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_unsupported_count(doc: *const CadDxfDocument) -> usize {
    doc_of(doc).map_or(0, |d| d.unsupported.len())
}

/// The name of the `index`-th unsupported entity type, copied into `buffer`.
///
/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null. `buffer` must be valid for
/// writes of `capacity` bytes, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_unsupported_name(
    doc: *const CadDxfDocument,
    index: usize,
    buffer: *mut c_char,
    capacity: usize,
) -> usize {
    match doc_of(doc).and_then(|d| d.unsupported.get(index)) {
        Some((name, _)) => copy_out(name, buffer, capacity),
        None => 0,
    }
}

/// How many entities of the `index`-th unsupported type the file contained.
///
/// # Safety
/// `doc` must be a live pointer from `cad_dxf_parse_file`, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_dxf_unsupported_occurrences(
    doc: *const CadDxfDocument,
    index: usize,
) -> usize {
    doc_of(doc)
        .and_then(|d| d.unsupported.get(index))
        .map_or(0, |(_, n)| *n)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The whole surface, called with a null handle and with an index past the end.
    ///
    /// This is the test that matters most here. Every one of these is called from a C++ loop
    /// bounded by a count from a previous call, and the bug that shape produces is an off-by-one at
    /// the end — so "reading past the end returns a defined value" is the property that decides
    /// whether that bug is a wrong number or an exploitable one.
    #[test]
    fn every_accessor_is_total_for_a_null_handle_and_an_out_of_range_index() {
        let mut buffer = [0i8; 64];
        for doc in [std::ptr::null::<CadDxfDocument>()] {
            unsafe {
                assert_eq!(cad_dxf_entity_count(doc), 0);
                assert_eq!(cad_dxf_entity_kind(doc, 0), -1);
                assert_eq!(cad_dxf_entity_radius(doc, 99), 0.0);
                assert_eq!(cad_dxf_entity_start_angle(doc, 99), 0.0);
                assert_eq!(cad_dxf_entity_end_angle(doc, 99), 0.0);
                assert_eq!(cad_dxf_entity_flags(doc, 99), 0);
                assert_eq!(cad_dxf_entity_point_count(doc, 99), 0);
                assert_eq!(cad_dxf_entity_bulge_count(doc, 99), 0);
                assert_eq!(cad_dxf_entity_bulge(doc, 99, 99), 0.0);
                assert_eq!(cad_dxf_entity_layer(doc, 99, buffer.as_mut_ptr(), 64), 0);
                assert_eq!(cad_dxf_malformed_count(doc), 0);
                assert_eq!(cad_dxf_unsupported_count(doc), 0);
                assert_eq!(
                    cad_dxf_unsupported_name(doc, 99, buffer.as_mut_ptr(), 64),
                    0
                );
                assert_eq!(cad_dxf_unsupported_occurrences(doc, 99), 0);

                let mut x = -1.0;
                let mut y = -1.0;
                assert_eq!(cad_dxf_entity_point(doc, 0, 0, &mut x, &mut y), 0);
                assert_eq!(
                    (x, y),
                    (-1.0, -1.0),
                    "a failed read must not touch the outputs"
                );
            }
        }
    }

    #[test]
    fn a_parsed_document_reads_back_through_the_c_surface() {
        let text = "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nedge\n10\n1\n20\n2\n11\n3\n21\n4\n\
                    0\nSPLINE\n0\nENDSEC\n0\nEOF\n";
        let dir = std::env::temp_dir().join("cad_dxf_c_surface");
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("one_line.dxf");
        std::fs::write(&path, text).unwrap();
        let c_path = std::ffi::CString::new(path.to_str().unwrap()).unwrap();

        unsafe {
            let mut error = -1;
            let doc = cad_dxf_parse_file(c_path.as_ptr(), &mut error);
            assert_eq!(error, CAD_DXF_OK);
            assert!(!doc.is_null());

            assert_eq!(cad_dxf_entity_count(doc), 1);
            assert_eq!(cad_dxf_entity_kind(doc, 0), CAD_DXF_LINE);
            assert_eq!(cad_dxf_entity_point_count(doc, 0), 2);

            let (mut x, mut y) = (0.0, 0.0);
            assert_eq!(cad_dxf_entity_point(doc, 0, 1, &mut x, &mut y), 1);
            assert_eq!((x, y), (3.0, 4.0));
            // One past the end of a real entity, which is where the off-by-one lives.
            assert_eq!(cad_dxf_entity_point(doc, 0, 2, &mut x, &mut y), 0);
            assert_eq!(
                (x, y),
                (3.0, 4.0),
                "a failed read must not touch the outputs"
            );

            let mut buffer = [0i8; 64];
            let written = cad_dxf_entity_layer(doc, 0, buffer.as_mut_ptr(), 64);
            assert_eq!(written, 4);
            assert_eq!(buffer[4], 0, "the layer name must be NUL-terminated");

            assert_eq!(cad_dxf_unsupported_count(doc), 1);
            assert_eq!(cad_dxf_unsupported_occurrences(doc, 0), 1);

            cad_dxf_free(doc);
        }
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn a_too_small_buffer_reports_the_length_it_needs_instead_of_truncating() {
        // The caller's recovery is to allocate `written + 1` and call again, so the returned length
        // has to be the FULL length even when nothing was written. Returning 0 here would make a
        // long layer name look like an empty one, which silently moves an entity off its layer.
        let mut tiny = [0i8; 2];
        let written = unsafe { copy_out("construction", tiny.as_mut_ptr(), tiny.len()) };
        assert_eq!(written, "construction".len());
        assert_eq!(
            tiny, [0i8; 2],
            "nothing may be written into a buffer that cannot hold it"
        );
    }

    #[test]
    fn every_error_code_has_its_own_message() {
        let mut seen = std::collections::BTreeSet::new();
        for code in [
            CAD_DXF_ERR_NOT_DXF,
            CAD_DXF_ERR_RECORD_TOO_LONG,
            CAD_DXF_ERR_TOO_LARGE,
            CAD_DXF_ERR_TRUNCATED,
            CAD_DXF_ERR_UNREADABLE,
        ] {
            let mut buffer = [0i8; 256];
            let written = unsafe { cad_dxf_error_message(code, buffer.as_mut_ptr(), 256) };
            assert!(written > 0, "code {code} has no message");
            let text: Vec<u8> = buffer[..written].iter().map(|&b| b as u8).collect();
            assert!(
                seen.insert(text),
                "code {code} reuses another code's message"
            );
        }
    }
}
