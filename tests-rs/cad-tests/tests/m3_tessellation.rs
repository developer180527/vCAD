//! M3.1 ACCEPTANCE — tessellation and the mesh cache.
//!
//! The assertions that matter here are structural and about caching, not about pixels. Two in
//! particular decide whether the design works at assembly scale:
//!
//!   * every mesh element slot resolves to a real ElementName — otherwise a GPU pick returns a
//!     triangle number and the whole naming layer is wasted at the point of use;
//!   * N identical parts tessellate ONCE. Content-addressed dedupe is what makes 100k parts
//!     tractable, and if it ever breaks, nothing else about the renderer can compensate.

use cad_tests::*;
use std::path::PathBuf;

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("cad-m3-{}-{}", name, std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// A box tessellates to something structurally sound: 12 triangles, 6 faces' worth of
/// elements plus 12 edges, and bounds that match the box.
#[test]
fn a_box_tessellates_to_a_sound_mesh() {
    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);

    let mesh = s.tessellate(b, 0.05, 0.35).unwrap();

    // Two triangles per planar quad face.
    assert_eq!(mesh.triangles, 12, "a box is 12 triangles");
    // 6 faces + 12 edges = 18 named element slots.
    assert_eq!(
        mesh.elements, 18,
        "every face and edge gets an element slot"
    );
    assert_eq!(mesh.edge_polylines, 12, "a box has 12 edges");

    // Faces do not share vertices — that is what keeps edges crisp — so 6 quads at 4 corners.
    assert_eq!(
        mesh.vertices, 24,
        "per-face vertex blocks, never shared across a face"
    );

    for i in 0..3 {
        assert!(
            mesh.bounds_min[i] <= mesh.bounds_max[i],
            "bounds must be ordered"
        );
    }
    assert!((mesh.bounds_max[0] - mesh.bounds_min[0] - 100.0).abs() < 1e-3);
    assert!((mesh.bounds_max[1] - mesh.bounds_min[1] - 60.0).abs() < 1e-3);
    assert!((mesh.bounds_max[2] - mesh.bounds_min[2] - 40.0).abs() < 1e-3);
}

/// Every element slot must resolve to a parseable ElementName. This is the assertion that
/// makes GPU ID-buffer picking worth building: a pick returns a slot, and a slot has to become
/// a stable geometric reference rather than a triangle index.
#[test]
fn every_mesh_element_resolves_to_a_name() {
    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let mesh = s.tessellate(b, 0.05, 0.35).unwrap();

    for slot in 0..mesh.elements as u32 {
        let name = s.mesh_element_name(b, slot);
        assert!(!name.is_empty(), "element slot {slot} has no name");
    }

    // And out of range is empty rather than a crash or a garbage read.
    assert!(s
        .mesh_element_name(b, mesh.elements as u32 + 1000)
        .is_empty());
}

/// A curved surface must respect the ANGULAR tolerance, not just the chord one. Tuning
/// deflection alone leaves cylinders visibly faceted at every zoom level, which is the single
/// most common way a CAD viewport looks cheap.
#[test]
fn angular_tolerance_controls_curve_fidelity() {
    let mut s = session();
    let c = s.add("Cylinder").unwrap();
    s.set_length(c, "radius", 25.0).unwrap();
    s.set_length(c, "height", 80.0).unwrap();
    recompute_ok(&mut s);

    let coarse = s.tessellate(c, 0.05, 1.0).unwrap();
    let fine = s.tessellate(c, 0.05, 0.05).unwrap();

    assert!(
        fine.triangles > coarse.triangles * 2,
        "a tighter angular tolerance must add substantially more triangles: \
         {} vs {}",
        fine.triangles,
        coarse.triangles
    );
    // The edge polylines must get finer too — they are sampled from the exact curve, so a
    // cylinder's silhouette should not inherit the surface mesh's faceting.
    assert!(fine.edge_points > coarse.edge_points);
}

/// Tessellation is deterministic. Non-determinism here would poison the mesh cache across
/// machines exactly as it would the feature cache.
#[test]
fn tessellation_is_deterministic() {
    let mut a = session();
    let ba = a.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut a);

    let mut b = session();
    let bb = b.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut b);

    let ma = a.tessellate(ba, 0.05, 0.35).unwrap();
    let mb = b.tessellate(bb, 0.05, 0.35).unwrap();

    assert_eq!(ma.triangles, mb.triangles);
    assert_eq!(ma.vertices, mb.vertices);
    assert_eq!(ma.elements, mb.elements);
    assert_eq!(ma.edge_points, mb.edge_points);
}

