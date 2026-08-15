//! Tests for the DXF reader.
//!
//! These are the parser's own tests, checking properties of the bytes-to-entities step in
//! isolation. They are not the acceptance gate — that is the seventeen C++ DXF tests, which run
//! the whole import against real files and are the only thing that can prove this parser agrees
//! with the one it replaces. These exist because a failure here says *which* record went wrong,
//! and a failure there says only that a sketch came out different.

use super::dxf::*;

/// Builds DXF text from (code, value) pairs, so a test reads as the records it is about.
fn dxf(pairs: &[(&str, &str)]) -> Vec<u8> {
    let mut text = String::new();
    for (code, value) in pairs {
        text.push_str(code);
        text.push('\n');
        text.push_str(value);
        text.push('\n');
    }
    text.into_bytes()
}

/// The preamble every entity test needs: into ENTITIES, where geometry is legal.
fn entities(body: &[(&str, &str)]) -> Vec<u8> {
    let mut pairs = vec![("0", "SECTION"), ("2", "ENTITIES")];
    pairs.extend_from_slice(body);
    pairs.push(("0", "ENDSEC"));
    pairs.push(("0", "EOF"));
    dxf(&pairs)
}

#[test]
fn a_line_carries_both_endpoints_and_its_layer() {
    let doc = parse(&entities(&[
        ("0", "LINE"),
        ("8", "outline"),
        ("10", "1.0"),
        ("20", "2.0"),
        ("11", "3.0"),
        ("21", "4.0"),
    ]))
    .expect("well-formed");
    assert_eq!(doc.entities.len(), 1);
    let line = &doc.entities[0];
    assert_eq!(line.kind, EntityKind::Line);
    assert_eq!(line.layer, "outline");
    assert_eq!(line.coords, vec![1.0, 2.0, 3.0, 4.0]);
}

#[test]
fn an_arc_keeps_its_angles_in_degrees() {
    // The conversion to radians lives in C++, next to the test that already pins it. If this
    // parser converted too, the angles would be scaled twice and every arc would land somewhere
    // plausible but wrong — the exact failure the C++ comment warns about.
    let doc = parse(&entities(&[
        ("0", "ARC"),
        ("10", "0"),
        ("20", "0"),
        ("40", "5"),
        ("50", "90"),
        ("51", "180"),
    ]))
    .expect("well-formed");
    let arc = &doc.entities[0];
    assert_eq!(arc.kind, EntityKind::Arc);
    assert_eq!(arc.start_angle, 90.0);
    assert_eq!(arc.end_angle, 180.0);
}

#[test]
fn an_entity_missing_a_required_field_is_dropped_and_counted() {
    // Not a parse error. One bad entity in a file must not cost the user the other thousand, but
    // it also must not vanish silently — the count is what lets the import report say geometry was
    // lost.
    let doc = parse(&entities(&[
        ("0", "LINE"),
        ("10", "1.0"),
        ("20", "2.0"),
        // no 11/21
        ("0", "CIRCLE"),
        ("10", "0"),
        ("20", "0"),
        ("40", "3"),
    ]))
    .expect("a malformed entity is not a malformed file");
    assert_eq!(doc.malformed, 1);
    assert_eq!(doc.entities.len(), 1, "the good entity still arrives");
    assert_eq!(doc.entities[0].kind, EntityKind::Circle);
}

#[test]
fn non_finite_coordinates_are_refused() {
    // Rust's float parser accepts "inf" and "NaN" from text, which is correct for Rust and wrong
    // for us: a NaN coordinate propagates into the kernel and fails a long way from here, where
    // nothing points back at the file that caused it.
    for poison in ["inf", "-inf", "NaN", "1e400"] {
        let doc = parse(&entities(&[
            ("0", "LINE"),
            ("10", poison),
            ("20", "0"),
            ("11", "1"),
            ("21", "1"),
        ]))
        .expect("still a valid file, just a bad number");
        assert_eq!(
            doc.entities.len(),
            0,
            "{poison} must not become a coordinate"
        );
        assert_eq!(doc.malformed, 1);
    }
}

#[test]
fn an_lwpolyline_collects_its_repeated_vertices() {
    let doc = parse(&entities(&[
        ("0", "LWPOLYLINE"),
        ("70", "1"),
        ("10", "0"),
        ("20", "0"),
        ("10", "10"),
        ("20", "0"),
        ("10", "10"),
        ("20", "10"),
    ]))
    .expect("well-formed");
    let poly = &doc.entities[0];
    assert_eq!(poly.kind, EntityKind::Polyline);
    assert_eq!(poly.coords, vec![0.0, 0.0, 10.0, 0.0, 10.0, 10.0]);
    assert_eq!(
        poly.flags & 1,
        1,
        "flag 1 is closed, and the caller needs it"
    );
}

