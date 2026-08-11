//! Shared helpers for the acceptance suite. The tests themselves live in `tests/`.

pub use cad::*;

/// Builds a session or explains, once, why the whole suite cannot run.
pub fn session() -> Session {
    Session::new().expect("could not create a CAD session — is libcad_abi built?")
}

/// Recomputes and asserts nothing failed, printing the per-object error if something did.
pub fn recompute_ok(s: &mut Session) -> RecomputeReport {
    let report = s.recompute().expect("recompute call failed");
    assert!(
        report.all_succeeded(),
        "recompute reported {} failed and {} blocked objects: {report:?}",
        report.failed,
        report.blocked
    );
    report
}

/// Probes a file through a throwaway session. Import reporting needs no document.
pub fn import_report(path: &str) -> Result<ImportReport> {
    let s = Session::new()?;
    s.probe_import(path, UnitSystem::Millimetre)
}

/// Volume of a box, for asserting a boolean actually removed material.
pub fn box_volume(dx: f64, dy: f64, dz: f64) -> f64 {
    dx * dy * dz
}
