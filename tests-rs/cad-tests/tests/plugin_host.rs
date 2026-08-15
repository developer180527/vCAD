//! The host vtable, driven exactly as a plugin would drive it.
//!
//! No loader and no shared library yet: this calls the function pointers directly, which is the
//! same code path a loaded plugin takes minus `dlopen`. That is deliberate. The boundary is
//! testable long before the loader exists, and ADR 0011's enforcement point 4 wants the vtable
//! exercised by the same suite that exercises the Session API so the two cannot drift.
//!
//! These tests check the promises in PLUGIN_CONTRACT.md §3 — the ones that are cheap to write down
//! and expensive to actually hold.

use std::ffi::{CStr, CString};
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

    // --- appended in ABI 1.14: compute context ---
    pub compute_input_count:
        Option<extern "C" fn(*mut c_void, CadComputeCtx, *mut u32) -> CadStatus>,
    pub compute_input_shape:
        Option<extern "C" fn(*mut c_void, CadComputeCtx, u32, *mut CadShape) -> CadStatus>,
    pub compute_param_real:
        Option<extern "C" fn(*mut c_void, CadComputeCtx, *const c_char, *mut f64) -> CadStatus>,
    pub compute_param_int:
        Option<extern "C" fn(*mut c_void, CadComputeCtx, *const c_char, *mut i64) -> CadStatus>,
    pub compute_param_text:
        Option<extern "C" fn(*mut c_void, CadComputeCtx, *const c_char, *mut CadStr) -> CadStatus>,
    pub compute_set_output:
        Option<extern "C" fn(*mut c_void, CadComputeCtx, CadShape) -> CadStatus>,
    pub compute_fail: Option<
        extern "C" fn(*mut c_void, CadComputeCtx, *const c_char, *const c_char) -> CadStatus,
    >,
}

pub type CadComputeCtx = u64;

