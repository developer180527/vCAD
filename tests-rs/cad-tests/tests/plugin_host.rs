//! The host vtable, driven exactly as a plugin would drive it.
//!
//! No loader and no shared library yet: this calls the function pointers directly, which is the
//! same code path a loaded plugin takes minus `dlopen`. That is deliberate. The boundary is
//! testable long before the loader exists, and ADR 0011's enforcement point 4 wants the vtable
//! exercised by the same suite that exercises the Session API so the two cannot drift.
//!
//! These tests check the promises in PLUGIN_CONTRACT.md §3 — the ones that are cheap to write down
//! and expensive to actually hold.

use std::ffi::CStr;
use std::os::raw::{c_char, c_void};

pub type CadShape = u64;
pub type CadStatus = i32;

const CAD_OK: CadStatus = 0;
const CAD_ERR_INVALID_INPUT: CadStatus = 1;
const CAD_ERR_NAMING_LOST: CadStatus = 6;
const CAD_ERR_BAD_HANDLE: CadStatus = 9;

#[repr(C)]
pub struct CadStr {
    pub data: *const c_char,
    pub len: usize,
}

/// Mirrors `struct CadHost` up to the entries these tests use.
///
/// Only the prefix is declared, and that is safe for exactly the reason the whole ABI exists:
/// fields are append-only, so a prefix stays valid forever. `struct_size` is checked below before
/// any pointer is touched — which is the same negotiation a real plugin performs.
#[repr(C)]
pub struct CadHostPrefix {
    pub struct_size: u32,
    pub struct_version: u32,
    pub abi_major: u32,
    pub abi_minor: u32,
    pub host_ctx: *mut c_void,

    pub log: Option<extern "C" fn(*mut c_void, i32, *const c_char)>,
    pub last_error: Option<extern "C" fn(*mut c_void) -> CadStr>,

    pub make_box: Option<extern "C" fn(*mut c_void, f64, f64, f64, *mut CadShape) -> CadStatus>,
    pub boolean_fuse:
        Option<extern "C" fn(*mut c_void, CadShape, CadShape, *mut CadShape) -> CadStatus>,
    pub boolean_cut:
        Option<extern "C" fn(*mut c_void, CadShape, CadShape, *mut CadShape) -> CadStatus>,
    pub fillet_edges: Option<extern "C" fn()>,
    pub shape_validate: Option<extern "C" fn(*mut c_void, CadShape) -> CadStatus>,
    pub shape_release: Option<extern "C" fn(*mut c_void, CadShape)>,

    pub element_resolve: Option<
        extern "C" fn(*mut c_void, CadShape, *const CadElementId, *mut CadShape) -> CadStatus,
    >,
    pub element_name_of: Option<
        extern "C" fn(*mut c_void, CadShape, CadShape, *mut CadElementId) -> CadStatus,
    >,

    // Declared but unused, purely to reach what follows them. The prefix trick only works if the
    // prefix is complete up to the member being read.
    pub txn_begin: Option<extern "C" fn()>,
    pub txn_commit: Option<extern "C" fn()>,
    pub txn_abort: Option<extern "C" fn()>,
    pub register_feature: Option<extern "C" fn()>,
    pub register_command: Option<extern "C" fn()>,
    pub register_format: Option<extern "C" fn()>,

    // --- appended in ABI 1.13 ---
    pub shape_sub_count:
        Option<extern "C" fn(*mut c_void, CadShape, u32, *mut u32) -> CadStatus>,
    pub shape_sub_at:
        Option<extern "C" fn(*mut c_void, CadShape, u32, u32, *mut CadShape) -> CadStatus>,
}

pub const CAD_SUB_FACE: u32 = 1;
pub const CAD_SUB_EDGE: u32 = 2;
pub const CAD_SUB_VERTEX: u32 = 3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CadElementId {
    pub digest: u64,
    pub text: *const c_char,
    pub text_len: usize,
}

impl Default for CadElementId {
    fn default() -> Self {
        CadElementId { digest: 0, text: std::ptr::null(), text_len: 0 }
    }
}

extern "C" {
    fn cad_session_create() -> u64;
    fn cad_session_release(session: u64);
    fn cad_plugin_host(session: u64) -> *const CadHostPrefix;
}

