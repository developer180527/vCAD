//! The Rust side of the C++ core, for parsing input we do not control.
//!
//! # Why this crate exists before it does anything
//!
//! This is the stub PICKUP.md asks for: **prove the build integration before writing any parser.**
//! A Rust static library consumed by the C++ build is a different proposition from the test-only
//! Rust already in `tests-rs/` — that one is driven BY cargo and consumes the C++ library, while
//! this one is driven BY cmake and is consumed by it. It has to work on three platforms, in CI,
//! without `cmake --build` ever reaching the network. If any of that does not hold, it is far
//! cheaper to discover on a function that returns a version number than on a DXF parser.
//!
//! # The boundary rules, which are the same as the plugin ABI's
//!
//! Everything crossing this seam is C. No Rust types, no `String`, no slices with lifetimes the
//! other side cannot see. Buffers are caller-allocated and length-checked, which is the only
//! arrangement where neither side has to guess who frees what.
//!
//! Panics abort rather than unwind: unwinding into a C++ frame is undefined behaviour, and a
//! parser whose job is to return errors should never panic anyway.

use std::os::raw::{c_char, c_int};

/// ABI generation for this library, checked by the C++ side at startup.
///
/// Same reasoning as the plugin ABI's version pair: the C++ and Rust halves are compiled
/// separately and could, through a stale build directory, be different vintages. A mismatch that
/// is caught is a clear message; one that is not is a wrong answer from a parser.
pub const CAD_PARSE_ABI: c_int = 1;

/// Returns the ABI generation this library was built as.
///
/// The trivial function the integration is proven on. It takes nothing, allocates nothing, and
/// cannot fail — so if this returns the right number through CMake, on this platform, the linking,
/// the symbol naming and the runtime are all working, and nothing about the answer can be
/// confused with a parser bug.
#[no_mangle]
pub extern "C" fn cad_parse_abi_version() -> c_int {
    CAD_PARSE_ABI
}

/// Writes a short human-readable build identity into `buffer`, returning the bytes written.
///
/// Present because a version number alone cannot distinguish "the integration works" from "the
/// linker found an old archive". A string proves the running code came from this source.
///
/// Caller-allocated and length-checked: the C++ side owns the memory, so there is no allocator
/// shared across the boundary and nothing for either side to free on the other's behalf. Returns
/// 0 and writes nothing when the buffer is null or too small, rather than truncating — a
/// half-written identity is worse than none.
///
/// # Safety
/// `buffer` must be valid for writes of `capacity` bytes, or null.
#[no_mangle]
pub unsafe extern "C" fn cad_parse_build_id(buffer: *mut c_char, capacity: usize) -> usize {
    const ID: &[u8] = b"cad-parse 0.1.0";
    if buffer.is_null() || capacity < ID.len() + 1 {
        return 0;
    }
    std::ptr::copy_nonoverlapping(ID.as_ptr(), buffer as *mut u8, ID.len());
    *buffer.add(ID.len()) = 0;
    ID.len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_build_id_refuses_a_buffer_it_would_overflow() {
        let mut small = [0i8; 4];
        let written = unsafe { cad_parse_build_id(small.as_mut_ptr(), small.len()) };
        assert_eq!(written, 0, "a buffer too small must be refused, not truncated");
        let written = unsafe { cad_parse_build_id(std::ptr::null_mut(), 64) };
        assert_eq!(written, 0, "a null buffer must be refused");
    }

    #[test]
    fn the_build_id_fits_exactly_when_it_should() {
        let mut buffer = [0i8; 64];
        let written = unsafe { cad_parse_build_id(buffer.as_mut_ptr(), buffer.len()) };
        assert!(written > 0 && written < buffer.len());
        assert_eq!(buffer[written], 0, "the result must be NUL-terminated");
    }
}
