//! An ASCII DXF reader.
//!
//! # What this owns, and what it deliberately does not
//!
//! This turns **bytes we did not write** into a checked list of entities. That is all. Projection
//! onto a sketch plane, unit scaling, construction-layer matching, degenerate-geometry rules and
//! the import report stay in C++, where the sketch types live.
//!
//! The split is not arbitrary. The argument for Rust here is that parsing hostile input in C is
//! how importers become the CVE source they are — and that argument covers the bytes and nothing
//! past them. Moving the domain logic across too would mean rewriting code that the existing DXF
//! acceptance tests already pin, which would destroy the only oracle available for the swap: those
//! tests pass against dime today, so they can only judge the new parser if everything downstream
//! of it is unchanged.
//!
//! # Structure of the format, which is why the parser is this shape
//!
//! ASCII DXF is a flat stream of two-line records: a numeric **group code**, then its value.
//! Meaning comes entirely from context — code 10 is "x coordinate of whatever entity we are
//! inside", and an entity ends when the next `0` arrives. There is no nesting and no length
//! prefix, so the parser is a state machine over pairs, and every field is optional as far as the
//! bytes are concerned.
//!
//! That last point drives the error handling: a missing field is not a parse error, it is a
//! malformed entity, and the right response is to drop that entity and keep going rather than to
//! reject the file. A CAD user with one bad entity in a thousand wants the nine hundred and
//! ninety-nine.
//!
//! # Limits are part of the contract
//!
//! Every bound here exists because fuzzing found the C parser without it. They are checked while
//! reading rather than after, so a hostile file is refused before it has cost anything.

use std::collections::BTreeMap;

/// Longest single record. DXF values are short — a coordinate is tens of characters and the
/// longest legitimate value is a layer or block name.
///
/// The C parser's equivalent limit is where its one-past-the-end write lived; here the bound is
/// not preventing a memory error (there is none to prevent) but a memory *exhaustion* one, and
/// refusing early is what keeps a 4 GB line from becoming a 4 GB allocation.
const MAX_RECORD_BYTES: usize = 4096;

/// Most entities in one file. Well past any real drawing — a dense architectural plan is tens of
/// thousands — and short of what an attacker needs to exhaust memory.
const MAX_ENTITIES: usize = 5_000_000;

/// Most vertices in one polyline.
const MAX_VERTICES: usize = 1_000_000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EntityKind {
    Line,
    Circle,
    Arc,
    Point,
    /// Both LWPOLYLINE and the older heavyweight POLYLINE, which differ in how they store their
    /// vertices and in nothing the caller cares about.
    Polyline,
}

/// One entity, with its numbers already validated as finite.
#[derive(Debug, Clone)]
pub struct Entity {
    pub kind: EntityKind,
    pub layer: String,
    /// Interleaved x,y pairs. Two for a line, one for a circle/arc/point centre, n for a polyline.
    pub coords: Vec<f64>,
    /// Circle and arc radius; 0 otherwise.
    pub radius: f64,
    /// Arc start and end, in DEGREES as the file stores them. Converted by the caller, because the
    /// caller is where the existing conversion and its test already live.
    pub start_angle: f64,
    pub end_angle: f64,
    /// DXF flags. Bit 0 on a polyline means closed.
    pub flags: i64,
    /// Per-segment bulge values for a polyline, empty when it has none. A file with no curved
    /// segments stores no bulges at all rather than an array of zeros.
    pub bulges: Vec<f64>,
}

/// What one file contained.
#[derive(Debug, Default)]
pub struct Document {
    pub entities: Vec<Entity>,
    /// Entity types present that this parser does not produce, with counts. Named rather than
    /// summarised so a caller can say "SPLINE x4" and let the user judge whether it mattered.
    pub unsupported: BTreeMap<String, usize>,
    /// Entities dropped because a required field was missing or non-finite. Counted rather than
    /// silent: "we read your file and threw part of it away" is something a user must be told.
    pub malformed: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// Not ASCII DXF at all — binary DXF, DWG renamed, or arbitrary bytes.
    NotDxf,
    /// A record longer than [`MAX_RECORD_BYTES`].
    RecordTooLong,
    /// More entities or vertices than the limits allow.
    TooLarge,
    /// The file ends part-way through a record.
    Truncated,
}