struct Host {
    session: u64,
    host: *const CadHostPrefix,
}

impl Host {
    fn new() -> Self {
        let session = unsafe { cad_session_create() };
        assert_ne!(session, 0, "session must be created");
        let host = unsafe { cad_plugin_host(session) };
        assert!(!host.is_null(), "a session must be able to produce a host vtable");
        Host { session, host }
    }
    fn h(&self) -> &CadHostPrefix {
        unsafe { &*self.host }
    }
    fn ctx(&self) -> *mut c_void {
        self.h().host_ctx
    }
    fn box_of(&self, dx: f64, dy: f64, dz: f64) -> (CadStatus, CadShape) {
        let mut shape: CadShape = 0;
        let status = (self.h().make_box.expect("make_box is offered"))(
            self.ctx(), dx, dy, dz, &mut shape,
        );
        (status, shape)
    }
    fn last_error(&self) -> String {
        let s = (self.h().last_error.expect("last_error is offered"))(self.ctx());
        if s.data.is_null() || s.len == 0 {
            return String::new();
        }
        unsafe { CStr::from_ptr(s.data) }.to_string_lossy().into_owned()
    }
}

impl Drop for Host {
    fn drop(&mut self) {
        unsafe { cad_session_release(self.session) };
    }
}

#[test]
fn the_host_declares_its_own_size_and_generation() {
    let host = Host::new();
    let h = host.h();
    // The negotiation a plugin performs before touching anything: is this struct at least as big
    // as the prefix I was compiled against?
    assert!(
        h.struct_size as usize >= std::mem::size_of::<CadHostPrefix>(),
        "host struct_size {} is smaller than the prefix this test compiled against",
        h.struct_size
    );
    assert_eq!(h.abi_major, cad::sys::CAD_ABI_VERSION_MAJOR);
    assert_eq!(h.abi_minor, cad::sys::CAD_ABI_VERSION_MINOR);
    assert!(!h.host_ctx.is_null(), "host_ctx is passed back to every call");
}

#[test]
fn unoffered_entries_are_null_rather_than_dangling() {
    // PLUGIN_CONTRACT.md §4.5: a NULL entry means "not offered in this configuration", and plugins
    // must check. Asserted so that filling one in later is a deliberate act, and so the discipline
    // is exercised from the first day rather than discovered when a sandboxed tier appears.
    let host = Host::new();
    assert!(host.h().fillet_edges.is_none(), "fillet_edges is not offered yet");
    assert!(host.h().make_box.is_some(), "make_box IS offered");
}

#[test]
fn geometry_round_trips_through_handles() {
    let host = Host::new();
    let (status, shape) = host.box_of(10.0, 20.0, 30.0);
    assert_eq!(status, CAD_OK, "make_box failed: {}", host.last_error());
    assert_ne!(shape, 0, "a successful call must return a non-zero handle");

    let valid = (host.h().shape_validate.unwrap())(host.ctx(), shape);
    assert_eq!(valid, CAD_OK, "a box must be a valid solid: {}", host.last_error());
}

#[test]
fn a_released_handle_reports_bad_handle_instead_of_crashing() {
    // The single most important promise in §3.1, and the one whose absence produces the crashes
    // that get blamed on the host. A plugin WILL use a shape after releasing it.
    let host = Host::new();
    let (_, shape) = host.box_of(10.0, 10.0, 10.0);

    (host.h().shape_release.unwrap())(host.ctx(), shape);

    let after = (host.h().shape_validate.unwrap())(host.ctx(), shape);
    assert_eq!(after, CAD_ERR_BAD_HANDLE, "a released handle must be rejected, not dereferenced");
    assert!(!host.last_error().is_empty(), "and it must say so");
}

#[test]
fn release_is_idempotent_and_accepts_zero() {
    // The uniform rule for every release in this ABI. A plugin shutting down after a failure
    // releases things it may already have released, and a double free must not be a crash.
    let host = Host::new();
    let (_, shape) = host.box_of(5.0, 5.0, 5.0);
    let release = host.h().shape_release.unwrap();
    release(host.ctx(), shape);
    release(host.ctx(), shape);
    release(host.ctx(), 0);
}

