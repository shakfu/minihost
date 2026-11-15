// chain_library.cpp -- see chain_library.h.

#include "chain_library.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace minihost_desktop::chains {

namespace {

using project::ChainSpec;
using project::ProjectError;

[[noreturn]] void fail(const juce::String& msg) { throw ProjectError(msg.toStdString()); }

const juce::String kSep = "/";
const char* const kFields[] = { "nodes", "edges" };

bool has(const juce::var& o, const char* key)
{
    auto* d = o.getDynamicObject();
    return d != nullptr && d->hasProperty(key);
}

juce::Array<juce::var>* arrayOf(juce::var& o, const char* key)
{
    if (!has(o, key)) o.getDynamicObject()->setProperty(key, juce::Array<juce::var>());
    return o.getDynamicObject()->getProperty(key).getArray();
}

void set(juce::var& o, const juce::Identifier& key, const juce::var& v)
{
    o.getDynamicObject()->setProperty(key, v);
}

bool isPort(const juce::var& n) { return isPortKind(n["kind"].toString()); }

// A chain's port nodes of one kind, in file order.
juce::StringArray portsOf(const juce::var& chain, bool midi, bool in)
{
    juce::StringArray out;
    for (const auto& n : *chain["nodes"].getArray())
        if (n["kind"].toString() == portKind(midi, in))
            out.add(n["id"].toString());
    return out;
}

ChainSpec specOf(const juce::var& n)
{
    ChainSpec s;
    s.id     = n["id"].toString();
    s.source = n["source"].toString();
    return s;
}

// Every node id of a serialized project, chain nodes expanded.
std::set<juce::String> idsOf(const juce::var& project)
{
    std::set<juce::String> ids;
    if (auto* nodes = project["nodes"].getArray())
        for (const auto& n : *nodes)
        {
            const auto id = n["id"].toString();
            ids.insert(id);
            if (n["kind"].toString() == "chain")
                if (auto* inner = n["nodes"].getArray())
                    for (const auto& m : *inner) ids.insert(id + kSep + m["id"].toString());
        }
    return ids;
}

int findChain(const juce::var& project, const juce::String& cid)
{
    auto* nodes = project["nodes"].getArray();
    for (int i = 0; nodes != nullptr && i < nodes->size(); ++i)
        if ((*nodes)[i]["kind"].toString() == "chain" && (*nodes)[i]["id"].toString() == cid)
            return i;
    fail("no chain named " + cid);
}

void checkChainId(const juce::String& cid)
{
    if (cid.isEmpty() || cid.contains(kSep))
        fail("chain id '" + cid + "' must be non-empty, no '/'");
}

// Rename node ids in edges, automation and layout.
void renameRefs(juce::var& project, const std::map<juce::String, juce::String>& to)
{
    auto pick = [&](const juce::var& v) {
        auto it = to.find(v.toString());
        return it == to.end() ? v : juce::var(it->second);
    };
    if (auto* edges = project["edges"].getArray())
        for (auto& e : *edges)
        {
            set(e, "src", pick(e["src"]));
            set(e, "dst", pick(e["dst"]));
        }
    if (auto* lanes = project["automation"].getArray())
        for (auto& a : *lanes) set(a, "node", pick(a["node"]));
    if (auto* layout = project["layout"].getDynamicObject())
        for (const auto& [from, dest] : to)
            if (layout->hasProperty(from))
            {
                const auto pos = layout->getProperty(from);
                layout->removeProperty(from);
                layout->setProperty(dest, pos);
            }
}

juce::DynamicObject* layoutOf(juce::var& project)
{
    if (!project["layout"].isObject())
        set(project, "layout", juce::var(new juce::DynamicObject()));
    return project["layout"].getDynamicObject();
}

juce::var position(float x, float y)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("x", (double) x);
    o->setProperty("y", (double) y);
    return juce::var(o);
}

// Place each of `chain`'s nodes missing from the layout at `origin`
// plus its library position, or in a row when it has none.
void placeMissing(juce::var& project, const juce::String& cid, const juce::var& chain,
                  juce::Point<float> origin)
{
    auto* layout = layoutOf(project);
    int i = 0;
    for (const auto& n : *chain["nodes"].getArray())
    {
        const auto local = n["id"].toString();
        const auto full  = cid + kSep + local;
        const auto rel   = chain["layout"][juce::Identifier(local)];
        if (!layout->hasProperty(full))
            layout->setProperty(full, rel.isObject()
                ? position(origin.x + (float) (double) rel["x"], origin.y + (float) (double) rel["y"])
                : position(origin.x + 200.0f * (float) i, origin.y));
        ++i;
    }
}