/// THE scale assertion. N identical parts must tessellate once.
///
/// Content-addressed dedupe is not an optimisation pass here, it is what makes 100k parts
/// possible at all: a large assembly is overwhelmingly repeated fasteners, so ~1000 unique
/// shapes stand in for 100k parts. If this ever regresses, no amount of GPU work recovers it.
#[test]
fn identical_parts_tessellate_once() {
    let mut s = session();

    let mut parts = Vec::new();
    for _ in 0..40 {
        parts.push(s.add_box(20.0, 20.0, 20.0).unwrap());
    }
    recompute_ok(&mut s);

    s.reset_mesh_cache_stats().ok();
    for p in &parts {
        s.tessellate(*p, 0.05, 0.35).unwrap();
    }

    let stats = s.mesh_cache_stats().unwrap();
    assert_eq!(
        stats.misses, 1,
        "40 identical boxes must tessellate exactly once, got {} misses",
        stats.misses
    );
    assert_eq!(stats.hits, 39, "the other 39 must be cache hits");
}

/// A different shape must NOT be deduped with an identical-looking one of another size.
#[test]
fn different_geometry_is_not_deduped() {
    let mut s = session();
    let a = s.add_box(20.0, 20.0, 20.0).unwrap();
    let b = s.add_box(20.0, 20.0, 20.1).unwrap();
    recompute_ok(&mut s);

    s.tessellate(a, 0.05, 0.35).unwrap();
    let before = s.mesh_cache_stats().unwrap();
    s.tessellate(b, 0.05, 0.35).unwrap();
    let after = s.mesh_cache_stats().unwrap();

    assert_eq!(
        after.misses,
        before.misses + 1,
        "a 0.1mm difference is still a different shape"
    );
}

/// Changing tolerances must miss — the settings are part of the key by construction, so
/// serving a mesh at the wrong fidelity should be impossible.
#[test]
fn changing_tolerance_misses_the_cache() {
    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);

    s.tessellate(b, 0.05, 0.35).unwrap();
    let before = s.mesh_cache_stats().unwrap();
    s.tessellate(b, 0.01, 0.35).unwrap();
    let after = s.mesh_cache_stats().unwrap();

    assert_eq!(
        after.misses,
        before.misses + 1,
        "a finer deflection is a different mesh"
    );
}

/// The DDC tier: a mesh tessellated in one session is served to a NEW session from disk. This
/// is what makes opening an assembly a colleague already opened fast — and it is the same
/// shared-tier mechanism the feature cache uses.
#[test]
fn the_ddc_serves_a_mesh_across_sessions() {
    let dir = scratch("mesh-ddc");

    let first_triangles = {
        let mut s = Session::with_cache(dir.to_str().unwrap()).unwrap();
        let b = s.add_box(123.0, 45.0, 67.0).unwrap();
        recompute_ok(&mut s);
        let m = s.tessellate(b, 0.05, 0.35).unwrap();
        assert_eq!(s.mesh_cache_stats().unwrap().misses, 1);
        m.triangles
    };

    let mut s = Session::with_cache(dir.to_str().unwrap()).unwrap();
    let b = s.add_box(123.0, 45.0, 67.0).unwrap();
    recompute_ok(&mut s);
    let m = s.tessellate(b, 0.05, 0.35).unwrap();

    let stats = s.mesh_cache_stats().unwrap();
    assert_eq!(
        stats.hits, 1,
        "the second session must be served from the DDC"
    );
    assert_eq!(stats.misses, 0);

    // And the deserialised mesh must be usable, not merely present.
    assert_eq!(m.triangles, first_triangles);
    assert!(m.elements > 0);
    for slot in 0..m.elements as u32 {
        assert!(
            !s.mesh_element_name(b, slot).is_empty(),
            "element names must survive the cache round trip — a mesh without them makes \
             every pick unresolvable"
        );
    }
}

/// A filleted part still tessellates soundly, with more triangles than the unfilleted one and
/// every element still named. Fillets are where naming and tessellation meet.
#[test]
fn a_filleted_part_tessellates_with_named_elements() {
    let mut s = session();
    let b = s.add_box(100.0, 60.0, 40.0).unwrap();
    recompute_ok(&mut s);
    let plain = s.tessellate(b, 0.05, 0.35).unwrap();

    let edge = s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin);
    let f = s.add_fillet(b, &edge, 5.0).unwrap();
    recompute_ok(&mut s);

    let filleted = s.tessellate(f, 0.05, 0.35).unwrap();
    assert!(
        filleted.triangles > plain.triangles,
        "a fillet adds a curved face, so it must add triangles"
    );
    assert!(filleted.elements > plain.elements);
    for slot in 0..filleted.elements as u32 {
        assert!(!s.mesh_element_name(f, slot).is_empty());
    }
}