/// Only the prefix, for the same append-only reason as CadHostPrefix.
#[repr(C)]
pub struct CadFeatureDescPrefix {
    pub struct_size: u32,
    pub struct_version: u32,
    pub type_name: *const c_char,
    pub compute_version: u32,
    pub reserved0: u32,
    pub param_schema_version: u32,
    pub reserved1: u32,
    pub plugin_ctx: *mut c_void,
    pub compute: Option<extern "C" fn(*mut c_void, CadComputeCtx) -> CadStatus>,
    pub external_inputs: Option<extern "C" fn()>,
    pub migrate_params: Option<extern "C" fn()>,
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

// ── step 3b: a feature registered by a plugin, computed by the engine ────────────────────
//
// The first end-to-end proof the plugin stack works: register a feature type, add an object of
// that type to the document, recompute, and find geometry in the tree WITH names attached.
//
// In-process rather than a loaded .so on purpose. The loader is built last (PLUGIN_CONTRACT.md
// §8) precisely so the contract is not frozen around its first client, and this exercises every
// part of the boundary the loader will use without existing yet.

use std::sync::atomic::{AtomicU32, Ordering};

extern "C" {
    fn cad_object_add(session: u64, ty: *const c_char, out: *mut u64) -> CadStatus;
    fn cad_object_set_real(session: u64, object: u64, prop: *const c_char, v: f64) -> CadStatus;
    fn cad_recompute(session: u64, out: *mut CadRecomputeReport) -> CadStatus;
    fn cad_object_is_valid_shape(session: u64, object: u64, out: *mut i32) -> CadStatus;
    fn cad_object_face_count(session: u64, object: u64, out: *mut u64) -> CadStatus;
    fn cad_object_error(session: u64, object: u64) -> *const c_char;
    fn cad_mesh_element_name(session: u64, object: u64, slot: u32) -> CadStr;
}

#[repr(C)]
#[derive(Default)]
struct CadRecomputeReport {
    computed: u64,
    cached: u64,
    skipped: u64,
    failed: u64,
    blocked: u64,
    needs_plugin: u64,
}

/// The fake plugin's own state, reached through `plugin_ctx`.
///
/// NOT globals. The first version kept the host vtable in a `static mut`, which passed under
/// `--test-threads=1` and aborted the moment the suite ran in parallel — each test overwriting
/// the pointer the others were using. `plugin_ctx` exists precisely so a plugin carries its own
/// state instead, and a test harness that cheats around it is not testing the boundary it claims
/// to.
struct DemoState {
    host: *const CadHostPrefix,
    host_ctx: *mut c_void,
    /// Compute invocations, so a test can tell a cache hit from a recompute.
    calls: AtomicU32,
}

/// A feature that builds a cube from one parameter. The smallest thing that exercises the whole
/// path: read a parameter, build geometry through the host, hand it back.
extern "C" fn demo_compute(plugin_ctx: *mut c_void, cc: CadComputeCtx) -> CadStatus {
    let state = unsafe { &*(plugin_ctx as *const DemoState) };
    state.calls.fetch_add(1, Ordering::SeqCst);
    let (host, ctx) = (unsafe { &*state.host }, state.host_ctx);

    let name = CString::new("size").unwrap();
    let mut size = 0.0f64;
    let status = (host.compute_param_real.unwrap())(ctx, cc, name.as_ptr(), &mut size);
    if status != CAD_OK {
        return status;
    }

    // Refused through compute_fail, so the model tree shows a sentence rather than a code. This
    // is the difference PLUGIN_CONTRACT.md §7.2 argues for at length.
    if size <= 0.0 {
        let message = CString::new("The cube size must be greater than zero.").unwrap();
        let detail = CString::new("demo_compute: size <= 0").unwrap();
        (host.compute_fail.unwrap())(ctx, cc, message.as_ptr(), detail.as_ptr());
        return CAD_ERR_INVALID_INPUT;
    }

    let mut shape: CadShape = 0;
    let status = (host.make_box.unwrap())(ctx, size, size, size, &mut shape);
    if status != CAD_OK {
        return status;
    }
    (host.compute_set_output.unwrap())(ctx, cc, shape)
}

struct Plugin {
    host: Host,
    /// Boxed so its address is stable while `plugin_ctx` points at it, and dropped with the test.
    state: Box<DemoState>,
    _type_name: CString,
}

impl Plugin {
    /// Registers the demo feature and returns the session it lives in.
    fn register(type_name: &str) -> Self {
        let host = Host::new();
        let state = Box::new(DemoState {
            host: host.host,
            host_ctx: host.ctx(),
            calls: AtomicU32::new(0),
        });
        let type_name = CString::new(type_name).unwrap();

        let desc = CadFeatureDescPrefix {
            struct_size: std::mem::size_of::<CadFeatureDescPrefix>() as u32,
            struct_version: 1,
            type_name: type_name.as_ptr(),
            compute_version: 1,
            reserved0: 0,
            param_schema_version: 1,
            reserved1: 0,
            plugin_ctx: &*state as *const DemoState as *mut c_void,
            compute: Some(demo_compute),
            external_inputs: None,
            migrate_params: None,
        };

        let register: extern "C" fn(*mut c_void, *const CadFeatureDescPrefix, *const c_void, u32)
            -> CadStatus = unsafe { std::mem::transmute(host.h().register_feature.unwrap()) };
        let status = register(host.ctx(), &desc, std::ptr::null(), 0);
        assert_eq!(status, CAD_OK, "register_feature failed: {}", host.last_error());

        Plugin { host, state, _type_name: type_name }
    }

    fn add_object(&self, ty: &str, size: f64) -> u64 {
        let ty = CString::new(ty).unwrap();
        let mut object = 0u64;
        let status = unsafe { cad_object_add(self.host.session, ty.as_ptr(), &mut object) };
        assert_eq!(status, CAD_OK, "add failed: {}", self.host.last_error());
        let prop = CString::new("size").unwrap();
        unsafe { cad_object_set_real(self.host.session, object, prop.as_ptr(), size) };
        object
    }