using ChainMap = std::map<juce::String, juce::var>;

ChainMap chainsIn(const juce::var& project)
{
    ChainMap out;
    if (auto* nodes = project["nodes"].getArray())
        for (const auto& n : *nodes)
            if (n["kind"].toString() == "chain") out[n["id"].toString()] = n;
    return out;
}

// Route edges that name a chain id to its port nodes (full ids). Throws
// for an edge that names a node inside a chain, or a missing port.
void resolve(juce::Array<juce::var>& edges, const ChainMap& chains)
{
    auto port = [&](const ChainMap::value_type& c, bool midi, bool in, const juce::var& k_var) {
        const auto ports = portsOf(c.second, midi, in);
        const int k = k_var.isVoid() ? 0 : (int) k_var;
        if (!(k_var.isVoid() || k_var.isInt() || k_var.isInt64()) || k < 0 || k >= ports.size())
            fail("chain " + c.first + " has no " + (midi ? "MIDI " : "audio ")
                 + (in ? "inlet " : "outlet ") + k_var.toString());
        return c.first + kSep + ports[k];
    };
    for (auto& e : edges)
    {
        if (!e.isObject()) continue;
        for (const char* end : { "src", "dst" })
        {
            const auto v = e[end].toString();
            if (v.contains(kSep) && chains.count(v.upToFirstOccurrenceOf(kSep, false, false)))
                fail("edge " + e["src"].toString() + " -> " + e["dst"].toString()
                     + ": connect to chain " + v.upToFirstOccurrenceOf(kSep, false, false)
                     + ", not to a node inside it");
        }
        const bool midi = e["kind"].toString() == "midi";
        auto* o = e.getDynamicObject();
        if (auto it = chains.find(e["src"].toString()); it != chains.end())
        {
            o->setProperty("src", port(*it, midi, false, e["src_port"]));
            o->removeProperty("src_port");
        }
        if (auto it = chains.find(e["dst"].toString()); it != chains.end())
        {
            o->setProperty("dst", port(*it, midi, true, e["dst_port"]));
            if (midi) o->removeProperty("dst_port");
            else      o->setProperty("dst_port", 0);
        }
    }
}

// The inverse: an edge to or from a port node names the chain and the
// port's index among the chain's ports of that kind.
void unresolve(juce::Array<juce::var>& edges, const ChainMap& chains)
{
    auto index = [&](const juce::String& full, bool midi, bool in) -> std::pair<juce::String, int> {
        const auto cid = full.upToFirstOccurrenceOf(kSep, false, false);
        auto it = chains.find(cid);
        if (!full.contains(kSep) || it == chains.end()) return { {}, -1 };
        const int k = portsOf(it->second, midi, in).indexOf(full.fromFirstOccurrenceOf(kSep, false, false));
        return { cid, k };
    };
    for (auto& e : edges)
    {
        if (!e.isObject()) continue;
        const bool midi = e["kind"].toString() == "midi";
        auto* o = e.getDynamicObject();
        if (auto [cid, k] = index(e["src"].toString(), midi, false); k >= 0)
        {
            o->setProperty("src", cid);
            if (k > 0) o->setProperty("src_port", k);
        }
        if (auto [cid, k] = index(e["dst"].toString(), midi, true); k >= 0)
        {
            o->setProperty("dst", cid);
            if (midi && k == 0) o->removeProperty("dst_port");
            else                o->setProperty("dst_port", k);
        }
    }
}

} // namespace

bool forbiddenKind(const juce::String& k)
{
    static const juce::StringArray kinds{ "input", "output", "device_input", "device_output",
                                          "midi_input", "midi_output", "chain" };
    return kinds.contains(k);
}

bool isPortKind(const juce::String& k)
{
    return k == "audio_inlet" || k == "audio_outlet" || k == "midi_inlet" || k == "midi_outlet";
}

juce::String portKind(bool midi, bool in)
{
    return juce::String(midi ? "midi_" : "audio_") + (in ? "inlet" : "outlet");
}

