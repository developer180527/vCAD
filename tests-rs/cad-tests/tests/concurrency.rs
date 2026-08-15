//! Threads.
//!
//! # What the code actually promises today
//!
//! Measured, not assumed, by reading `abi/src/Session.cpp`:
//!
//!   * **The C ABI is thread-safe per session.** A global mutex guards the session registry, and
//!     every export takes a per-session mutex. Two threads calling into one session are
//!     serialised, not racing.
//!   * **`core/` and `render/` contain no threading at all.** Recompute is a single-threaded walk;
//!     tessellation is single-threaded; `MeshCache` has no mutex. Nothing below the ABI expects to
//!     be entered concurrently, and the per-session lock is what makes that safe.
//!   * **The Rust wrapper is `Send` but deliberately NOT `Sync`**, because the returned-string
//!     contract — valid until the next call on this session — cannot survive concurrent readers.
//!
//! So the tests below split in two. Anything about ONE session shared between threads has to use
//! raw FFI, because that is the level the promise is made at. Anything about independent sessions
//! can use the safe wrapper.
//!
//! # What this does NOT test, and why that matters
//!
//! `PLUGIN_CONTRACT.md` §4.3 tells plugins that "compute may be called on a worker thread". No
//! code anywhere calls compute on a worker thread, so that promise is currently aspirational —
//! and a plugin author who takes it at face value will write locking they do not need, or worse,
//! rely on it and be broken the day it becomes true. These tests pin what is real now, so the day
//! parallel recompute lands, what changes is visible.

use cad::*;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Barrier};

// ── independent sessions ────────────────────────────────────────────────────────────────

/// The same document built on several threads, each in its own session, must come out identical.
///
/// This is the real test for hidden shared state: a static cache, a global counter, a naming
/// serial that depends on process-wide order. Any of those would show up as two threads producing
/// documents that differ, and only under concurrency — which is the hardest kind of bug to
/// reproduce from a bug report.
#[test]
fn independent_sessions_build_identical_documents() {
    const THREADS: usize = 8;
    let barrier = Arc::new(Barrier::new(THREADS));

    let digests: Vec<u64> = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..THREADS)
            .map(|_| {
                let barrier = Arc::clone(&barrier);
                scope.spawn(move || {
                    let mut s = Session::new().expect("session");
                    // Released together, so the threads overlap rather than running in sequence.
                    barrier.wait();

                    let base = s.add_box(40.0, 30.0, 20.0).expect("box");
                    let tool = s.add_box(10.0, 10.0, 60.0).expect("tool");
                    let moved = s.add_translate(tool, 15.0, 10.0, -20.0).expect("translate");
                    let _cut = s.add_cut(base, moved).expect("cut");
                    s.recompute().expect("recompute");
                    s.document_digest().expect("digest")
                })
            })
            .collect();
        handles
            .into_iter()
            .map(|h| h.join().expect("thread"))
            .collect()
    });

    let first = digests[0];
    for (i, d) in digests.iter().enumerate() {
        assert_eq!(
            *d, first,
            "thread {i} built a different document from thread 0 ({d:#x} vs {first:#x}) — \
             something is shared between sessions that should not be"
        );
    }
}

/// Element names must not depend on which thread produced them.
///
/// Stronger than the digest check above and worth its own test: the digest could match while names
/// differed, and names are what downstream features hold across a rebuild. A naming serial derived
/// from anything process-wide would fail here.
#[test]
fn element_names_do_not_depend_on_the_thread() {
    const THREADS: usize = 6;
    let names: Vec<String> = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..THREADS)
            .map(|_| {
                scope.spawn(|| {
                    let mut s = Session::new().expect("session");
                    let b = s.add_box(25.0, 25.0, 25.0).expect("box");
                    s.recompute().expect("recompute");
                    s.box_edge_between(b, BoxFace::ZMax, BoxFace::YMin)
                })
            })
            .collect();
        handles
            .into_iter()
            .map(|h| h.join().expect("thread"))
            .collect()
    });

    assert!(!names[0].is_empty(), "the edge should have a name at all");
    for (i, n) in names.iter().enumerate() {
        assert_eq!(
            n, &names[0],
            "thread {i} named the same edge differently: {n:?} vs {:?}",
            names[0]
        );
    }
}

// ── one session, several threads ────────────────────────────────────────────────────────
//
// Raw FFI, because `Session` is deliberately not `Sync` and this is exactly the promise the C
// layer makes on its own: a per-session mutex serialising every export.

mod raw {
    use std::os::raw::c_char;
    pub type CadSession = u64;
    pub type CadObject = u64;
    pub type CadStatus = i32;

    #[repr(C)]
    #[derive(Default)]
    pub struct RecomputeReport {
        pub computed: u64,
        pub cached: u64,
        pub skipped: u64,
        pub failed: u64,
        pub blocked: u64,
    }

    extern "C" {
        pub fn cad_session_create() -> CadSession;
        pub fn cad_session_release(s: CadSession);
        pub fn cad_object_add(s: CadSession, ty: *const c_char, out: *mut CadObject) -> CadStatus;
        pub fn cad_object_set_length(
            s: CadSession,
            o: CadObject,
            prop: *const c_char,
            mm: f64,
        ) -> CadStatus;
        pub fn cad_object_count(s: CadSession, out: *mut u64) -> CadStatus;
        pub fn cad_recompute(s: CadSession, out: *mut RecomputeReport) -> CadStatus;
    }
}

