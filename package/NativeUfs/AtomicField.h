// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Scalar snapshots are also read from IRQ and panic contexts.
template <class T, bool Publish = false> struct UfsAtomicField {
  constexpr UfsAtomicField (T initial = {}) : value (initial) {}
  UfsAtomicField (const UfsAtomicField &other) : value (other.load ()) {}
  T load () const {
    return __atomic_load_n (&value, Publish ? __ATOMIC_ACQUIRE : __ATOMIC_RELAXED);
  }
  operator T () const { return load (); }
  T operator= (T next) {
    __atomic_store_n (&value, next, Publish ? __ATOMIC_RELEASE : __ATOMIC_RELAXED);
    return next;
  }
  T operator= (const UfsAtomicField &other) { return *this = other.load (); }
  void increment () { __atomic_add_fetch (&value, 1, __ATOMIC_RELAXED); }
private:
  T value;
};