juce::File libraryDir()
{
    // Same rule as settingsFile() in main.cpp.
    if (const auto* env = std::getenv("MINIHOST_DESKTOP_SETTINGS_DIR"); env != nullptr && env[0] != '\0')
        return juce::File(juce::String::fromUTF8(env)).getChildFile("chains");
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("minihost").getChildFile("chains");
}

bool validName(const juce::String& name)
{
    return name.trim() == name && name.isNotEmpty() && !name.startsWith(".")
        && !name.containsAnyOf("/\\:");
}

juce::File fileFor(const juce::String& name)
{
    return libraryDir().getChildFile(name + kExtension);
}

juce::StringArray names()
{
    juce::StringArray out;
    for (const auto& f : libraryDir().findChildFiles(juce::File::findFiles, false,
                                                     juce::String("*") + kExtension))
        out.add(f.getFileNameWithoutExtension());
    out.sort(false);
    return out;
}

void validate(const juce::var& chain, const juce::String& where)
{
    if (!chain.isObject()) fail(where + ": chain must be an object");
    const auto* nodes = chain["nodes"].getArray();
    if (nodes == nullptr || nodes->isEmpty()) fail(where + ": nodes must be a non-empty list");
    std::set<juce::String> ids;
    for (const auto& n : *nodes)
    {
        if (!n.isObject()) fail(where + ": node entry must be an object");
        const auto id = n["id"].toString();
        if (!n["id"].isString() || id.isEmpty() || id.contains(kSep))
            fail(where + ": node id '" + id + "' must be non-empty, no '/'");
        if (!ids.insert(id).second) fail(where + ": duplicate node id '" + id + "'");
        if (forbiddenKind(n["kind"].toString()))
            fail(where + ": node '" + id + "': kind '" + n["kind"].toString()
                 + "' cannot be in a chain");
    }
    std::map<juce::String, juce::String> kind;
    for (const auto& n : *nodes) kind[n["id"].toString()] = n["kind"].toString();
    auto isNode = [&](const juce::var& v) { return v.isString() && ids.count(v.toString()) > 0; };
    if (has(chain, "edges"))
    {
        const auto* edges = chain["edges"].getArray();
        if (edges == nullptr) fail(where + ": edges must be a list");
        for (const auto& e : *edges)
        {
            if (!e.isObject() || !isNode(e["src"]) || !isNode(e["dst"]))
                fail(where + ": edge " + juce::JSON::toString(e, true) + " must join two chain nodes");
            if (kind[e["dst"].toString()].endsWith("_inlet"))
                fail(where + ": edge into inlet '" + e["dst"].toString() + "'; it is fed from outside");
            if (kind[e["src"].toString()].endsWith("_outlet"))
                fail(where + ": edge from outlet '" + e["src"].toString() + "'; it feeds the outside");
        }
    }
}

juce::var readChain(const juce::File& file)
{
    const auto where = file.getFullPathName();
    if (!file.existsAsFile()) fail("chain file not found: " + where);
    auto doc = juce::JSON::parse(file);
    if (!doc.isObject()) fail(where + ": chain root must be an object");
    if (!(doc["minihost_chain_version"].isInt() && (int) doc["minihost_chain_version"] == kVersion))
        fail(where + ": unsupported chain version " + doc["minihost_chain_version"].toString());
    validate(doc, where);
    for (auto& n : *doc["nodes"].getArray())
    {
        const auto p = n["path"].toString();
        if (n["path"].isString() && !juce::File::isAbsolutePath(p))
            set(n, "path", file.getParentDirectory().getChildFile(p).getFullPathName());
    }
    return doc;
}

void writeChain(const juce::File& file, const juce::var& chain)
{
    validate(chain, file.getFileName());
    auto* o = new juce::DynamicObject();
    juce::var doc(o);
    o->setProperty("minihost_chain_version", kVersion);
    o->setProperty("name", file.getFileNameWithoutExtension());
    for (const char* key : kFields)
        if (has(chain, key)) o->setProperty(key, chain[key]);
    if (has(chain, "layout")) o->setProperty("layout", chain["layout"]);

    if (!file.getParentDirectory().createDirectory())
        fail("cannot create " + file.getParentDirectory().getFullPathName());
    const auto tmp = file.getSiblingFile(file.getFileName() + ".tmp");
    if (!tmp.replaceWithText(juce::JSON::toString(doc, false) + "\n") || !tmp.replaceFileIn(file))
    {
        tmp.deleteFile();
        fail("failed to write " + file.getFullPathName());
    }
}