#[test]
fn handles_are_never_reused() {
    // If ids were recycled, a stale handle would silently address a DIFFERENT shape: no error,
    // wrong geometry, and a plugin author with no way to see it. Worse than either alternative.
    let host = Host::new();
    let (_, first) = host.box_of(1.0, 1.0, 1.0);
    (host.h().shape_release.unwrap())(host.ctx(), first);
    let (_, second) = host.box_of(1.0, 1.0, 1.0);

    assert_ne!(first, second, "a freed handle id must never be handed out again");
    assert_eq!(
        (host.h().shape_validate.unwrap())(host.ctx(), first),
        CAD_ERR_BAD_HANDLE,
        "the old handle must stay dead"
    );
}

#[test]
fn a_boolean_on_a_stale_handle_is_refused() {
    let host = Host::new();
    let (_, a) = host.box_of(10.0, 10.0, 10.0);
    let (_, b) = host.box_of(5.0, 5.0, 5.0);
    (host.h().shape_release.unwrap())(host.ctx(), b);

    let mut out: CadShape = 0;
    let status = (host.h().boolean_cut.unwrap())(host.ctx(), a, b, &mut out);
    assert_eq!(status, CAD_ERR_BAD_HANDLE);
    assert_eq!(out, 0, "a failed call must not write an output handle");
}

#[test]
fn invalid_geometry_fails_legibly_rather_than_throwing() {
    // §3.2: nothing throws across the boundary. A NaN reaches the kernel guards added earlier and
    // must come back as a status plus a message, not as an exception unwinding into plugin code
    // compiled by a different toolchain -- which is undefined behaviour, not an error.
    let host = Host::new();
    let (status, shape) = host.box_of(f64::NAN, 10.0, 10.0);
    assert_eq!(status, CAD_ERR_INVALID_INPUT);
    assert_eq!(shape, 0);
    assert!(
        !host.last_error().is_empty(),
        "a refusal must carry a message a plugin author can act on"
    );
}

#[test]
fn two_sessions_do_not_share_shape_handles() {
    // Handles are per-session. A plugin holding a handle from one document must not be able to
    // reach into another -- which is also what makes "reading another plugin's state" (§6)
    // structurally impossible rather than merely forbidden.
    let a = Host::new();
    let b = Host::new();
    let (_, shape) = a.box_of(10.0, 10.0, 10.0);

    assert_eq!(
        (b.h().shape_validate.unwrap())(b.ctx(), shape),
        CAD_ERR_BAD_HANDLE,
        "a handle from another session must not resolve"
    );
}

// --- naming ---------------------------------------------------------------------------------
//
// The trap these exist to avoid: FreeCAD's topological naming problem is a PLUGIN API problem,
// because every addon holding a face reference breaks when an edit reorders faces. A plugin that
// can only say "face 3" is a plugin that breaks.
//
// element_resolve and element_name_of are now wired. What is NOT yet possible is obtaining a
// sub-shape handle to hand them: the vtable has no face or edge enumeration, so a plugin cannot
// reach a face in order to name it. Found by writing these tests -- the design reviewed clean and
// the gap only appeared when something tried to use it.
//
// Until enumeration lands, what is testable is the negative half, and the negative half is the
// half that matters most: an unknown name must be REFUSED rather than approximated.

#[test]
fn an_unknown_name_is_reported_lost_not_guessed() {
    // Falling back to "some nearby face" would be invisible until an edit reordered the geometry,
    // and would hand plugins the exact failure this API exists to prevent.
    let host = Host::new();
    let (_, shape) = host.box_of(10.0, 10.0, 10.0);

    let bogus = CadElementId { digest: 0xDEAD_BEEF, text: std::ptr::null(), text_len: 0 };
    let mut resolved: CadShape = 0;
    let status = (host.h().element_resolve.expect("naming is offered"))(
        host.ctx(),
        shape,
        &bogus,
        &mut resolved,
    );

    assert_eq!(status, CAD_ERR_NAMING_LOST, "an unknown name must be refused, never approximated");
    assert_eq!(resolved, 0, "and must not write an output handle");
    assert!(!host.last_error().is_empty());
}

