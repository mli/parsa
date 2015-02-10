#pragma once
#include <vector>

// double linked list supporting random access and cache, the value is sorted
class DblinkArray {
 public:
  void init(const std::vector<int>& data, int cache_limit);
  void remove(int i);
  void decrAndSort(int i);
  int operator[](int i) { return data_[i].value; }
  int minIdx() {
    // LL << data_[cached_pos_[0]].value;
    return cached_pos_[0];
  }
  // valid check, for debug purpose
  void check();
 private:
  struct Entry {
    int value;
    int prev = -1;
    int next = -1;
  };
  std::vector<Entry> data_;
  // cached_pos_[i] is the first index whose value >= i
  std::vector<int> cached_pos_;
  int cache_limit_;
  int size_ = 0;
};