/// A session handle is a plain integer, and the C side owns the locking. Wrapping it so it can
/// cross a thread boundary is exactly what the ABI's design permits and the Rust wrapper
/// deliberately does not.
#[derive(Clone, Copy)]
struct Shared(raw::CadSession);
unsafe impl Send for Shared {}
unsafe impl Sync for Shared {}

/// Several threads editing ONE session must serialise, not corrupt it.
///
/// The assertion is the object count: every successful `add` must be present at the end. A lost
/// update — two threads reading and writing the document around each other — shows up as a count
/// lower than the number of successful adds, and as nothing else.
#[test]
fn concurrent_edits_to_one_session_are_serialised() {
    const THREADS: usize = 8;
    const PER_THREAD: usize = 25;

    let session = Shared(unsafe { raw::cad_session_create() });
    assert_ne!(session.0, 0, "session must be created");

    let added = Arc::new(AtomicU32::new(0));
    let barrier = Arc::new(Barrier::new(THREADS));

    std::thread::scope(|scope| {
        for _ in 0..THREADS {
            let added = Arc::clone(&added);
            let barrier = Arc::clone(&barrier);
            scope.spawn(move || {
                let ty = std::ffi::CString::new("Box").unwrap();
                let dx = std::ffi::CString::new("dx").unwrap();
                barrier.wait();
                for i in 0..PER_THREAD {
                    let mut o: raw::CadObject = 0;
                    let status = unsafe { raw::cad_object_add(session.0, ty.as_ptr(), &mut o) };
                    if status == 0 {
                        added.fetch_add(1, Ordering::SeqCst);
                        unsafe {
                            raw::cad_object_set_length(session.0, o, dx.as_ptr(), 10.0 + i as f64);
                        }
                    }
                    if i % 5 == 0 {
                        let mut report = raw::RecomputeReport::default();
                        unsafe { raw::cad_recompute(session.0, &mut report) };
                    }
                }
            });
        }
    });

    let mut count = 0u64;
    unsafe { raw::cad_object_count(session.0, &mut count) };
    assert_eq!(
        count,
        added.load(Ordering::SeqCst) as u64,
        "the document holds {count} objects but {} adds succeeded — an update was lost",
        added.load(Ordering::SeqCst)
    );

    unsafe { raw::cad_session_release(session.0) };
}

/// Releasing a session while calls are still in flight must not crash.
///
/// This is the case `Session.cpp` is explicitly built for: exports take a shared_ptr COPY under
/// the global mutex before locking the session, so a concurrent release means "no new calls can
/// find it" rather than "destroy it now, whoever is inside". Without that, a release landing
/// between lookup and lock would leave the next line locking a dangling mutex.
#[test]
fn releasing_a_session_under_load_does_not_crash() {
    for _ in 0..20 {
        let session = Shared(unsafe { raw::cad_session_create() });
        let barrier = Arc::new(Barrier::new(4));

        std::thread::scope(|scope| {
            for t in 0..3 {
                let barrier = Arc::clone(&barrier);
                scope.spawn(move || {
                    let ty = std::ffi::CString::new("Box").unwrap();
                    barrier.wait();
                    for _ in 0..40 {
                        let mut o: raw::CadObject = 0;
                        // After the release these must return BAD_HANDLE, never fault.
                        unsafe { raw::cad_object_add(session.0, ty.as_ptr(), &mut o) };
                        let _ = t;
                    }
                });
            }
            scope.spawn(|| {
                barrier.wait();
                unsafe { raw::cad_session_release(session.0) };
            });
        });
    }
}

// ── sessions sharing an on-disk cache ───────────────────────────────────────────────────

/// Two sessions pointed at ONE shared DDC directory, working at the same time.
///
/// This is the configuration ADR 0004 is for — "open this project" also means "use the team's
/// cache" — and it is the one place concurrency reaches storage. `core/recompute/DdcCache` has no
/// mutex of its own, so whatever safety exists comes from assetlib's store and from the fact that
/// entries are content-addressed: two writers producing the same bytes for the same key is benign,
/// two writers producing different bytes for one key would not be.
///
/// Asserts the outcome that matters rather than the mechanism: both sessions finish, and both
/// agree on the geometry. A corrupt entry served to one of them shows up as a digest mismatch.
#[test]
fn sessions_sharing_a_disk_cache_agree_on_the_result() {
    let dir = std::env::temp_dir().join(format!("cad-shared-cache-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    let path = dir.to_str().expect("utf-8 temp path").to_string();

    const THREADS: usize = 4;
    let barrier = Arc::new(Barrier::new(THREADS));

    let digests: Vec<u64> = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..THREADS)
            .map(|_| {
                let barrier = Arc::clone(&barrier);
                let path = path.clone();
                scope.spawn(move || {
                    let mut s = Session::with_cache(&path).expect("cached session");
                    barrier.wait();
                    // Identical geometry on every thread, so every one of them races for the same
                    // cache entries — which is the interesting case, not distinct work.
                    let b = s.add_box(35.0, 35.0, 35.0).expect("box");
                    let t = s.add_box(15.0, 15.0, 80.0).expect("tool");
                    let m = s.add_translate(t, 10.0, 10.0, -20.0).expect("translate");
                    let _ = s.add_cut(b, m).expect("cut");
                    s.recompute().expect("recompute");
                    s.document_digest().expect("digest")
                })
            })
            .collect();
        handles
            .into_iter()
            .map(|h| h.join().expect("thread"))
            .collect()
    });

    for (i, d) in digests.iter().enumerate() {
        assert_eq!(
            *d, digests[0],
            "session {i} sharing the cache produced a different document — a cache entry \
             disagrees with a fresh compute"
        );
    }
    let _ = std::fs::remove_dir_all(&dir);
}
