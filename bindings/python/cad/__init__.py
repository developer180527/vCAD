"""Parametric CAD.

    >>> import cad
    >>> s = cad.Session()
    >>> box = s.add("Box")
    >>> s.set_length(box, "dx", cad.Length.mm(100))
    >>> s.set_length(box, "dy", cad.Length.mm(60))
    >>> s.set_length(box, "dz", cad.Length.mm(40))
    >>> s.recompute().all_succeeded()
    True
    >>> s.volume(box)
    240000.0

The API mirrors the C ABI deliberately: same surface, same guarantees. Geometric references
are ElementNames, never indices, so they survive a rebuild — see docs/decisions/0005.
"""

from ._cad import (  # noqa: F401
    ElementName,
    ImportReport,
    Length,
    ObjectId,
    RecomputeReport,
    Session,
    State,
    Unit,
    __version__,
    import_file,
    parse_length,
)

__all__ = [
    "ElementName", "ImportReport", "Length", "ObjectId", "RecomputeReport",
    "Session", "State", "Unit", "import_file", "parse_length", "__version__",
]
