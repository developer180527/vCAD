#include "cad/io/Serialize.h"

#include "cad/kernel/Guard.h"
#include "cad/kernel/internal/Occt.h"

#include <BinTools.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <map>
#include <sstream>

namespace cad::io {
namespace {

using kernel::Error;
using kernel::ErrorCode;

void writeU32(std::ostream& os, std::uint32_t v) {
    // Little-endian, explicitly. The shared DDC tier is read by other machines, so
    // "whatever this compiler does" is not an encoding.
    for (int i = 0; i < 4; ++i) os.put(static_cast<char>((v >> (i * 8)) & 0xFF));
}

bool readU32(std::istream& is, std::uint32_t& out) {
    out = 0;
    for (int i = 0; i < 4; ++i) {
        const int c = is.get();
        if (c == EOF) return false;
        out |= static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << (i * 8);
    }
    return true;
}

void writeString(std::ostream& os, const std::string& s) {
    writeU32(os, static_cast<std::uint32_t>(s.size()));
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

bool readString(std::istream& is, std::string& out) {
    std::uint32_t n = 0;
    if (!readU32(is, n)) return false;
    out.resize(n);
    if (n != 0) is.read(out.data(), static_cast<std::streamsize>(n));
    return is.good() || is.eof();
}

}  // namespace

kernel::Result<std::string> serialize(const document::Output& output) {
    if (output.shape.isNull()) {
        return Error{ErrorCode::InvalidInput, "Cannot serialise an empty shape."};
    }

    return kernel::guard("serialize", [&] {
        std::ostringstream os(std::ios::binary);
        writeU32(os, kSerializationVersion);

        // Shape first, in OCCT's binary format — an order of magnitude smaller and faster
        // than the BREP text format, which matters when this is on the interactive path.
        std::ostringstream shapeStream(std::ios::binary);
        BinTools::Write(kernel::occt(const_cast<kernel::Shape&>(output.shape)), shapeStream);
        writeString(os, shapeStream.str());

        // Then the element map, as (sub-shape index, name text) pairs.
        //
        // Indices are safe HERE and nowhere else: they index into the shape we just wrote,
        // in the same deterministic traversal order the reader will use. They are an
        // encoding detail of one blob, not an identity that outlives it.
        const TopoDS_Shape& shape = kernel::occt(const_cast<kernel::Shape&>(output.shape));
        std::vector<std::pair<std::uint32_t, std::string>> entries;

        std::uint32_t base = 0;
        for (const auto type : {TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX}) {
            TopTools_IndexedMapOfShape map;
            TopExp::MapShapes(shape, type, map);
            for (int i = 1; i <= map.Extent(); ++i) {
                if (const auto name = output.map.nameOf(kernel::wrap(map(i)))) {
                    entries.emplace_back(base + static_cast<std::uint32_t>(i),
                                         name->toString());
                }
            }
            base += 1'000'000u;   // keep the three ranges disjoint
        }

        writeU32(os, static_cast<std::uint32_t>(entries.size()));
        for (const auto& [index, text] : entries) {
            writeU32(os, index);
            writeString(os, text);
        }
        return os.str();
    });
}

kernel::Result<document::Output> deserialize(const std::string& bytes) {
    if (bytes.empty()) {
        return Error{ErrorCode::InvalidInput, "Cannot read an empty cache blob."};
    }

    return kernel::guard("deserialize", [&] {
        std::istringstream is(bytes, std::ios::binary);

        std::uint32_t version = 0;
        if (!readU32(is, version) || version != kSerializationVersion) {
            // Deliberately an error, which the cache turns into a miss. A blob we cannot
            // read must never be able to fail a build.
            throw std::runtime_error("unsupported cache blob version");
        }

        std::string shapeBytes;
        if (!readString(is, shapeBytes)) throw std::runtime_error("truncated shape record");

        document::Output out;
        std::istringstream shapeStream(shapeBytes, std::ios::binary);
        TopoDS_Shape shape;
        BinTools::Read(shape, shapeStream);
        if (shape.IsNull()) throw std::runtime_error("cache blob contained no shape");
        out.shape = kernel::wrap(shape);

        // Rebuild the index -> sub-shape lookup exactly as the writer built it.
        std::uint32_t base = 0;
        std::map<std::uint32_t, TopoDS_Shape> lookup;
        for (const auto type : {TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX}) {
            TopTools_IndexedMapOfShape map;
            TopExp::MapShapes(shape, type, map);
            for (int i = 1; i <= map.Extent(); ++i) {
                lookup[base + static_cast<std::uint32_t>(i)] = map(i);
            }
            base += 1'000'000u;
        }

        std::uint32_t count = 0;
        if (!readU32(is, count)) throw std::runtime_error("truncated element map");
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t index = 0;
            std::string text;
            if (!readU32(is, index) || !readString(is, text)) {
                throw std::runtime_error("truncated element map entry");
            }
            const auto it = lookup.find(index);
            if (it == lookup.end()) {
                // The shape and its map disagree: the blob is corrupt. Refuse it rather
                // than returning a partially-named shape, which would fail later and
                // somewhere less obvious.
                throw std::runtime_error("cache blob element map does not match its shape");
            }
            out.map.bind(kernel::wrap(it->second), naming::ElementName::parse(text));
        }
        return out;
    });
}

}  // namespace cad::io
