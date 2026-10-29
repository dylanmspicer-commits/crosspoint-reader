#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// The user's search order for "All dictionaries", stored as folder names joined
// by '/' (a character no folder name can contain).
namespace dictionaryOrder {

inline constexpr char SEPARATOR = '/';

// Position of name in the stored order, or SIZE_MAX when it is not listed.
inline size_t rank(const std::string_view order, const std::string_view name) {
  size_t position = 0;
  size_t start = 0;
  while (start <= order.size()) {
    const size_t end = std::min(order.find(SEPARATOR, start), order.size());
    if (order.substr(start, end - start) == name) return position;
    ++position;
    start = end + 1;
  }
  return SIZE_MAX;
}

// Sort items (already in folder-name order) into the stored order. Folders the
// order does not mention, such as newly added ones, keep name order at the end.
template <typename T, typename NameOf>
void apply(std::vector<T>& items, const std::string_view order, NameOf nameOf) {
  if (order.empty()) return;
  std::stable_sort(items.begin(), items.end(),
                   [&](const T& a, const T& b) { return rank(order, nameOf(a)) < rank(order, nameOf(b)); });
}

// The stored form of names, in this order. Names that would overflow capacity
// (including the terminator) are left off; they then sort after the listed ones.
inline std::string join(const std::vector<std::string>& names, const size_t capacity) {
  std::string out;
  for (const auto& name : names) {
    const size_t needed = out.size() + (out.empty() ? 0 : 1) + name.size() + 1;
    if (name.empty() || name.find(SEPARATOR) != std::string::npos || needed > capacity) continue;
    if (!out.empty()) out.push_back(SEPARATOR);
    out += name;
  }
  return out;
}

// A folder name as shown to the reader: a leading sort prefix such as "1-",
// "02_" or "3 " is dropped, since the order setting now decides the order.
inline std::string displayName(const std::string& folder) {
  size_t i = 0;
  while (i < folder.size() && folder[i] >= '0' && folder[i] <= '9') ++i;
  if (i == 0 || i + 1 >= folder.size()) return folder;
  const char separator = folder[i];
  if (separator != '-' && separator != '_' && separator != ' ' && separator != '.') return folder;
  return folder.substr(i + 1);
}

}  // namespace dictionaryOrder