impl Error {
    /// The user-facing sentence. Kept here rather than at the C boundary so the wording lives with
    /// the condition that produces it.
    pub fn message(&self) -> &'static str {
        match self {
            Error::NotDxf => {
                "That file is not an ASCII DXF. Binary DXF and DWG are not supported — convert it \
                 to ASCII DXF first."
            }
            Error::RecordTooLong => {
                "That DXF file is corrupt — it contains a record too long to be valid."
            }
            Error::TooLarge => "That DXF file declares more geometry than can be imported.",
            Error::Truncated => {
                "That DXF file is incomplete — it ends part-way through a record. It may have been \
                 truncated in transfer."
            }
        }
    }
}

/// A (group code, value) pair reader over the raw bytes.
///
/// Bytes rather than `str`, deliberately. Real DXF in circulation is frequently CP1252 or another
/// single-byte encoding in its text fields, and requiring valid UTF-8 would reject files that
/// AutoCAD opens happily. Only layer names and entity types are ever interpreted as text, and both
/// are converted lossily at the point of use.
struct Records<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Records<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        // A UTF-8 BOM is common on files written by Windows tools and is not part of the first
        // group code. Skipping it here rather than failing is the difference between opening a
        // colleague's file and telling them it is corrupt.
        let bytes = bytes.strip_prefix(&[0xEF, 0xBB, 0xBF]).unwrap_or(bytes);
        Records { bytes, at: 0 }
    }

    /// The next line, trimmed of its terminator and surrounding whitespace.
    fn line(&mut self) -> Result<Option<&'a [u8]>, Error> {
        if self.at >= self.bytes.len() {
            return Ok(None);
        }
        let start = self.at;
        let end = match self.bytes[start..].iter().position(|&b| b == b'\n') {
            Some(offset) => start + offset,
            None => self.bytes.len(),
        };
        if end - start > MAX_RECORD_BYTES {
            return Err(Error::RecordTooLong);
        }
        self.at = if end < self.bytes.len() { end + 1 } else { end };

        let mut slice = &self.bytes[start..end];
        while let Some((&last, rest)) = slice.split_last() {
            if last == b'\r' || last == b' ' || last == b'\t' {
                slice = rest;
            } else {
                break;
            }
        }
        while let Some((&first, rest)) = slice.split_first() {
            if first == b' ' || first == b'\t' {
                slice = rest;
            } else {
                break;
            }
        }
        Ok(Some(slice))
    }

    /// The next (code, value) pair. `None` at a clean end of file.
    ///
    /// A code with no value is [`Error::Truncated`] rather than a silent stop: it is precisely the
    /// shape that segfaulted the C parser, and treating it as a normal ending would mean the two
    /// parsers disagree about whether a truncated file is acceptable.
    fn next(&mut self) -> Result<Option<(i64, &'a [u8])>, Error> {
        loop {
            let Some(code_line) = self.line()? else {
                return Ok(None);
            };
            if code_line.is_empty() {
                continue;
            }
            let text = std::str::from_utf8(code_line).map_err(|_| Error::NotDxf)?;
            let code: i64 = text.trim().parse().map_err(|_| Error::NotDxf)?;
            let Some(value) = self.line()? else {
                return Err(Error::Truncated);
            };
            return Ok(Some((code, value)));
        }
    }
}

