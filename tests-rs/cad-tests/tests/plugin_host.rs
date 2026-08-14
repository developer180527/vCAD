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
