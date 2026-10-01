#ifndef VALUE_DETAIL_MEMORY_H_62B23520_7C8E_11DE_8A39_0800200C9A66
#define VALUE_DETAIL_MEMORY_H_62B23520_7C8E_11DE_8A39_0800200C9A66

#if defined(_MSC_VER) ||                                            \
    (defined(__GNUC__) && (__GNUC__ == 3 && __GNUC_MINOR__ >= 4) || \
     (__GNUC__ >= 4))  // GCC supports "pragma once" correctly since 3.4
#pragma once
#endif

#include <vector>

#include "yaml-cpp/dll.h"
#include "yaml-cpp/node/ptr.h"

namespace YAML {
namespace detail {
class node;
}  // namespace detail
}  // namespace YAML

namespace YAML {
namespace detail {
// Owns the nodes of one tree. Merging two memories (assigning a node of one
// tree into another) moves the smaller node list into the larger one and
// leaves the emptied memory forwarding to the merged one, so a node moves at
// most O(log n) times however the tree is assembled.
class YAML_CPP_API memory {
 public:
  memory() : m_nodes{}, m_pForward{} {}
  memory(const memory&) = delete;
  memory& operator=(const memory&) = delete;
  ~memory();

  node& create_node();
  // Takes over the nodes of rhs, which must not be forwarded
  void merge(memory& rhs);

 private:
  using Nodes = std::vector<shared_node>;
  Nodes m_nodes;
  // Set when this memory was merged into another one, which now holds its nodes
  shared_memory m_pForward;

  friend class memory_holder;
};

class YAML_CPP_API memory_holder {
 public:
  memory_holder() : m_pMemory(std::make_shared<memory>()) {}

  node& create_node() { return resolve().create_node(); }
  void merge(memory_holder& rhs);

 private:
  // The memory the nodes live in now, following (and shortening) forwards
  memory& resolve();

  shared_memory m_pMemory;
};
}  // namespace detail
}  // namespace YAML

#endif  // VALUE_DETAIL_MEMORY_H_62B23520_7C8E_11DE_8A39_0800200C9A66
