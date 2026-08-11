"""Python binding tests.

The only tests in this repo that are not Rust, and necessarily so: you cannot exercise a
Python API from Rust. Everything these check is Python-surface behaviour — that errors
arrive as exceptions rather than status codes, that the objects are idiomatic, that the
docstring example works. The geometry and recompute semantics underneath are covered once,
in Rust, through the C ABI (docs/TESTING.md).

    PYTHONPATH=bindings/python pytest bindings/python/tests
"""

import os
import tempfile

import pytest

import cad


def test_readme_example_works():
    """The example in cad/__init__.py must be true. Docstrings rot silently otherwise."""
    s = cad.Session()
    box = s.add("Box")
    s.set_length(box, "dx", cad.Length.mm(100))
    s.set_length(box, "dy", cad.Length.mm(60))
    s.set_length(box, "dz", cad.Length.mm(40))
    assert s.recompute().all_succeeded()
    assert s.volume(box) == pytest.approx(240000.0)


def test_failures_are_exceptions_not_status_codes():
    """A Python caller must never have to check a return code."""
    s = cad.Session()
    with pytest.raises(TypeError):
        s.add("NoSuchFeature")

    box = s.add("Box")
    with pytest.raises(RuntimeError, match="recompute"):
        s.volume(box)          # no geometry yet

    with pytest.raises(ValueError):
        cad.parse_length("10 furlongs")


def test_a_failed_feature_is_reported_not_raised():
    """A feature that fails is document state, not an exception: the recompute succeeded,
    it is the feature that did not. Raising here would make a partly-broken model
    unopenable."""
    s = cad.Session()
    box = s.add("Box")
    for name, value in (("dx", 100), ("dy", 60), ("dz", 40)):
        s.set_length(box, name, cad.Length.mm(value))
    s.recompute()

    fillet = s.add("Fillet")
    s.set_input(fillet, "base", box)
    s.set_element(fillet, "edges", s.box_edge_between(box, 1, 4))
    s.set_length(fillet, "radius", cad.Length.mm(500))   # far too large

    report = s.recompute()
    assert report.failed == 1
    assert s.state(fillet) == cad.State.FAILED
    assert s.error(fillet)          # legible, and non-empty


def test_element_names_survive_a_rebuild():
    """The M1 guarantee, reachable from Python."""
    s = cad.Session()
    box = s.add("Box")
    for name, value in (("dx", 100), ("dy", 60), ("dz", 40)):
        s.set_length(box, name, cad.Length.mm(value))
    s.recompute()

    edge = s.box_edge_between(box, 1, 4)
    assert edge and not edge.is_null()

    s.set_length(box, "dx", 250.0 and cad.Length.mm(250))
    s.recompute()
    assert str(s.box_edge_between(box, 1, 4)) == str(edge)

    # And the name round-trips through text, which is how it is persisted.
    assert str(cad.ElementName.parse(str(edge))) == str(edge)


def test_undo_redo():
    s = cad.Session()
    box = s.add("Box")
    for name, value in (("dx", 100), ("dy", 60), ("dz", 40)):
        s.set_length(box, name, cad.Length.mm(value))
    s.recompute()
    original = s.volume(box)

    s.set_length(box, "dx", cad.Length.mm(200))
    s.recompute()
    assert s.volume(box) == pytest.approx(original * 2)

    assert s.undo()
    s.recompute()
    assert s.volume(box) == pytest.approx(original)
    assert s.redo()
    s.recompute()
    assert s.volume(box) == pytest.approx(original * 2)


def test_parse_length_never_guesses():
    assert cad.parse_length("10", cad.Unit.MM) == pytest.approx(10.0)
    assert cad.parse_length("10", cad.Unit.IN) == pytest.approx(254.0)
    assert cad.parse_length("1.5in") == pytest.approx(38.1)
    assert cad.parse_length("2ft 6in") == pytest.approx(762.0)


def test_export_and_probe_round_trip():
    s = cad.Session()
    box = s.add("Box")
    for name, value in (("dx", 100), ("dy", 60), ("dz", 40)):
        s.set_length(box, name, cad.Length.mm(value))
    s.recompute()

    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "part.step")
        s.export_file(box, path)
        assert os.path.getsize(path) > 0

        report = cad.import_file(path)
        assert report.solids == 1
        assert report.faces == 6
        assert not report.units_were_assumed      # STEP declares its units
        assert "STEP" in report.summary()

        stl = os.path.join(d, "part.stl")
        s.export_file(box, stl)
        stl_report = cad.import_file(stl)
        assert stl_report.units_were_assumed      # STL does not
        assert not stl_report.lossless()


def test_the_ddc_serves_a_second_session():
    with tempfile.TemporaryDirectory() as d:
        def build():
            s = cad.Session(cache_dir=d)
            box = s.add("Box")
            for name, value in (("dx", 31), ("dy", 41), ("dz", 59)):
                s.set_length(box, name, cad.Length.mm(value))
            return s, box, s.recompute()

        _, _, first = build()
        assert first.computed == 1

        s, box, second = build()
        assert second.cached == 1, "the second session must be served from the DDC"
        assert s.volume(box) == pytest.approx(31 * 41 * 59)


def test_supported_formats_are_discoverable():
    s = cad.Session()
    readable = s.readable_extensions()
    assert ".step" in readable and ".stl" in readable
    assert ".step" in s.writable_extensions()