#[test]
fn bulges_are_reported_only_when_some_segment_actually_curves() {
    // A polyline of straight segments must report no bulges at all, not a run of zeros: the
    // import report counts flattened curved segments, and zeros would make every straight
    // polyline claim it lost curvature.
    let straight = parse(&entities(&[
        ("0", "LWPOLYLINE"),
        ("10", "0"),
        ("20", "0"),
        ("42", "0"),
        ("10", "1"),
        ("20", "0"),
    ]))
    .expect("well-formed");
    assert!(straight.entities[0].bulges.is_empty());

    let curved = parse(&entities(&[
        ("0", "LWPOLYLINE"),
        ("10", "0"),
        ("20", "0"),
        ("42", "0.5"),
        ("10", "1"),
        ("20", "0"),
    ]))
    .expect("well-formed");
    assert_eq!(curved.entities[0].bulges.len(), 2);
    assert_eq!(curved.entities[0].bulges[0], 0.5);
}

#[test]
fn a_heavyweight_polyline_gathers_the_vertices_that_follow_it() {
    let doc = parse(&entities(&[
        ("0", "POLYLINE"),
        ("8", "profile"),
        ("70", "1"),
        ("0", "VERTEX"),
        ("10", "0"),
        ("20", "0"),
        ("0", "VERTEX"),
        ("10", "5"),
        ("20", "0"),
        ("0", "VERTEX"),
        ("10", "5"),
        ("20", "5"),
        ("0", "SEQEND"),
    ]))
    .expect("well-formed");
    assert_eq!(
        doc.entities.len(),
        1,
        "the VERTEX entities are not separate geometry"
    );
    let poly = &doc.entities[0];
    assert_eq!(poly.kind, EntityKind::Polyline);
    assert_eq!(poly.layer, "profile");
    assert_eq!(poly.coords.len(), 6);
}

#[test]
fn a_polyline_never_closed_by_seqend_still_emits() {
    // Files written by broken exporters end the ENTITIES section with the polyline still open.
    // Dropping it would lose the whole profile over a missing terminator.
    let doc = parse(&entities(&[
        ("0", "POLYLINE"),
        ("0", "VERTEX"),
        ("10", "0"),
        ("20", "0"),
        ("0", "VERTEX"),
        ("10", "1"),
        ("20", "1"),
    ]))
    .expect("well-formed enough");
    assert_eq!(doc.entities.len(), 1);
    assert_eq!(doc.entities[0].coords, vec![0.0, 0.0, 1.0, 1.0]);
}

#[test]
fn unsupported_entities_are_counted_by_name() {
    let doc = parse(&entities(&[
        ("0", "SPLINE"),
        ("0", "SPLINE"),
        ("0", "INSERT"),
    ]))
    .expect("well-formed");
    assert_eq!(doc.entities.len(), 0);
    assert_eq!(doc.unsupported.get("SPLINE"), Some(&2));
    assert_eq!(doc.unsupported.get("INSERT"), Some(&1));
}

#[test]
fn an_inserts_block_name_is_not_mistaken_for_a_section_name() {
    // Group code 2 is a section name after `0 SECTION` and something else everywhere else — an
    // INSERT's block name here, an ATTRIB's tag elsewhere. Reading those as a section name turns
    // ENTITIES off part-way through, so everything after the first INSERT disappears from a file
    // that opens correctly in every other program. Silent, total, and only visible as "my import
    // is missing most of the drawing".
    let doc = parse(&entities(&[
        ("0", "INSERT"),
        ("2", "TITLEBLOCK"),
        ("10", "0"),
        ("20", "0"),
        ("0", "LINE"),
        ("10", "1"),
        ("20", "2"),
        ("11", "3"),
        ("21", "4"),
    ]))
    .expect("well-formed");
    assert_eq!(
        doc.entities.len(),
        1,
        "the LINE after the INSERT must survive"
    );
    assert_eq!(doc.unsupported.get("INSERT"), Some(&1));
}

#[test]
fn geometry_outside_the_entities_section_is_ignored() {
    // A LINE inside a BLOCK definition is a template, placed by INSERT with a transform. Importing
    // it as if it were drawn at the origin puts geometry in the sketch that is not in the drawing.
    let doc = parse(&dxf(&[
        ("0", "SECTION"),
        ("2", "BLOCKS"),
        ("0", "LINE"),
        ("10", "0"),
        ("20", "0"),
        ("11", "1"),
        ("21", "1"),
        ("0", "ENDSEC"),
        ("0", "EOF"),
    ]))
    .expect("well-formed");
    assert_eq!(doc.entities.len(), 0);
}

#[test]
fn a_file_that_is_not_ascii_dxf_is_refused_immediately() {
    // Binary DXF starts with "AutoCAD Binary DXF\r\n\x1a\0", which is not a group code. The
    // structural check catches it, and equally catches a DWG or a JPEG with the wrong extension,
    // without a table of magic numbers to keep current.
    let binary = b"AutoCAD Binary DXF\r\n\x1a\0\x00\x01\x02";
    assert_eq!(parse(binary).unwrap_err(), Error::NotDxf);
    assert_eq!(parse(b"\x89PNG\r\n\x1a\n").unwrap_err(), Error::NotDxf);
}

