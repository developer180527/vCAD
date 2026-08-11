//! M2 — file interchange and the on-disk DDC tier.
//!
//! These run through the C ABI like every other acceptance test. The DDC assertions matter
//! most: a disk cache that returns *almost* the right thing is worse than no cache at all,
//! because the damage surfaces far from its cause.

use cad_tests::*;
use std::path::PathBuf;

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("cad-tests-{}-{}", name, std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// STEP round trip. The strong assertion is the volume: a reader that produces geometry of
/// the right shape but the wrong scale is the classic interchange bug, and a face count
/// would not catch it.
#[test]
fn step_round_trips_geometry_and_scale() {
    let dir = scratch("step");
    let path = dir.join("part.step");

    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let original_volume = s.volume(b).unwrap();

    s.export_file(b, path.to_str().unwrap()).unwrap();
    assert!(path.exists() && std::fs::metadata(&path).unwrap().len() > 0);

    let mut t = session();
    let imported = t.add_import(path.to_str().unwrap()).unwrap();
    recompute_ok(&mut t);

    assert!(t.is_valid_shape(imported).unwrap());
    assert_eq!(t.face_count(imported).unwrap(), 6);
    let round_tripped = t.volume(imported).unwrap();
    assert!(
        (round_tripped - original_volume).abs() < original_volume * 1e-6,
        "STEP round trip changed the volume: {original_volume} -> {round_tripped}"
    );
}

/// STL carries no unit declaration, so import must say it assumed rather than knew.
/// docs/FORMATS.md rule 2: a wrong scale is worse than a refused import.
#[test]
fn stl_import_reports_that_units_were_assumed() {
    let dir = scratch("stl");
    let path = dir.join("part.stl");

    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    s.export_file(b, path.to_str().unwrap()).unwrap();

    let report = import_report(path.to_str().unwrap()).unwrap();
    assert!(report.units_were_assumed, "STL has no units to read");
    assert!(
        !report.unsupported.is_empty(),
        "an STL import must say what it could not bring across — it has no exact surfaces"
    );
    assert!(
        report.summary().to_lowercase().contains("stl"),
        "the summary should name the format: {}",
        report.summary()
    );
}

/// An unknown extension is refused with a message a user can act on, not a crash and not a
/// silent empty document.
#[test]
fn an_unknown_format_is_refused_legibly() {
    let err = import_report("/nonexistent/thing.parasolid").expect_err("should refuse");
    assert_eq!(err.code, ErrorCode::Unsupported);
    assert!(!err.message.is_empty());

    // A file we do handle but which is not there fails differently — and also legibly.
    let err = import_report("/nonexistent/thing.step").expect_err("should fail");
    assert!(!err.message.is_empty());
}

/// The DDC's whole point: a result computed in one session is served to a NEW session from
/// disk, with no recompute. This is what makes a team cache — or CI — worth having.
#[test]
fn the_ddc_serves_a_result_across_sessions() {
    let dir = scratch("ddc-cross");

    // First session computes and stores.
    {
        let mut s = Session::with_cache(dir.to_str().unwrap()).unwrap();
        let b = s.add_box(123.0, 45.0, 67.0).unwrap();
        let report = recompute_ok(&mut s);
        assert_eq!(report.computed, 1, "first session must actually compute");
        assert!(s.volume(b).unwrap() > 0.0);
    }

    // Second session, fresh in-memory state, same parameters.
    {
        let mut s = Session::with_cache(dir.to_str().unwrap()).unwrap();
        let b = s.add_box(123.0, 45.0, 67.0).unwrap();
        let report = recompute_ok(&mut s);

        assert_eq!(
            report.cached, 1,
            "the second session must be served from the DDC, not recompute"
        );
        assert_eq!(report.computed, 0);

        // And the geometry must be genuinely usable, not merely present.
        assert!(s.is_valid_shape(b).unwrap());
        assert_eq!(s.face_count(b).unwrap(), 6);
        let volume = s.volume(b).unwrap();
        assert!((volume - 123.0 * 45.0 * 67.0).abs() < 1e-6);
    }
}

/// A cached shape must arrive WITH its element map. A shape without names is worse than a
/// cache miss: every downstream reference into it would fail to resolve, so the cache would
/// silently break the guarantee M1 exists to provide.
#[test]
fn a_cached_shape_keeps_its_element_names() {
    let dir = scratch("ddc-names");

    let edge_name = {
        let mut s = Session::with_cache(dir.to_str().unwrap()).unwrap();
        let b = s.add_box(100.0, 60.0, 40.0).unwrap();
        recompute_ok(&mut s);
        s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin)
    };
    assert!(!edge_name.is_empty());

    let mut s = Session::with_cache(dir.to_str().unwrap()).unwrap();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    let report = recompute_ok(&mut s);
    assert_eq!(report.cached, 1, "expected a DDC hit");

    // The name resolves against the deserialised shape…
    assert_eq!(
        s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin),
        edge_name,
        "the element map must survive the cache round trip"
    );

    // …and is genuinely usable: a feature built on it must succeed.
    let f = s.add_fillet(b, &edge_name, 5.0).unwrap();
    recompute_ok(&mut s);
    assert!(s.is_valid_shape(f).unwrap());
    assert!(s.volume(f).unwrap() < s.volume(b).unwrap());
}

/// A damaged blob must degrade to a miss, never to a failure. A cache is an optimisation;
/// it must not be able to break a build.
#[test]
fn a_corrupt_cache_blob_becomes_a_miss_not_an_error() {
    let dir = scratch("ddc-corrupt");

    {
        let mut s = Session::with_cache(dir.to_str().unwrap()).unwrap();
        s.add_box(77.0, 33.0, 11.0).unwrap();
        recompute_ok(&mut s);
    }

    // Overwrite every stored blob with garbage.
    let mut damaged = 0usize;
    for entry in walk(&dir) {
        if entry.is_file() {
            // Blobs are stored read-only, deliberately. Undo that to simulate damage.
            let mut perms = std::fs::metadata(&entry).unwrap().permissions();
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                perms.set_mode(0o644);
            }
            #[cfg(not(unix))]
            #[allow(clippy::permissions_set_readonly_false)]
            perms.set_readonly(false);
            let _ = std::fs::set_permissions(&entry, perms);
            if std::fs::write(&entry, b"this is not a shape").is_ok() {
                damaged += 1;
            }
        }
    }
    assert!(damaged > 0, "expected the DDC to have written something to damage");

    let mut s = Session::with_cache(dir.to_str().unwrap()).unwrap();
    let b = s.add_box(77.0, 33.0, 11.0).unwrap();
    let report = s.recompute().expect("a corrupt cache must not fail the recompute");

    assert!(report.all_succeeded(), "corruption must degrade to a miss: {report:?}");
    assert_eq!(report.computed, 1, "the damaged blob must be recomputed, not trusted");
    assert!(s.is_valid_shape(b).unwrap());
}

fn walk(dir: &std::path::Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(entries) = std::fs::read_dir(dir) {
        for e in entries.flatten() {
            let p = e.path();
            if p.is_dir() {
                out.extend(walk(&p));
            } else {
                out.push(p);
            }
        }
    }
    out
}
