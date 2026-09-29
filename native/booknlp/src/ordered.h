// Insertion-ordered map / counter, to reproduce Python dict and Counter iteration order.
#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rm::booknlp {

template <class K, class V, class Hash = std::hash<K>>
class OrderedMap {
public:
    V & operator[](const K & k) {
        auto it = index_.find(k);
        if (it != index_.end()) return items_[it->second].second;
        index_.emplace(k, items_.size());
        items_.emplace_back(k, V());
        return items_.back().second;
    }
    bool contains(const K & k) const { return index_.count(k) != 0; }
    const V * find(const K & k) const {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    V * find(const K & k) {
        auto it = index_.find(k);
        return it == index_.end() ? nullptr : &items_[it->second].second;
    }
    const V & at(const K & k) const { return items_.at(index_.at(k)).second; }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    auto begin() { return items_.begin(); }
    auto end() { return items_.end(); }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }

private:
    std::vector<std::pair<K, V>> items_;
    std::unordered_map<K, size_t, Hash> index_;
};

// collections.Counter.most_common(): by count descending, ties in insertion order
template <class K, class Hash = std::hash<K>>
std::vector<std::pair<K, int>> most_common(const OrderedMap<K, int, Hash> & c) {
    std::vector<std::pair<K, int>> v(c.begin(), c.end());
    std::stable_sort(v.begin(), v.end(), [](const auto & a, const auto & b) { return a.second > b.second; });
    return v;
}

}  // namespace rm::booknlp