fn text_of(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

/// Parses a value that must be a finite number. `None` for anything else, including the infinities
/// and NaNs that Rust's own float parser accepts from the strings "inf" and "nan".
fn number(bytes: &[u8]) -> Option<f64> {
    let text = std::str::from_utf8(bytes).ok()?.trim();
    let value: f64 = text.parse().ok()?;
    value.is_finite().then_some(value)
}

fn integer(bytes: &[u8]) -> Option<i64> {
    let text = std::str::from_utf8(bytes).ok()?.trim();
    // Through f64 as well as i64: DXF writers emit "1.0" where an integer is expected often enough
    // that refusing it costs real files for no benefit.
    text.parse::<i64>()
        .ok()
        .or_else(|| number(bytes).map(|v| v as i64))
}

/// Fields accumulated for the entity currently being read.
#[derive(Default)]
struct Pending {
    kind: String,
    layer: String,
    /// Group code -> value, for the single-valued codes.
    scalars: BTreeMap<i64, f64>,
    flags: i64,
    /// LWPOLYLINE vertices arrive as repeated 10/20 pairs.
    xs: Vec<f64>,
    ys: Vec<f64>,
    bulges: Vec<f64>,
    /// Whether any bulge was non-zero, so a polyline with only zeros reports none.
    any_bulge: bool,
}

/// Parses ASCII DXF bytes into entities.
pub fn parse(bytes: &[u8]) -> Result<Document, Error> {
    let mut records = Records::new(bytes);
    let mut document = Document::default();

    // The first record must be a group code. ASCII DXF is group-code records from its first byte —
    // even a comment is code 999 — so anything else is a different format wearing a .dxf name.
    // Checked structurally rather than by sniffing for a binary header, because the header is the
    // symptom and this is the property.
    let first = records.next()?;
    let Some((code, value)) = first else {
        return Ok(document);
    };

    let mut in_entities = false;
    // Set only by `0 SECTION`, cleared by the next record of any kind. See the code-2 arm.
    let mut expecting_section_name = false;
    let mut pending: Option<Pending> = None;
    // The heavyweight POLYLINE owns following VERTEX entities; its vertices are not its own fields.
    let mut open_polyline: Option<Pending> = None;

    let mut record = Some((code, value));
    while let Some((code, value)) = record {
        // A section name is only the record immediately after `0 SECTION`. Reset before the match
        // and set again inside it, so the window is exactly one record wide by construction rather
        // than by every arm remembering to clear it.
        let expecting_name = std::mem::take(&mut expecting_section_name);

        match code {
            0 => {
                let kind = text_of(value).to_ascii_uppercase();

                // Finish whatever was being read before starting the next one. Unconditional,
                // including before a VERTEX: `pending` is the entity whose fields are still
                // arriving, which is a different thing from `open_polyline`, the POLYLINE those
                // vertices belong to. `finish` is what routes a completed VERTEX into the open
                // polyline, so skipping it here is what stops vertices accumulating at all.
                if let Some(previous) = pending.take() {
                    finish(previous, &mut document, &mut open_polyline)?;
                }

                expecting_section_name = kind == "SECTION";

                match kind.as_str() {
                    "SECTION" | "ENDSEC" | "EOF" | "TABLE" | "ENDTAB" | "BLOCK" | "ENDBLK" => {
                        // Structure, not geometry. ENDSEC also closes a heavyweight polyline that
                        // a malformed file never terminated with SEQEND.
                        if kind == "ENDSEC" {
                            if let Some(poly) = open_polyline.take() {
                                emit_polyline(poly, &mut document);
                            }
                        }
                    }
                    "SEQEND" => {
                        if let Some(poly) = open_polyline.take() {
                            emit_polyline(poly, &mut document);
                        }
                    }
                    _ => {
                        if in_entities {
                            pending = Some(Pending {
                                kind,
                                ..Default::default()
                            });
                        }
                    }
                }
            }
            2 => {
                // Only a section name when it follows `0 SECTION`. Code 2 is heavily reused — it
                // is an INSERT's block name and an ATTRIB's tag, both of which appear inside
                // ENTITIES. Reading those as a section name switches ENTITIES off part-way
                // through, and every entity after the first INSERT vanishes from a file that
                // opens correctly everywhere else.
                //
                // Only ENTITIES carries geometry we import; BLOCKS carries definitions placed by
                // INSERT, and expanding those means transforms and recursion. Not imported, and
                // counted as unsupported at the INSERT so the user is told rather than handed a
                // silently empty sketch.
                if expecting_name {
                    in_entities = text_of(value).eq_ignore_ascii_case("ENTITIES");
                }
            }
            8 => {
                if let Some(p) = pending.as_mut() {
                    p.layer = text_of(value);
                }
            }
            70 => {
                if let Some(p) = pending.as_mut() {
                    p.flags = integer(value).unwrap_or(0);
                }
            }
            10 | 20 | 11 | 21 | 40 | 50 | 51 | 42 => {
                if let Some(p) = pending.as_mut() {
                    if let Some(v) = number(value) {
                        // A repeated 10 in an LWPOLYLINE is the next vertex, not a correction.
                        if code == 10 && p.kind == "LWPOLYLINE" {
                            if p.xs.len() >= MAX_VERTICES {
                                return Err(Error::TooLarge);
                            }
                            p.xs.push(v);
                        } else if code == 20 && p.kind == "LWPOLYLINE" {
                            p.ys.push(v);
                        } else if code == 42 {
                            // Bulge applies to the segment starting at the last vertex read.
                            while p.bulges.len() + 1 < p.xs.len() {
                                p.bulges.push(0.0);
                            }
                            p.bulges.push(v);
                            if v != 0.0 {
                                p.any_bulge = true;
                            }
                        } else {
                            p.scalars.insert(code, v);
                        }
                    }
                    // A non-finite or unparseable coordinate is simply not recorded. The entity
                    // then fails its required-field check below and is counted malformed, which is
                    // the same outcome by a route that needs no special case.
                }
            }
            _ => {}
        }

        if document.entities.len() > MAX_ENTITIES {
            return Err(Error::TooLarge);
        }
        record = records.next()?;
    }

    if let Some(previous) = pending.take() {
        finish(previous, &mut document, &mut open_polyline)?;
    }
    if let Some(poly) = open_polyline.take() {
        emit_polyline(poly, &mut document);
    }

    Ok(document)
}

/// Turns one completed pending entity into a `Document` entry, or counts it.
fn finish(
    p: Pending,
    document: &mut Document,
    open_polyline: &mut Option<Pending>,
) -> Result<(), Error> {
    let get = |code: i64| p.scalars.get(&code).copied();

    match p.kind.as_str() {
        "LINE" => match (get(10), get(20), get(11), get(21)) {
            (Some(x1), Some(y1), Some(x2), Some(y2)) => document.entities.push(Entity {
                kind: EntityKind::Line,
                layer: p.layer,
                coords: vec![x1, y1, x2, y2],
                radius: 0.0,
                start_angle: 0.0,
                end_angle: 0.0,
                flags: p.flags,
                bulges: Vec::new(),
            }),
            _ => document.malformed += 1,
        },
        "CIRCLE" => match (get(10), get(20), get(40)) {
            (Some(x), Some(y), Some(r)) => document.entities.push(Entity {
                kind: EntityKind::Circle,
                layer: p.layer,
                coords: vec![x, y],
                radius: r,
                start_angle: 0.0,
                end_angle: 0.0,
                flags: p.flags,
                bulges: Vec::new(),
            }),
            _ => document.malformed += 1,
        },
        "ARC" => match (get(10), get(20), get(40), get(50), get(51)) {
            (Some(x), Some(y), Some(r), Some(a0), Some(a1)) => document.entities.push(Entity {
                kind: EntityKind::Arc,
                layer: p.layer,
                coords: vec![x, y],
                radius: r,
                start_angle: a0,
                end_angle: a1,
                flags: p.flags,
                bulges: Vec::new(),
            }),
            _ => document.malformed += 1,
        },
        "POINT" => match (get(10), get(20)) {
            (Some(x), Some(y)) => document.entities.push(Entity {
                kind: EntityKind::Point,
                layer: p.layer,
                coords: vec![x, y],
                radius: 0.0,
                start_angle: 0.0,
                end_angle: 0.0,
                flags: p.flags,
                bulges: Vec::new(),
            }),
            _ => document.malformed += 1,
        },
        "LWPOLYLINE" => emit_polyline(p, document),
        "POLYLINE" => {
            // Opens; its vertices arrive as separate VERTEX entities until SEQEND.
            *open_polyline = Some(p);
        }
        "VERTEX" => {
            if let Some(poly) = open_polyline.as_mut() {
                if let (Some(x), Some(y)) = (get(10), get(20)) {
                    if poly.xs.len() >= MAX_VERTICES {
                        return Err(Error::TooLarge);
                    }
                    poly.xs.push(x);
                    poly.ys.push(y);
                }
            }
            // A VERTEX with no owner is a malformed file, not an entity. Ignored rather than
            // counted: it is a structural stray, and counting it as dropped geometry would
            // over-report the loss.
        }
        other if !other.is_empty() => {
            *document.unsupported.entry(other.to_string()).or_insert(0) += 1;
        }
        _ => {}
    }
    Ok(())
}

/// Emits a polyline, pairing coordinates and padding bulges.
fn emit_polyline(p: Pending, document: &mut Document) {
    // Truncated to the shorter of the two: a file whose x and y counts disagree is malformed, and
    // pairing what is there beats dropping the whole polyline.
    let n = p.xs.len().min(p.ys.len());
    if n < 2 {
        if n > 0 {
            document.malformed += 1;
        }
        return;
    }
    let mut coords = Vec::with_capacity(n * 2);
    for i in 0..n {
        coords.push(p.xs[i]);
        coords.push(p.ys[i]);
    }
    let bulges = if p.any_bulge {
        let mut b = p.bulges;
        b.resize(n, 0.0);
        b
    } else {
        Vec::new()
    };
    document.entities.push(Entity {
        kind: EntityKind::Polyline,
        layer: p.layer,
        coords,
        radius: 0.0,
        start_angle: 0.0,
        end_angle: 0.0,
        flags: p.flags,
        bulges,
    });
}
