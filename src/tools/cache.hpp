/*
 * Copyright (c) 2021 Maneesh P M <manu.pm55@gmail.com>
 * Copyrigth (c) 2021, Matthieu Gautier <mgautier@kymeria.fr>
 * Copyright (c) 2020, Veloman Yunkan
 * Copyright (c) 2014, lamerman
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of lamerman nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU  General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#ifndef LRUCACHE_H
#define LRUCACHE_H

#include <map>
#include <list>
#include <cstddef>
#include <stdexcept>
#include <cassert>
#include <future>
#include <mutex>

template<typename key_t, typename value_t>
class lru_cache {
public: // types
  typedef typename std::pair<key_t, value_t> key_value_pair_t;
  typedef typename std::list<key_value_pair_t>::iterator list_iterator_t;

  enum AccessStatus {
    HIT, // key was found in the cache
    PUT, // key was not in the cache but was created by the getOrPut() access
    MISS // key was not in the cache; get() access failed
  };

  class AccessResult {
    const AccessStatus status_;
    const value_t val_;

  public:
    AccessResult(const value_t& val, AccessStatus status)
      : status_(status), val_(val)
    {}
    AccessResult()
      : status_(MISS), val_()
    {}

    bool hit() const { return status_ == HIT; }
    bool miss() const { return !hit(); }

    operator const value_t& () const { return value(); }

    const value_t& value() const {
      if ( status_ == MISS )
        throw std::range_error("There is no such key in cache");
      return val_;
    }
  };

public: // functions
  explicit lru_cache(size_t max_size)
    : _max_size(max_size)
  {}

  // If 'key' is present in the cache, returns the associated value,
  // otherwise puts the given value into the cache (and returns it with
  // a status of a cache miss).
  AccessResult getOrPut(const key_t& key, const value_t& value) {
    auto it = _cache_items_map.find(key);
    if (it != _cache_items_map.end()) {
      _cache_items_list.splice(_cache_items_list.begin(), _cache_items_list, it->second);
      return AccessResult(it->second->second, HIT);
    } else {
      putMissing(key, value);
      return AccessResult(value, PUT);
    }
  }

  void put(const key_t& key, const value_t& value) {
    auto it = _cache_items_map.find(key);
    if (it != _cache_items_map.end()) {
      _cache_items_list.splice(_cache_items_list.begin(), _cache_items_list, it->second);
      it->second->second = value;
    } else {
      putMissing(key, value);
    }
  }

  AccessResult get(const key_t& key) {
    auto it = _cache_items_map.find(key);
    if (it == _cache_items_map.end()) {
      return AccessResult();
    } else {
      _cache_items_list.splice(_cache_items_list.begin(), _cache_items_list, it->second);
      return AccessResult(it->second->second, HIT);
    }
  }

  bool drop(const key_t& key) {
    try {
      auto list_it = _cache_items_map.at(key);
      _cache_items_list.erase(list_it);
      _cache_items_map.erase(key);
      return true;
    } catch (std::out_of_range& e) {
      return false;
    }
  }

  bool exists(const key_t& key) const {
    return _cache_items_map.find(key) != _cache_items_map.end();
  }

  size_t size() const {
    return _cache_items_map.size();
  }

private: // functions
  void putMissing(const key_t& key, const value_t& value) {
    assert(_cache_items_map.find(key) == _cache_items_map.end());
    _cache_items_list.push_front(key_value_pair_t(key, value));
    _cache_items_map[key] = _cache_items_list.begin();
    if (_cache_items_map.size() > _max_size) {
      _cache_items_map.erase(_cache_items_list.back().first);
      _cache_items_list.pop_back();
    }
  }

private: // data
  std::list<key_value_pair_t> _cache_items_list;
  std::map<key_t, list_iterator_t> _cache_items_map;
  size_t _max_size;
};

/**
   ConcurrentCache implements a concurrent thread-safe cache

   Compared to zim::lru_cache, each access operation is slightly more expensive.
   However, different slots of the cache can be safely accessed concurrently
   with minimal blocking. Concurrent access to the same element is also
   safe, and, in case of a cache miss, will block until that element becomes
   available.
 */
template <typename Key, typename Value>
class ConcurrentCache {
private: // types
  typedef std::shared_future<Value> ValuePlaceholder;
  typedef lru_cache<Key, ValuePlaceholder> Impl;

public: // types
  explicit ConcurrentCache(size_t maxEntries)
    : impl_(maxEntries)
  {}

  // Gets the entry corresponding to the given key. If the entry is not in the
  // cache, it is obtained by calling f() (without any arguments) and the
  // result is put into the cache.
  //
  // The cache as a whole is locked only for the duration of accessing
  // the respective slot. If, in the case of the a cache miss, the generation
  // of the missing element takes a long time, only attempts to access that
  // element will block - the rest of the cache remains open to concurrent
  // access.
  template<class F>
  Value getOrPut(const Key& key, F f) {
    std::promise<Value> valuePromise;
    std::unique_lock<std::mutex> l(lock_);
    const auto x = impl_.getOrPut(key, valuePromise.get_future().share());
    l.unlock();
    if ( x.miss() ) {
      try {
        valuePromise.set_value(f());
      } catch (std::exception& e) {
        drop(key);
        throw;
      }
    }

    return x.value().get();
  }

  bool drop(const Key& key) {
    std::unique_lock<std::mutex> l(lock_);
    return impl_.drop(key);
  }

private: // data
  Impl impl_;
  std::mutex lock_;
};

#endif  // LRUCACHE_H