void expand(juce::Array<juce::var>& nodes, juce::Array<juce::var>& edges,
            std::vector<ChainSpec>& out)
{
    std::map<juce::String, juce::var> by_id;
    juce::Array<juce::var> flat, routed;  // inner edges first, as in Python
    for (const auto& n : nodes)
    {
        if (!n.isObject() || n["kind"].toString() != "chain") { flat.add(n); continue; }
        const auto cid = n["id"].toString();
        checkChainId(cid);
        validate(n, "chain " + cid);
        by_id[cid] = n;
        out.push_back(specOf(n));
        for (const auto& inner : *n["nodes"].getArray())
        {
            auto c = inner.clone();
            set(c, "id", cid + kSep + inner["id"].toString());
            flat.add(c);
        }
        if (auto* inner_edges = n["edges"].getArray())
            for (const auto& e : *inner_edges)
            {
                auto c = e.clone();
                set(c, "src", cid + kSep + e["src"].toString());
                set(c, "dst", cid + kSep + e["dst"].toString());
                routed.add(c);
            }
    }
    if (by_id.empty()) return;
    auto outer = edges;
    for (auto& e : outer) e = e.clone();
    resolve(outer, by_id);
    routed.addArray(outer);
    nodes = std::move(flat);
    edges = std::move(routed);
}

float pinY(const project::ProjectDocument::Layout& layout, const juce::String& id)
{
    const auto it = layout.find(id.toStdString());
    return it == layout.end() ? std::numeric_limits<float>::infinity() : it->second.y;
}

void fold(juce::Array<juce::var>& nodes, juce::Array<juce::var>& edges,
          const std::vector<ChainSpec>& chains, const project::ProjectDocument::Layout& layout)
{
    for (const auto& spec : chains)
    {
        const auto prefix = spec.id + kSep;
        auto localOf = [&](const juce::String& id) {
            const auto rest = id.fromFirstOccurrenceOf(prefix, false, false);
            return id.startsWith(prefix) && rest.isNotEmpty() && !rest.contains(kSep)
                ? rest : juce::String();
        };
        juce::Array<juce::var> inner;
        int at = -1;
        for (int i = 0; i < nodes.size();)
        {
            const auto local = localOf(nodes[i]["id"].toString());
            if (local.isEmpty()) { ++i; continue; }
            if (at < 0) at = i;
            auto c = nodes[i];
            set(c, "id", local);
            inner.add(c);
            nodes.remove(i);
        }
        if (inner.isEmpty()) continue;
        std::vector<int> slots;
        std::vector<juce::var> ports;
        for (int i = 0; i < inner.size(); ++i)
            if (isPort(inner[i])) { slots.push_back(i); ports.push_back(inner[i]); }
        std::stable_sort(ports.begin(), ports.end(), [&](const juce::var& a, const juce::var& b) {
            return pinY(layout, prefix + a["id"].toString()) < pinY(layout, prefix + b["id"].toString());
        });
        for (size_t k = 0; k < slots.size(); ++k) inner.set(slots[k], ports[k]);

        auto* o = new juce::DynamicObject();
        juce::var chain(o);
        o->setProperty("id", spec.id);
        o->setProperty("kind", "chain");
        if (spec.source.isNotEmpty()) o->setProperty("source", spec.source);
        o->setProperty("nodes", inner);
        juce::Array<juce::var> inner_edges;
        for (int i = 0; i < edges.size();)
        {
            const auto s = localOf(edges[i]["src"].toString());
            const auto d = localOf(edges[i]["dst"].toString());
            if (s.isEmpty() || d.isEmpty()) { ++i; continue; }
            auto c = edges[i];
            set(c, "src", s);
            set(c, "dst", d);
            inner_edges.add(c);
            edges.remove(i);
        }
        o->setProperty("edges", inner_edges);
        nodes.insert(at, chain);
    }
    ChainMap by_id;
    for (const auto& n : nodes)
        if (n["kind"].toString() == "chain") by_id[n["id"].toString()] = n;
    unresolve(edges, by_id);
}

