// Spike 0.1 / 0.2 — prove OCCT 8.0.1 works on every target before M1 starts.
//
// Deliberately uses the modern idioms so we find out now if our assumptions are wrong:
//   * Standard_Failure inherits std::exception in 8.x -> plain catch(const std::exception&)
//   * DataExchange toolkits are TKDE*-prefixed since 7.8 -> TKDESTEP, not TKSTEP
//   * arm64 macOS/iOS is first-class in the 8.x CMake

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS.hxx>

#include <cstdio>
#include <exception>

static int countEdges(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_EDGE); e.More(); e.Next()) ++n;
    return n;
}

int main() {
    try {
        TopoDS_Shape box = BRepPrimAPI_MakeBox(100.0, 60.0, 40.0).Shape();
        TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(15.0, 100.0).Shape();

        BRepAlgoAPI_Cut cut(box, cyl);
        if (!cut.IsDone()) { std::fprintf(stderr, "cut not done\n"); return 1; }
        TopoDS_Shape cutShape = cut.Shape();

        BRepFilletAPI_MakeFillet fillet(cutShape);
        int added = 0;
        for (TopExp_Explorer e(cutShape, TopAbs_EDGE); e.More() && added < 4; e.Next(), ++added) {
            fillet.Add(2.0, TopoDS::Edge(e.Current()));
        }
        fillet.Build();
        if (!fillet.IsDone()) { std::fprintf(stderr, "fillet not done\n"); return 1; }
        TopoDS_Shape result = fillet.Shape();

        if (!BRepCheck_Analyzer(result).IsValid()) {
            std::fprintf(stderr, "result invalid\n");
            return 1;
        }

        STEPControl_Writer writer;
        writer.Transfer(result, STEPControl_AsIs);
        if (writer.Write("spike_occt_smoke.step") != IFSelect_RetDone) {
            std::fprintf(stderr, "STEP write failed\n"); return 1;
        }

        STEPControl_Reader reader;
        if (reader.ReadFile("spike_occt_smoke.step") != IFSelect_RetDone) {
            std::fprintf(stderr, "STEP read failed\n"); return 1;
        }
        reader.TransferRoots();
        TopoDS_Shape roundTripped = reader.OneShape();

        std::printf("edges: built=%d roundtripped=%d\n",
                    countEdges(result), countEdges(roundTripped));

        // NOTE: edge counts routinely differ across a STEP round trip. That is exactly the
        // kind of thing docs/FORMATS.md rule 1 exists for, and why import must produce a
        // report rather than pretend nothing was lost.
        return 0;
    } catch (const std::exception& e) {
        // OCCT 8.x: Standard_Failure derives from std::exception, so this is sufficient.
        std::fprintf(stderr, "exception: %s\n", e.what());
        return 1;
    }
}