    fn recompute(&self) -> CadRecomputeReport {
        let mut report = CadRecomputeReport::default();
        let status = unsafe { cad_recompute(self.host.session, &mut report) };
        assert_eq!(status, CAD_OK, "recompute call failed: {}", self.host.last_error());
        report
    }

    fn object_error(&self, object: u64) -> String {
        unsafe {
            CStr::from_ptr(cad_object_error(self.host.session, object))
                .to_string_lossy()
                .into_owned()
        }
    }
}

#[test]
fn a_plugin_feature_computes_into_the_document_with_names() {
    let plugin = Plugin::register("com.vcad.test.Cube");
    let object = plugin.add_object("com.vcad.test.Cube", 25.0);

    let report = plugin.recompute();
    assert_eq!(
        report.failed + report.blocked,
        0,
        "the plugin feature failed: {}",
        plugin.object_error(object)
    );
    assert_eq!(report.computed, 1, "exactly the one plugin feature computed");

    let mut valid = 0i32;
    unsafe { cad_object_is_valid_shape(plugin.host.session, object, &mut valid) };
    assert_eq!(valid, 1, "a plugin feature must produce a valid solid");

    let mut faces = 0u64;
    unsafe { cad_object_face_count(plugin.host.session, object, &mut faces) };
    assert_eq!(faces, 6, "a cube has six faces");

    // The part that matters. Geometry that arrives unnamed cannot be built on: a fillet on one of
    // these faces would have nothing to reference and would break on the next edit. §4.2 requires
    // plugins to name what they create, and this is the assertion that it actually happened.
    let name = unsafe { cad_mesh_element_name(plugin.host.session, object, 0) };
    assert!(
        !name.data.is_null() && name.len > 0,
        "the plugin's output reached the document with NO element names"
    );
}

#[test]
fn a_plugin_feature_that_fails_says_why_in_its_own_words() {
    let plugin = Plugin::register("com.vcad.test.Cube");
    let object = plugin.add_object("com.vcad.test.Cube", -5.0);

    let report = plugin.recompute();
    assert_eq!(report.failed, 1, "a negative size must fail the feature");

    let message = plugin.object_error(object);
    assert!(
        message.contains("greater than zero"),
        "the plugin's own message must survive to the model tree, got: {message:?}"
    );
}

/// Two objects of the same plugin type, identical parameters — the second must come from the
/// cache rather than running compute again.
///
/// This is the plugin stack meeting the DDC, and it only works because the feature's output is
/// named deterministically. It is also why the naming serial had to be fixed first: a compute
/// whose names varied per call would produce a cache entry that disagreed with a fresh compute.
#[test]
fn identical_plugin_features_share_a_cache_entry() {
    let plugin = Plugin::register("com.vcad.test.Cube");
    plugin.add_object("com.vcad.test.Cube", 30.0);
    plugin.add_object("com.vcad.test.Cube", 30.0);
    let report = plugin.recompute();

    assert_eq!(report.failed + report.blocked, 0);
    let calls = plugin.state.calls.load(Ordering::SeqCst);
    assert_eq!(
        calls, 1,
        "two identical plugin features ran compute {calls} times; the second must be a cache hit"
    );
    assert_eq!(report.cached, 1, "the second is served from the cache");
}

#[test]
fn registering_the_same_feature_type_twice_is_refused() {
    let plugin = Plugin::register("com.vcad.test.Cube");

    let type_name = CString::new("com.vcad.test.Cube").unwrap();
    let desc = CadFeatureDescPrefix {
        struct_size: std::mem::size_of::<CadFeatureDescPrefix>() as u32,
        struct_version: 1,
        type_name: type_name.as_ptr(),
        compute_version: 1,
        reserved0: 0,
        param_schema_version: 1,
        reserved1: 0,
        plugin_ctx: &*plugin.state as *const DemoState as *mut c_void,
        compute: Some(demo_compute),
        external_inputs: None,
        migrate_params: None,
    };
    let register: extern "C" fn(*mut c_void, *const CadFeatureDescPrefix, *const c_void, u32)
        -> CadStatus = unsafe { std::mem::transmute(plugin.host.h().register_feature.unwrap()) };

    // Refused, not replaced: two plugins claiming one type name would decide between themselves
    // by load order, and a document referencing that type would mean different geometry depending
    // on what happened to be installed.
    let status = register(plugin.host.ctx(), &desc, std::ptr::null(), 0);
    assert_eq!(status, CAD_ERR_INVALID_INPUT);
}

#[test]
fn a_stale_compute_handle_is_refused_rather_than_dangling() {
    let plugin = Plugin::register("com.vcad.test.Cube");
    plugin.add_object("com.vcad.test.Cube", 20.0);
    plugin.recompute();

    // A handle from a compute that has returned. This is why CadComputeCtx is an index into a
    // host map rather than a pointer to a ComputeContext: a plugin that stores one and uses it
    // later gets a clean rejection instead of a dangling reference into a frame that is gone.
    let mut count = 0u32;
    let status = (plugin.host.h().compute_input_count.unwrap())(plugin.host.ctx(), 1, &mut count);
    assert_eq!(status, CAD_ERR_BAD_HANDLE);
}

// ── a plugin feature inside the document's lifecycle ─────────────────────────────────────
//
// Everything above tests a plugin feature computing. These test it EXISTING: surviving the
// operations a user performs on a document that contains one. That is a different set of code
// paths — history, serialisation, invalidation — none of which knew about plugin types until now.

extern "C" {
    fn cad_undo(session: u64, out: *mut i32) -> CadStatus;
    fn cad_redo(session: u64, out: *mut i32) -> CadStatus;
    fn cad_document_digest(session: u64, out: *mut u64) -> CadStatus;
    fn cad_object_count(session: u64, out: *mut u64) -> CadStatus;
    fn cad_document_save(session: u64, path: *const c_char) -> CadStatus;
}

impl Plugin {
    fn digest(&self) -> u64 {
        let mut d = 0u64;
        unsafe { cad_document_digest(self.host.session, &mut d) };
        d
    }
    fn undo(&self) -> bool {
        let mut out = 0i32;
        unsafe { cad_undo(self.host.session, &mut out) };
        out != 0
    }
    fn redo(&self) -> bool {
        let mut out = 0i32;
        unsafe { cad_redo(self.host.session, &mut out) };
        out != 0
    }
}

/// A plugin feature must survive undo and redo like any other.
///
/// History predates plugins entirely, and a feature type it has never seen is exactly the sort of
/// thing that gets dropped on the way back up. Compared by digest, so this is about what the
/// document IS rather than what it reports.
#[test]
fn a_plugin_feature_survives_undo_and_redo() {
    let plugin = Plugin::register("com.vcad.test.Cube");
    plugin.add_object("com.vcad.test.Cube", 20.0);
    plugin.recompute();
    let before = plugin.digest();

    let mut undos = 0;
    while plugin.undo() {
        undos += 1;
        assert!(undos < 100, "undo did not terminate");
    }
    assert!(undos > 0, "adding a plugin feature must be undoable at all");

    for i in 0..undos {
        assert!(plugin.redo(), "redo ran out after {i} of {undos} steps");
    }

    assert_eq!(
        plugin.digest(),
        before,
        "undoing and redoing a plugin feature did not restore the document"
    );

    // And it still computes afterwards — a restored feature that cannot recompute is a document
    // the user cannot edit further.
    let report = plugin.recompute();
    assert_eq!(report.failed + report.blocked, 0, "the restored plugin feature will not compute");
}

/// A document containing a plugin feature must save and reopen — WITH the plugin present.
///
/// The harder case, where the plugin is absent, is step 4 (§4A) and is not built yet. This is its
/// prerequisite: if a plugin feature cannot round-trip even when its plugin IS registered, the
/// unknown-feature path has nothing to stand on.
#[test]
fn a_plugin_feature_round_trips_through_a_saved_file() {
    let plugin = Plugin::register("com.vcad.test.Cube");
    plugin.add_object("com.vcad.test.Cube", 32.0);
    plugin.recompute();

    let count_before = {
        let mut c = 0u64;
        unsafe { cad_object_count(plugin.host.session, &mut c) };
        c
    };
    let digest_before = plugin.digest();

    let path = std::env::temp_dir().join(format!("cad-plugin-{}.vcad", std::process::id()));
    let path_c = CString::new(path.to_str().unwrap()).unwrap();
    let status = unsafe { cad_document_save(plugin.host.session, path_c.as_ptr()) };
    assert_eq!(status, CAD_OK, "saving failed: {}", plugin.host.last_error());

    // Reopened into a session that has the SAME feature registered, which is what happens when
    // the plugin is installed on both machines.
    let reopened = Plugin::register("com.vcad.test.Cube");
    let status = unsafe { cad_document_open(reopened.host.session, path_c.as_ptr()) };
    assert_eq!(status, CAD_OK, "reopening failed: {}", reopened.host.last_error());

    let mut count_after = 0u64;
    unsafe { cad_object_count(reopened.host.session, &mut count_after) };
    assert_eq!(count_after, count_before, "reopening lost the plugin feature");
    assert_eq!(
        reopened.digest(),
        digest_before,
        "the reopened document differs from the one that was saved"
    );

    let report = reopened.recompute();
    assert_eq!(
        report.failed + report.blocked,
        0,
        "the reopened plugin feature will not compute"
    );

    let _ = std::fs::remove_file(&path);
}

extern "C" {
    fn cad_document_open(session: u64, path: *const c_char) -> CadStatus;
    fn cad_object_state(session: u64, object: u64, out: *mut i32) -> CadStatus;
}

// ── §4A: a document must outlive the plugin that made it ────────────────────────────────



/// Opens a saved document in a session that does NOT have the plugin registered.
///
/// The failure this guards against is the one that ends platforms: opening a colleague's file,
/// seeing "unknown feature, removed", saving, and destroying their work. So the assertion is not
/// about behaviour first — it is about DATA. Every property must still be there afterwards, and
/// saving again must produce a file the plugin's owner can still open.
#[test]
fn a_document_opens_without_the_plugin_that_made_it_and_loses_nothing() {
    let path = std::env::temp_dir().join(format!("cad-4a-{}.vcad", std::process::id()));
    let path_c = CString::new(path.to_str().unwrap()).unwrap();

    // 1. Author the document WITH the plugin.
    let digest_with_plugin = {
        let plugin = Plugin::register("com.vcad.test.Cube");
        plugin.add_object("com.vcad.test.Cube", 42.0);
        plugin.recompute();
        let status = unsafe { cad_document_save(plugin.host.session, path_c.as_ptr()) };
        assert_eq!(status, CAD_OK, "saving failed: {}", plugin.host.last_error());
        plugin.digest()
    };

    // 2. Open it in a plain session — no plugin, no such feature type.
    let bare = Host::new();
    let status = unsafe { cad_document_open(bare.session, path_c.as_ptr()) };
    assert_eq!(
        status, CAD_OK,
        "a document containing a plugin feature must OPEN without the plugin: {}",
        bare.last_error()
    );

    let mut count = 0u64;
    unsafe { cad_object_count(bare.session, &mut count) };
    assert_eq!(count, 1, "the unknown feature was dropped on load");

    // 3. Recompute. The feature cannot be built, and that is expected — what matters is that it
    //    is reported legibly and that nothing is destroyed.
    let mut report = CadRecomputeReport::default();
    unsafe { cad_recompute(bare.session, &mut report) };

    let object: u64 = 1;
    let message = unsafe {
        CStr::from_ptr(cad_object_error(bare.session, object))
            .to_string_lossy()
            .into_owned()
    };
    // Rule 2: distinguishable from a broken feature, so the shell can grey it rather than red it.
    // "Your file is damaged" and "you are missing software" must not look the same.
    let mut state = 0i32;
    unsafe { cad_object_state(bare.session, object, &mut state) };
    assert_eq!(
        state, 4,
        "a feature whose plugin is missing must be NeedsPlugin, not Failed — got state {state}"
    );
    assert_eq!(
        report.needs_plugin, 1,
        "the recompute report must count a missing plugin apart from a failure"
    );
    assert_eq!(
        report.failed, 0,
        "a missing plugin is not a failed document; reporting it as one tells the user their \
         file is damaged when it is not"
    );

    // And the message names the plugin AND says the data is safe. The second half matters as
    // much: a user who believes their parameters are gone will not trust the file again.
    assert!(
        message.contains("com.vcad.test.Cube"),
        "the message must name the plugin that is missing, got: {message:?}"
    );
    assert!(
        message.contains("unchanged"),
        "the message must say the settings survived, got: {message:?}"
    );

    // 4. Save it BACK from the session that could not compute it, and reopen with the plugin.
    //    This is the step that destroys work if preservation is only skin-deep.
    let status = unsafe { cad_document_save(bare.session, path_c.as_ptr()) };
    assert_eq!(status, CAD_OK, "re-saving failed: {}", bare.last_error());

    let restored = Plugin::register("com.vcad.test.Cube");
    let status = unsafe { cad_document_open(restored.host.session, path_c.as_ptr()) };
    assert_eq!(status, CAD_OK, "reopening with the plugin failed");

    assert_eq!(
        restored.digest(),
        digest_with_plugin,
        "a round trip through a session WITHOUT the plugin changed the document — \
         the user's parameters did not survive"
    );

    // 5. And it computes again, because nothing was lost.
    let report = restored.recompute();
    assert_eq!(
        report.failed + report.blocked,
        0,
        "reinstalling the plugin did not restore full behaviour: {}",
        restored.object_error(object)
    );

    let _ = std::fs::remove_file(&path);
}

/// Rule 3: a feature downstream of one whose plugin is missing goes Blocked with a reason.
///
/// Without this the document opens, the plugin feature is greyed honestly, and everything built
/// on it fails with no explanation — which sends the user hunting through features that are
/// perfectly fine.
#[test]
fn a_feature_downstream_of_a_missing_plugin_is_blocked_with_a_reason() {
    let path = std::env::temp_dir().join(format!("cad-4a-down-{}.vcad", std::process::id()));
    let path_c = CString::new(path.to_str().unwrap()).unwrap();

    {
        let plugin = Plugin::register("com.vcad.test.Cube");
        let cube = plugin.add_object("com.vcad.test.Cube", 30.0);
        plugin.recompute();
        // A built-in that consumes the plugin's output.
        let ty = CString::new("Translate").unwrap();
        let mut moved = 0u64;
        unsafe { cad_object_add(plugin.host.session, ty.as_ptr(), &mut moved) };
        let prop = CString::new("base").unwrap();
        unsafe { cad_object_set_object(plugin.host.session, moved, prop.as_ptr(), cube) };
        plugin.recompute();
        assert_eq!(
            unsafe { cad_document_save(plugin.host.session, path_c.as_ptr()) },
            CAD_OK
        );
    }

    let bare = Host::new();
    assert_eq!(unsafe { cad_document_open(bare.session, path_c.as_ptr()) }, CAD_OK);
    let mut report = CadRecomputeReport::default();
    unsafe { cad_recompute(bare.session, &mut report) };

    assert_eq!(report.needs_plugin, 1, "the plugin feature needs its plugin");
    assert_eq!(report.blocked, 1, "the feature built on it must be blocked");

    // Object 2 is the Translate. Its message must point at what actually stopped it.
    let message = unsafe {
        CStr::from_ptr(cad_object_error(bare.session, 2))
            .to_string_lossy()
            .into_owned()
    };
    assert!(
        !message.trim().is_empty(),
        "a feature blocked by a missing plugin must say why"
    );

    let _ = std::fs::remove_file(&path);
}

extern "C" {
    fn cad_object_set_object(session: u64, object: u64, prop: *const c_char, target: u64)
        -> CadStatus;
}
