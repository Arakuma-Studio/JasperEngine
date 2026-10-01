# JasperEngine patches to yaml-cpp 0.8.0

Local changes to the vendored yaml-cpp, to re-apply on an upgrade (or drop once upstream
has an equivalent). None changes what yaml-cpp parses or emits: node values, styles,
tags and emitted text are the same; they only change how node memory is kept and how
map keys are compared.

Measured on 1,000 sprite-like entities (~300 nodes each, what the editor's
`Stream::encodeEntity` builds for a sprite), 2-CPU container, warm runs:

| Step | Stock | Patched |
|---|---|---|
| Build the 1,000 entity trees | 565–850 ms | 190–350 ms |
| Assign the sequence of them into a document | 220–310 ms | < 1 ms |
| Read them back (map lookups, 57 absent keys each) | 215–255 ms | 125–140 ms |

In the editor this is most of Copy, Cut, Duplicate and Delete (which encode the
GameObjects) and of Paste and undoing a delete (which decode them).

## 1. Node memory: forwarding pools instead of set merges

`include/yaml-cpp/node/detail/memory.h`, `src/memory.cpp`

Every standalone `YAML::Node` owns a `memory` holding its nodes. Assigning a node of one
tree into another merged the two by inserting every `shared_ptr<node>` of one into the
other's `std::set`, and as trees are built bottom-up each node was re-inserted once per
nesting level (the "assign" row above is one such merge of 300k nodes).

Now `memory` keeps its nodes in a vector. A merge moves the smaller list into the larger
one and leaves the emptied memory forwarding (`m_pForward`) to the merged one, so a node
moves at most O(log n) times. `memory_holder` follows the forwards (shortening its own
pointer) before creating or merging nodes. Nodes stay alive as before: every holder
reaches, through the forwards, the memory that now owns its nodes. `~memory` unlinks
forward chains iteratively.

Also in this group: `std::make_shared` instead of `shared_ptr(new ...)` for `node`,
`node_ref`, `node_data`, `memory` and `memory_holder` (`node.h`, `node_ref.h`,
`node/impl.h`, `src/nodebuilder.cpp`): one allocation instead of two each.

## 2. Map key lookup without a temporary Node

`include/yaml-cpp/node/detail/node.h`, `include/yaml-cpp/node/detail/impl.h`

`node::equals(const char*)` built a `Node` (a `shared_ptr` copy) and copied the key's
scalar through `convert<std::string>::decode` for every key compared. Now
`equals(const char*)` and a new `equals(const std::string&)` compare the scalar in place
when the key node is a scalar, which is exactly what that decode accepts.

## 3. Missing text keys named without a stream

`include/yaml-cpp/node/impl.h`

`const Node::operator[]` names a missing key in the zombie node it returns
(`key_to_string`), which built a `std::stringstream` imbued with a named locale for every
lookup that misses. Overloads for `std::string` and `const char*` (including character
arrays) return the key text directly, which is what streaming it gives.