void group(juce::var& project, const juce::StringArray& ids, const juce::String& cid,
           const juce::String& source, const std::function<int(const juce::String&)>& channels_of)
{
    checkChainId(cid);
    if (idsOf(project).count(cid)) fail("id '" + cid + "' is already in use");
    if (ids.isEmpty()) fail("select the nodes to group");
    resolve(*arrayOf(project, "edges"), chainsIn(project));
    auto* nodes = project["nodes"].getArray();
    std::map<juce::String, juce::String> to_local;
    std::set<juce::String> locals;
    int at = -1;
    for (const auto& id : ids)
    {
        int idx = -1;
        for (int i = 0; i < nodes->size() && idx < 0; ++i)
            if ((*nodes)[i]["id"].toString() == id) idx = i;
        if (idx < 0)
            fail("'" + id + "' is inside a chain; ungroup that chain first");
        const auto kind = (*nodes)[idx]["kind"].toString();
        if (kind == "chain") fail("chains cannot contain chains; ungroup " + id + " first");
        if (forbiddenKind(kind)) fail("a " + kind + " node cannot be in a chain");
        auto local = id.replace(kSep, "_");
        for (int k = 2; locals.count(local); ++k)
            local = id.replace(kSep, "_") + "_" + juce::String(k);
        to_local[id] = local;
        locals.insert(local);
        at = at < 0 ? idx : std::min(at, idx);
    }
    auto fresh = [&](const juce::String& stem) {
        auto id = stem;
        for (int k = 2; locals.count(id); ++k) id = stem + "_" + juce::String(k);
        locals.insert(id);
        return id;
    };

    juce::Array<juce::var> inner, ins, outs, inner_edges;
    for (int i = 0; i < nodes->size();)
    {
        auto it = to_local.find((*nodes)[i]["id"].toString());
        if (it == to_local.end()) { ++i; continue; }
        auto c = (*nodes)[i];
        set(c, "id", it->second);
        inner.add(c);
        nodes->remove(i);
    }
    auto edge = [](const juce::String& src, const juce::String& dst, const juce::var& like,
                   const juce::var& port) {
        auto c = like.clone();
        set(c, "src", src);
        set(c, "dst", dst);
        if (port.isVoid()) c.getDynamicObject()->removeProperty("dst_port");
        else               set(c, "dst_port", port);
        return c;
    };
    // One port per outside source of an edge in, one per inside source of
    // an edge out; the edge then runs through the port.
    std::map<std::pair<juce::String, bool>, juce::String> in_port, out_port;
    auto port = [&](juce::Array<juce::var>& list, bool midi, bool in, const juce::String& src) {
        auto* o = new juce::DynamicObject();
        const auto id = fresh(juce::String(midi ? "midi_" : "") + (in ? "inlet" : "outlet"));
        o->setProperty("id", id);
        o->setProperty("kind", portKind(midi, in));
        if (!midi)
        {
            const int ch = channels_of ? channels_of(src) : -1;
            o->setProperty("channels", ch > 0 ? ch : 2);
        }
        list.add(juce::var(o));
        return id;
    };
    juce::Array<juce::var> outside;
    for (const auto& e : *project["edges"].getArray())
    {
        const auto s = e["src"].toString(), d = e["dst"].toString();
        const bool in_s = to_local.count(s) > 0, in_d = to_local.count(d) > 0;
        const bool midi = e["kind"].toString() == "midi";
        const juce::var zero = midi ? juce::var() : juce::var(0);
        if (in_s && in_d)
            inner_edges.add(edge(to_local[s], to_local[d], e, e["dst_port"]));
        else if (in_d)
        {
            auto& p = in_port[{ s, midi }];
            if (p.isEmpty()) p = port(ins, midi, true, s);
            inner_edges.add(edge(p, to_local[d], e, e["dst_port"]));
            const bool wired = std::any_of(outside.begin(), outside.end(), [&](const juce::var& x) {
                return x["src"].toString() == s && x["dst"].toString() == cid + kSep + p;
            });
            if (!wired) outside.add(edge(s, cid + kSep + p, e, zero));
        }
        else if (in_s)
        {
            auto& p = out_port[{ s, midi }];
            if (p.isEmpty())
            {
                p = port(outs, midi, false, s);
                inner_edges.add(edge(to_local[s], p, e, zero));
            }
            outside.add(edge(cid + kSep + p, d, e, e["dst_port"]));
        }
        else
            outside.add(e);
    }
    project.getDynamicObject()->setProperty("edges", outside);
    juce::Array<juce::var> all;
    all.addArray(ins);
    all.addArray(inner);
    all.addArray(outs);

    auto* o = new juce::DynamicObject();
    o->setProperty("id", cid);
    o->setProperty("kind", "chain");
    if (source.isNotEmpty()) o->setProperty("source", source);
    o->setProperty("nodes", all);
    o->setProperty("edges", inner_edges);
    nodes->insert(std::min(at, nodes->size()), juce::var(o));

    std::map<juce::String, juce::String> to_full;
    for (const auto& [id, local] : to_local) to_full[id] = cid + kSep + local;
    renameRefs(project, to_full);
    unresolve(*arrayOf(project, "edges"), chainsIn(project));

    // Inputs to the left of the selection, outputs to its right, each level
    // with the node it connects to.
    auto* layout = layoutOf(project);
    auto posOf = [&](const juce::String& local) -> std::optional<juce::Point<float>> {
        const auto p = layout->getProperty(cid + kSep + local);
        if (!p.isObject()) return std::nullopt;
        return juce::Point<float>{ (float) (double) p["x"], (float) (double) p["y"] };
    };
    float left = 1e9f, right = -1e9f, top = 1e9f;
    for (const auto& [id, local] : to_local)
        if (auto p = posOf(local))
        {
            left = std::min(left, p->x); right = std::max(right, p->x); top = std::min(top, p->y);
        }
    if (left > right) return;
    layout->setProperty(cid, position(left, top));
    auto place = [&](const juce::Array<juce::var>& ports, float x) {
        float last_y = -1e9f;
        for (const auto& n : ports)
        {
            const auto id = n["id"].toString();
            float y = 0.0f;
            for (const auto& e : inner_edges)
            {
                const auto other = e["src"].toString() == id ? e["dst"].toString()
                                 : e["dst"].toString() == id ? e["src"].toString() : juce::String();
                if (auto p = other.isNotEmpty() ? posOf(other) : std::nullopt) { y = p->y; break; }
            }
            y = std::max(y, last_y + 60.0f);
            last_y = y;
            layout->setProperty(cid + kSep + id, position(x, y));
        }
    };
    place(ins, left - 200.0f);
    place(outs, right + 200.0f);
}

