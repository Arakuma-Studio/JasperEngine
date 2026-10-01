#include "yaml-cpp/node/detail/memory.h"
#include "yaml-cpp/node/detail/node.h"  // IWYU pragma: keep
#include "yaml-cpp/node/ptr.h"

#include <iterator>
#include <utility>

namespace YAML {
namespace detail {

memory& memory_holder::resolve() {
  while (m_pMemory->m_pForward) {
    shared_memory next = m_pMemory->m_pForward;
    m_pMemory = std::move(next);
  }
  return *m_pMemory;
}

void memory_holder::merge(memory_holder& rhs) {
  resolve();
  rhs.resolve();
  if (m_pMemory == rhs.m_pMemory)
    return;

  // Both end up on the same memory, as before; the smaller node list moves
  if (m_pMemory->m_nodes.size() < rhs.m_pMemory->m_nodes.size()) {
    rhs.m_pMemory->merge(*m_pMemory);
    m_pMemory->m_pForward = rhs.m_pMemory;
    m_pMemory = rhs.m_pMemory;
  } else {
    m_pMemory->merge(*rhs.m_pMemory);
    rhs.m_pMemory->m_pForward = m_pMemory;
    rhs.m_pMemory = m_pMemory;
  }
}

memory::~memory() {
  // Unlinks the forward chain iteratively instead of through nested destructors
  shared_memory next = std::move(m_pForward);
  while (next && next.use_count() == 1) {
    shared_memory after = std::move(next->m_pForward);
    next = std::move(after);
  }
}

node& memory::create_node() {
  m_nodes.push_back(std::make_shared<node>());
  return *m_nodes.back();
}

void memory::merge(memory& rhs) {
  if (m_nodes.empty()) {
    m_nodes.swap(rhs.m_nodes);
    return;
  }
  m_nodes.insert(m_nodes.end(), std::make_move_iterator(rhs.m_nodes.begin()),
                 std::make_move_iterator(rhs.m_nodes.end()));
  Nodes().swap(rhs.m_nodes);
}
}  // namespace detail
}  // namespace YAML
