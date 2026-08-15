//! Whether the host will load a given plugin, in both directions.
//!
//! ADR 0011 promises that a plugin compiled today runs a decade from now, and that a plugin built
//! for a future host is REFUSED rather than loaded into a null function pointer. Both halves are
//! stated in prose in the ADR; this is the half that is executable, so the rule is tested rather
//! than described.

use std::ffi::CStr;
use std::os::raw::c_char;

use cad::sys::cad_abi_accepts;

/// (accepted, reason). The reason is empty when accepted.
fn accepts(major: u32, min_host_minor: u32) -> (bool, String) {
    let mut reason: *const c_char = std::ptr::null();
    let ok = unsafe { cad_abi_accepts(major, min_host_minor, &mut reason) };
    let text = if reason.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(reason) }
            .to_string_lossy()
            .into_owned()
    };
    (ok != 0, text)
}

const MAJOR: u32 = cad::sys::CAD_ABI_VERSION_MAJOR;
const MINOR: u32 = cad::sys::CAD_ABI_VERSION_MINOR;

#[test]
fn a_plugin_built_for_this_exact_version_is_accepted() {
    let (ok, _) = accepts(MAJOR, MINOR);
    assert!(ok, "the host must accept a plugin built against itself");
}

#[test]
fn an_old_plugin_is_accepted_by_a_newer_host() {
    // THE decade promise, in one assertion. Every minor this host has passed through must still
    // load: a plugin built against 1.0 runs on 1.10, and would run on 1.400.
    for minor in 0..=MINOR {
        let (ok, reason) = accepts(MAJOR, minor);
        assert!(
            ok,
            "a plugin requiring host minor {minor} was refused by host {MAJOR}.{MINOR}: {reason}"
        );
    }
}

#[test]
fn a_plugin_needing_a_newer_host_is_refused_legibly() {
    let (ok, reason) = accepts(MAJOR, MINOR + 1);
    assert!(!ok, "a plugin needing minor {} must be refused", MINOR + 1);

    // The message is the point. Refusing is easy; refusing in a way that tells a user what to do
    // about it is what stops this becoming a support ticket, and it is why cad_abi_accepts
    // returns a reason rather than a bare bool.
    assert!(
        !reason.is_empty() && reason.contains("newer version"),
        "refusal must explain itself, got: {reason:?}"
    );
}

#[test]
fn a_plugin_from_a_future_generation_is_refused() {
    let (ok, reason) = accepts(MAJOR + 1, 0);
    assert!(!ok, "a plugin built for ABI {} must be refused", MAJOR + 1);
    assert!(!reason.is_empty(), "refusal must explain itself");
}

#[test]
fn a_zero_major_is_refused() {
    // Not pedantry: a zeroed CadPluginDesc is what a plugin that forgot to fill it in looks like,
    // and accepting one would mean loading a library that never declared anything at all.
    let (ok, _) = accepts(0, 0);
    assert!(!ok, "abi_major 0 is not a generation and must be refused");
}

#[test]
fn acceptance_tolerates_a_null_reason() {
    // The loader will often not care why. Passing NULL must not be a crash.
    let ok = unsafe { cad_abi_accepts(MAJOR, MINOR, std::ptr::null_mut()) };
    assert!(ok != 0);
}