void ungroup(juce::var& project, const juce::String& cid)
{
    const int idx = findChain(project, cid);
    resolve(*arrayOf(project, "edges"), chainsIn(project));
    auto* nodes = project["nodes"].getArray();
    const auto chain = (*nodes)[idx];
    nodes->remove(idx);
    auto ids = idsOf(project);
    std::map<juce::String, juce::String> to_top, local_to_top;
    juce::Array<juce::var> moved;
    juce::StringArray ports;
    for (const auto& n : *chain["nodes"].getArray())
    {
        const auto local = n["id"].toString();
        auto top = local;
        for (int k = 2; ids.count(top); ++k) top = local + "_" + juce::String(k);
        ids.insert(top);
        to_top[cid + kSep + local] = top;
        local_to_top[local] = top;
        if (isPort(n)) { ports.add(top); continue; }
        auto c = n;
        set(c, "id", top);
        moved.add(c);
    }
    for (int k = 0; k < moved.size(); ++k) nodes->insert(idx + k, moved[k]);
    auto* edges = arrayOf(project, "edges");
    if (auto* inner = chain["edges"].getArray())
        for (auto e : *inner)
        {
            set(e, "src", local_to_top[e["src"].toString()]);
            set(e, "dst", local_to_top[e["dst"].toString()]);
            edges->add(e);
        }
    renameRefs(project, to_top);

    // Ports only exist in chains: join each one's sources to its targets.
    for (const auto& p : ports)
    {
        juce::Array<juce::var> into, from;
        for (int i = 0; i < edges->size();)
        {
            const auto& e = edges->getReference(i);
            const bool to_p = e["dst"].toString() == p, from_p = e["src"].toString() == p;
            if (to_p)   into.add(e);
            if (from_p) from.add(e);
            if (to_p || from_p) edges->remove(i); else ++i;
        }
        for (const auto& a : into)
            for (auto b : from)
            {
                b = b.clone();
                set(b, "src", a["src"]);
                edges->add(b);
            }
        if (auto* layout = project["layout"].getDynamicObject()) layout->removeProperty(p);
    }
    if (auto* layout = project["layout"].getDynamicObject()) layout->removeProperty(cid);
    unresolve(*edges, chainsIn(project));
}

