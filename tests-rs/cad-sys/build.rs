//! Locates the C ABI shared library built by CMake.
//!
//! Deliberately does NOT invoke CMake. Driving a 40-minute OCCT build from `cargo test`
//! makes the Rust suite hostage to the C++ build's state and produces incomprehensible
//! failures. The contract is explicit instead: CMake builds first, cargo consumes the
//! result, and if the library is missing you get a message that says exactly that.

use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=CAD_BUILD_DIR");

    let build_dir = std::env::var("CAD_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| {
            // tests-rs/cad-sys -> repo root -> build
            let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
            manifest
                .parent()
                .and_then(|p| p.parent())
                .expect("cad-sys must live at <repo>/tests-rs/cad-sys")
                .join("build")
        });

    let lib_dir = build_dir.join("core/abi");
    let names = [
        "libcad_abi.dylib",
        "libcad_abi.so",
        "cad_abi.dll",
        "cad_abi.lib",
    ];
    if !names.iter().any(|n| lib_dir.join(n).exists()) {
        panic!(
            "cad_abi not found in {}.\n\
             Build the C++ core first:\n\
             \n    cmake --build build -j\n\n\
             or point CAD_BUILD_DIR at your build directory.",
            lib_dir.display()
        );
    }

    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib=cad_abi");

    // Let the test binaries find the dylib at run time without the caller exporting
    // DYLD_LIBRARY_PATH / LD_LIBRARY_PATH by hand.
    #[cfg(target_os = "macos")]
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    #[cfg(target_os = "linux")]
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());

    println!("cargo:rerun-if-changed={}", lib_dir.display());
}