#[test]
fn a_group_code_with_no_value_is_truncation_not_a_clean_end() {
    // The shape that segfaulted the C parser. Ending quietly here would mean the two parsers
    // disagree about whether a truncated file imported successfully.
    let mut bytes = entities(&[("0", "LINE"), ("10", "1")]);
    bytes.extend_from_slice(b"20\n");
    assert_eq!(parse(&bytes).unwrap_err(), Error::Truncated);
}

#[test]
fn an_over_long_record_is_refused_before_it_is_allocated() {
    let mut bytes = b"0\n".to_vec();
    bytes.extend(std::iter::repeat(b'A').take(5000));
    bytes.push(b'\n');
    assert_eq!(parse(&bytes).unwrap_err(), Error::RecordTooLong);
}

#[test]
fn an_empty_file_is_an_empty_drawing_not_an_error() {
    let doc = parse(b"").expect("nothing to read is not a failure");
    assert_eq!(doc.entities.len(), 0);
}

#[test]
fn a_utf8_bom_does_not_make_the_first_group_code_unreadable() {
    let mut bytes = vec![0xEF, 0xBB, 0xBF];
    bytes.extend_from_slice(&entities(&[("0", "POINT"), ("10", "1"), ("20", "2")]));
    let doc = parse(&bytes).expect("a BOM is not corruption");
    assert_eq!(doc.entities.len(), 1);
}

#[test]
fn crlf_line_endings_and_leading_whitespace_are_tolerated() {
    // DXF is written by Windows tools and by exporters that pad group codes to a fixed width.
    // Both are ubiquitous and neither is corruption.
    let text = "  0\r\nSECTION\r\n  2\r\nENTITIES\r\n  0\r\nPOINT\r\n 10\r\n7.5\r\n 20\r\n8.5\r\n\
                  0\r\nENDSEC\r\n  0\r\nEOF\r\n";
    let doc = parse(text.as_bytes()).expect("well-formed");
    assert_eq!(doc.entities[0].coords, vec![7.5, 8.5]);
}

#[test]
fn non_utf8_text_in_a_layer_name_does_not_reject_the_file() {
    // Layer names in real files are often CP1252. Requiring UTF-8 would refuse a file AutoCAD
    // opens; the name is only ever shown or matched, so a lossy conversion is the right trade.
    let mut bytes = b"0\nSECTION\n2\nENTITIES\n0\nPOINT\n8\n".to_vec();
    bytes.extend_from_slice(&[0xC0, 0xE9]); // Latin-1 "Àé", invalid UTF-8
    bytes.extend_from_slice(b"\n10\n1\n20\n2\n0\nENDSEC\n0\nEOF\n");
    let doc = parse(&bytes).expect("a non-UTF-8 layer name is not corruption");
    assert_eq!(doc.entities.len(), 1);
    assert!(!doc.entities[0].layer.is_empty());
}

#[test]
fn no_input_of_any_shape_panics() {
    // A cheap structural sweep, not a substitute for the mutation fuzzer in dxf_fuzz.rs. Its value
    // is that it runs on every `cargo test` rather than only when the fuzz suite is invoked.
    let seed = entities(&[
        ("0", "LINE"),
        ("10", "1"),
        ("20", "2"),
        ("11", "3"),
        ("21", "4"),
        ("0", "LWPOLYLINE"),
        ("10", "0"),
        ("20", "0"),
        ("42", "0.5"),
        ("10", "1"),
        ("20", "1"),
    ]);
    let mut state = 0x243F_6A88_85A3_08D3u64;
    let mut next = || {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state
    };
    for _ in 0..2000 {
        let mut bytes = seed.clone();
        for _ in 0..(next() % 8 + 1) {
            if bytes.is_empty() {
                break;
            }
            match next() % 3 {
                0 => {
                    let at = (next() as usize) % bytes.len();
                    bytes[at] = (next() % 256) as u8;
                }
                1 => {
                    let at = (next() as usize) % bytes.len();
                    bytes.truncate(at);
                }
                _ => {
                    let at = (next() as usize) % bytes.len();
                    bytes.insert(at, (next() % 256) as u8);
                }
            }
        }
        // The contract is "returns", not "succeeds". Either arm is fine; a panic is not.
        if let Ok(doc) = parse(&bytes) {
            for e in &doc.entities {
                assert!(
                    e.coords.iter().all(|v| v.is_finite()),
                    "a surviving entity must never carry a non-finite coordinate"
                );
                assert!(e.radius.is_finite() && e.start_angle.is_finite());
            }
        }
    }
}