#[test]
fn naming_a_shape_that_carries_no_name_is_lost_not_invented() {
    // A solid is not itself a named element -- nameprimitive binds the FACES. The honest answer is
    // NAMING_LOST, and it must stay honest: inventing a name here would make every plugin
    // reference look valid and break silently later.
    let host = Host::new();
    let (_, shape) = host.box_of(10.0, 20.0, 30.0);

    let mut id = CadElementId::default();
    let status = (host.h().element_name_of.expect("naming is offered"))(
        host.ctx(),
        shape,
        shape,
        &mut id,
    );
    assert_eq!(status, CAD_ERR_NAMING_LOST);
    assert!(!host.last_error().is_empty(), "and it must say which");
}

#[test]
fn resolving_against_a_released_shape_is_refused() {
    let host = Host::new();
    let (_, shape) = host.box_of(10.0, 10.0, 10.0);
    (host.h().shape_release.unwrap())(host.ctx(), shape);

    let id = CadElementId::default();
    let mut resolved: CadShape = 0;
    let status = (host.h().element_resolve.unwrap())(host.ctx(), shape, &id, &mut resolved);
    assert_eq!(status, CAD_ERR_BAD_HANDLE, "a dead shape cannot resolve names");
}

// ── sub-shape enumeration, and the determinism it finally makes testable ─────────────────
//
// PLUGIN_CONTRACT.md 3.4 was marked (PARTIAL) because element_resolve and element_name_of were
// wired while nothing could obtain a sub-shape to name. These close that, and the first of them
// is the reason the gap mattered more than it looked: without a face handle there was no way to
// observe that host-built shapes were being named from a session counter.

impl Host {
    fn sub_count(&self, shape: CadShape, kind: u32) -> (CadStatus, u32) {
        let mut n = 0u32;
        let status = (self.h().shape_sub_count.expect("shape_sub_count is offered"))(
            self.ctx(), shape, kind, &mut n,
        );
        (status, n)
    }

    fn sub_at(&self, shape: CadShape, kind: u32, index: u32) -> (CadStatus, CadShape) {
        let mut out: CadShape = 0;
        let status = (self.h().shape_sub_at.expect("shape_sub_at is offered"))(
            self.ctx(), shape, kind, index, &mut out,
        );
        (status, out)
    }

    /// The digest of the name of one face, which is the whole point of the chain: enumerate a
    /// face, ask what it is called, and get an answer that survives a rebuild.
    fn face_name_digest(&self, shape: CadShape, index: u32) -> u64 {
        let (status, face) = self.sub_at(shape, CAD_SUB_FACE, index);
        assert_eq!(status, CAD_OK, "shape_sub_at failed: {}", self.last_error());
        let mut id = CadElementId::default();
        let status = (self.h().element_name_of.expect("naming is offered"))(
            self.ctx(), shape, face, &mut id,
        );
        assert_eq!(status, CAD_OK, "element_name_of failed: {}", self.last_error());
        id.digest
    }
}

#[test]
fn a_solid_enumerates_its_faces_edges_and_vertices() {
    let host = Host::new();
    let (status, b) = host.box_of(20.0, 30.0, 40.0);
    assert_eq!(status, CAD_OK, "make_box failed: {}", host.last_error());

    let (status, faces) = host.sub_count(b, CAD_SUB_FACE);
    assert_eq!(status, CAD_OK);
    assert_eq!(faces, 6, "a box has six faces, deduplicated across shells");

    let (status, edges) = host.sub_count(b, CAD_SUB_EDGE);
    assert_eq!(status, CAD_OK);
    assert_eq!(edges, 12, "a box has twelve edges, each visited once");

    let (status, vertices) = host.sub_count(b, CAD_SUB_VERTEX);
    assert_eq!(status, CAD_OK);
    assert_eq!(vertices, 8, "a box has eight vertices");
}