void insert(juce::var& project, const juce::var& chain, const juce::String& cid,
            juce::Point<float> origin)
{
    checkChainId(cid);
    validate(chain, "chain " + cid);
    if (idsOf(project).count(cid)) fail("id '" + cid + "' is already in use");
    auto* o = new juce::DynamicObject();
    juce::var node(o);
    o->setProperty("id", cid);
    o->setProperty("kind", "chain");
    if (chain["name"].toString().isNotEmpty()) o->setProperty("source", chain["name"]);
    for (const char* key : kFields)
        if (has(chain, key)) o->setProperty(key, chain[key].clone());
    arrayOf(project, "nodes")->add(node);
    layoutOf(project)->setProperty(cid, position(origin.x, origin.y));
    placeMissing(project, cid, chain, origin);
}

void replace(juce::var& project, const juce::String& cid, const juce::var& chain)
{
    validate(chain, "chain " + cid);
    const int idx = findChain(project, cid);
    resolve(*arrayOf(project, "edges"), chainsIn(project));
    auto& node = project["nodes"].getArray()->getReference(idx);
    std::set<juce::String> keep;
    for (const auto& n : *chain["nodes"].getArray()) keep.insert(cid + kSep + n["id"].toString());
    auto gone = [&](const juce::var& id) {
        return id.toString().startsWith(cid + kSep) && keep.count(id.toString()) == 0;
    };

    // The chain's corner, from the members that have a position.
    juce::Point<float> origin{ 1e9f, 1e9f };
    auto* layout = layoutOf(project);
    // Inner positions only; the chain's own entry places its box.
    for (const auto& kv : layout->getProperties())
        if (kv.name.toString().startsWith(cid + kSep))
            origin = { std::min(origin.x, (float) (double) kv.value["x"]),
                       std::min(origin.y, (float) (double) kv.value["y"]) };
    if (origin.x > 1e8f) origin = {};

    auto* o = node.getDynamicObject();
    for (const char* key : kFields) o->removeProperty(key);
    for (const char* key : kFields)
        if (has(chain, key)) o->setProperty(key, chain[key].clone());
    if (chain["name"].toString().isNotEmpty()) o->setProperty("source", chain["name"]);

    if (auto* edges = project["edges"].getArray())
        edges->removeIf([&](const juce::var& e) { return gone(e["src"]) || gone(e["dst"]); });
    if (auto* lanes = project["automation"].getArray())
        lanes->removeIf([&](const juce::var& a) { return gone(a["node"]); });
    juce::StringArray stale;
    for (const auto& kv : layout->getProperties())
        if (gone(kv.name.toString())) stale.add(kv.name.toString());
    for (const auto& k : stale) layout->removeProperty(k);
    placeMissing(project, cid, chain, origin);
    unresolve(*arrayOf(project, "edges"), chainsIn(project));
}

juce::var extract(const juce::var& project, const juce::String& cid, const juce::String& name)
{
    const auto node = (*project["nodes"].getArray())[findChain(project, cid)];
    auto* o = new juce::DynamicObject();
    juce::var out(o);
    o->setProperty("minihost_chain_version", kVersion);
    o->setProperty("name", name);
    for (const char* key : kFields)
        if (has(node, key)) o->setProperty(key, node[key].clone());

    juce::Point<float> corner{ 1e9f, 1e9f };
    std::vector<std::pair<juce::String, juce::Point<float>>> at;
    for (const auto& n : *node["nodes"].getArray())
    {
        const auto p = project["layout"][juce::Identifier(cid + kSep + n["id"].toString())];
        if (!p.isObject()) continue;
        juce::Point<float> xy{ (float) (double) p["x"], (float) (double) p["y"] };
        corner = { std::min(corner.x, xy.x), std::min(corner.y, xy.y) };
        at.emplace_back(n["id"].toString(), xy);
    }
    if (!at.empty())
    {
        auto* layout = new juce::DynamicObject();
        for (const auto& [id, xy] : at)
            layout->setProperty(id, position(xy.x - corner.x, xy.y - corner.y));
        o->setProperty("layout", juce::var(layout));
    }
    validate(out, name);
    return out;
}

} // namespace minihost_desktop::chains