#[test]
fn a_face_obtained_from_a_shape_can_be_named_and_resolved_back() {
    let host = Host::new();
    let (_, b) = host.box_of(20.0, 30.0, 40.0);

    let (status, face) = host.sub_at(b, CAD_SUB_FACE, 0);
    assert_eq!(status, CAD_OK, "shape_sub_at failed: {}", host.last_error());
    assert_ne!(face, 0, "a sub-shape handle is a real handle");

    let mut id = CadElementId::default();
    let status = (host.h().element_name_of.expect("naming is offered"))(
        host.ctx(), b, face, &mut id,
    );
    assert_eq!(
        status, CAD_OK,
        "a face of a host-built box must have a name: {}",
        host.last_error()
    );
    assert_ne!(id.digest, 0, "a real name has a non-zero digest");

    // And back again. This is the round trip a plugin needs to hold a face reference across a
    // rebuild — the guarantee 3.4 promises and could not previously demonstrate.
    let mut resolved: CadShape = 0;
    let status = (host.h().element_resolve.expect("resolve is offered"))(
        host.ctx(), b, &id, &mut resolved,
    );
    assert_eq!(status, CAD_OK, "resolve failed: {}", host.last_error());
    assert_ne!(resolved, 0);
}

/// **Identical requests must produce identical names.**
///
/// PLUGIN_CONTRACT.md 4.1 makes determinism the strictest rule in the contract, and recompute is
/// content-addressed, so a name that varies by session state lets the DDC serve a cached result
/// whose names disagree with what recomputing would produce.
///
/// This failed before the fix: `make_box` took its naming serial from `Session::nextShape`, a
/// counter incremented on every intern, so the second box in a session was named differently from
/// the first — and a box in a fresh session differently again, depending on what had been interned
/// before it. The serial now comes from the request.
#[test]
fn identical_geometry_is_named_identically() {
    let host = Host::new();
    let (_, first) = host.box_of(20.0, 30.0, 40.0);
    let (_, second) = host.box_of(20.0, 30.0, 40.0);

    for index in 0..6 {
        assert_eq!(
            host.face_name_digest(first, index),
            host.face_name_digest(second, index),
            "face {index} of two identical boxes got different names — the naming serial is \
             session state again, and every plugin's cached geometry is now suspect"
        );
    }
}

/// The other half: names must still DISTINGUISH different geometry, or the test above could be
/// satisfied by naming everything the same.
#[test]
fn different_geometry_is_named_differently() {
    let host = Host::new();
    let (_, small) = host.box_of(20.0, 30.0, 40.0);
    let (_, large) = host.box_of(50.0, 30.0, 40.0);

    let differs = (0..6).any(|i| host.face_name_digest(small, i) != host.face_name_digest(large, i));
    assert!(differs, "two differently-sized boxes were named identically");
}

/// Determinism has to hold across SESSIONS, not just within one — that is the case the DDC
/// actually exercises, since a cached result outlives the session that produced it.
#[test]
fn naming_is_identical_across_sessions() {
    let digests_of = || {
        let host = Host::new();
        let (_, b) = host.box_of(20.0, 30.0, 40.0);
        (0..6).map(|i| host.face_name_digest(b, i)).collect::<Vec<_>>()
    };
    assert_eq!(
        digests_of(),
        digests_of(),
        "the same box named differently in two sessions"
    );
}

#[test]
fn an_unknown_sub_shape_kind_is_refused_rather_than_guessed() {
    let host = Host::new();
    let (_, b) = host.box_of(10.0, 10.0, 10.0);
    let (status, _) = host.sub_count(b, 99);
    assert_eq!(
        status, CAD_ERR_INVALID_INPUT,
        "an unknown kind must be refused, not mapped onto whatever enumerator sits there"
    );
}

#[test]
fn an_out_of_range_sub_shape_index_is_refused() {
    let host = Host::new();
    let (_, b) = host.box_of(10.0, 10.0, 10.0);
    let (status, _) = host.sub_at(b, CAD_SUB_FACE, 999);
    assert_eq!(status, CAD_ERR_INVALID_INPUT);
}

#[test]
fn enumerating_a_released_shape_is_refused() {
    let host = Host::new();
    let (_, b) = host.box_of(10.0, 10.0, 10.0);
    (host.h().shape_release.expect("release is offered"))(host.ctx(), b);

    let (status, _) = host.sub_count(b, CAD_SUB_FACE);
    assert_eq!(status, CAD_ERR_BAD_HANDLE);
}
