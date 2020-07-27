// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef GOOGLE_PROTOBUF_ARENASTRING_H__
#define GOOGLE_PROTOBUF_ARENASTRING_H__

#include <string>
#include <type_traits>
#include <utility>

#include <google/protobuf/stubs/logging.h>
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/stubs/fastmem.h>
#include <google/protobuf/arena.h>
#include <google/protobuf/port.h>

#include <google/protobuf/port_def.inc>

#ifdef SWIG
#error "You cannot SWIG proto headers"
#endif


// This is the implementation of arena string fields written for the open-source
// release. The ArenaStringPtr struct below is an internal implementation class
// and *should not be used* by user code. It is used to collect string
// operations together into one place and abstract away the underlying
// string-field pointer representation, so that (for example) an alternate
// implementation that knew more about ::std::string's internals could integrate
// more closely with the arena allocator.

namespace google {
namespace protobuf {
namespace internal {

struct ArenaString : public std::string {
 private:
  struct StrRep {
    const char* ptr;
    size_t len;
    size_t capacity;
  };
  static constexpr int kInternalCapacity = 15;

 public:
  typedef void InternalArenaConstructable_;
  typedef void DestructorSkippable_;

  ArenaString(const string& other) {
    GOOGLE_LOG(FATAL) << "should only construct ArenaString with an arena";
  }

  ArenaString(Arena* arena, const string& other) {
    Assign(other, arena);
  }

  bool Assign(const std::string& other, Arena* arena) {
    if (!arena) {
      assign(other);
      return false;
    }

    // TODO: handle case where moving between internal capacity
    // and external, need to drop reference so superclass doesn't
    // free.

    if (capacity() < other.size()) {
      char* buf = Arena::CreateArray<char>(arena, other.size());
      StrRep new_rep;
      new_rep.ptr = buf;
      new_rep.len = 0;
      new_rep.capacity = other.size();
      memcpy(this, &new_rep, sizeof(new_rep));
    }

    assign(other);
    return true;
  }

  std::string* MoveStorageToHeap() {
    if (capacity() != kInternalCapacity) {
      char* buf = new char[size()];
      memcpy(buf, data(), size());
      StrRep new_rep;
      new_rep.ptr = buf;
      new_rep.len = size();
      new_rep.capacity = size();
      memcpy(this, &new_rep, sizeof(new_rep));
    }
    return this;
  }
};

template<class T>
struct class_tag {
};

template<>
struct class_tag<std::string> {
  static constexpr bool value = false;
};

template<>
struct class_tag<ArenaString> {
  static constexpr bool value = true;
};

template <typename T>
class TaggedPtr {
 public:
  template<class U>
  void Set(U* p) {
    ptr_ = reinterpret_cast<uintptr_t>(static_cast<T*>(p)) | (uint8_t)class_tag<U>::value;
  }

  template<class U>
  U* As() const {
    GOOGLE_DCHECK(Is<U>());
    return reinterpret_cast<U*>(ptr_ & kPointerMask);
  }

  T* Get() const {
    return reinterpret_cast<T*>(ptr_ & kPointerMask);
  }

  bool IsNull() const { return (ptr_ & kPointerMask) == 0; }

  template<class U>
  bool Is() const {
    return tag() == class_tag<U>::value;
  }

  bool tag() const {
    return ptr_ & kTagMask;
  }
  
 private:

  static constexpr uintptr_t kTagMask = 1;
  static constexpr uintptr_t kPointerMask = ~kTagMask;

  // NOTE: important not to zero-initialize this, seems like the initialization
  // of the PB default instances relies on it not being zero-initted.
  uintptr_t ptr_;
};

// The 'string' itself is either on the heap, or an arena. This is determined by the
// 'arena' parameter. If 'arena' is non-null, the string must be on the arena.
//
// If the string instance is on the arena, the pointed-to buffer may either be on
// the arena or on the heap. The tag bit in the pointer is used to indicate this.
// a 'true' tag means the string buffer is on the arena.
struct PROTOBUF_EXPORT ArenaStringPtr {
  inline void Set(const ::std::string* default_value,
                  const ::std::string& value, Arena* arena) {
    if (IsDefault(default_value)) {
      CreateInstance(arena, &value);
      return;
    }
    if (ptr_.Is<std::string>()) {
      // TODO(todd): could switch from heap back to arenastring in this case.
      ptr_.As<std::string>()->assign(value);
    } else {
      ptr_.As<ArenaString>()->Assign(value, arena);
    }
  }

  inline void SetLite(const ::std::string* default_value,
                      const ::std::string& value, Arena* arena) {
    Set(default_value, value, arena);
  }

  // Basic accessors.
  inline const ::std::string& Get() const {
    return *ptr_.Get();
  }

  inline ::std::string* Mutable(const ::std::string* default_value,
                                Arena* arena) {
    if (IsDefault(default_value)) {
      CreateInstance<true>(arena, default_value);
      return ptr_.As<std::string>();
    } else if (ptr_.Is<ArenaString>()) {
    // If it's an Arena string, we need to switch it back to a std::string.
      ptr_.Set(ptr_.As<ArenaString>()->MoveStorageToHeap());
    }
    return ptr_.As<std::string>();
  }

  // Release returns a ::std::string* instance that is heap-allocated and is not
  // Own()'d by any arena. If the field was not set, it returns NULL. The caller
  // retains ownership. Clears this field back to NULL state. Used to implement
  // release_<field>() methods on generated classes.
  inline ::std::string* Release(const ::std::string* default_value,
                                Arena* arena) {
    if (IsDefault(default_value)) {
      return NULL;
    }
    return ReleaseNonDefault(default_value, arena);
  }

  // Similar to Release, but ptr_ cannot be the default_value.
  inline ::std::string* ReleaseNonDefault(const ::std::string* default_value,
                                          Arena* arena) {
    GOOGLE_DCHECK(!IsDefault(default_value));
    ::std::string* released = NULL;
    if (arena != NULL) {
      // ptr_ is owned by the arena. The storage may be on the arena or on the heap.
      if (ptr_.Is<std::string>()) {
        // Storage is on the heap, we can move from it.
        released = new ::std::string(std::move(*ptr_.As<std::string>()));
      } else {
        // Storage is in the arena, we need to copy from it.
        released = new ::std::string(*ptr_.As<ArenaString>());
      }
    } else {
      released = ptr_.As<std::string>();
    }
    ptr_.Set(const_cast< ::std::string*>(default_value));
    return released;
  }

  // UnsafeArenaRelease returns a ::std::string*, but it may be arena-owned
  // (i.e.  have its destructor already registered) if arena != NULL. If the
  // field was not set, this returns NULL. This method clears this field back to
  // NULL state. Used to implement unsafe_arena_release_<field>() methods on
  // generated classes.
  inline ::std::string* UnsafeArenaRelease(const ::std::string* default_value,
                                           Arena* /* arena */) {
    if (IsDefault(default_value)) {
      return NULL;
    }
    ::std::string* released;
    if (ptr_.Is<ArenaString>()) {
      // TODO is this necessary?
      released = ptr_.As<ArenaString>()->MoveStorageToHeap();
    } else {
      released = ptr_.As<std::string>();
    }
    ptr_.Set(const_cast< ::std::string*>(default_value));
    return released;
  }

  // Takes a string that is heap-allocated, and takes ownership. The string's
  // destructor is registered with the arena. Used to implement
  // set_allocated_<field> in generated classes.
  inline void SetAllocated(const ::std::string* default_value,
                           ::std::string* value, Arena* arena) {
    if (arena == NULL && !IsDefault(default_value)) {
      Destroy(default_value, arena);
    }
    if (value != NULL) {
      ptr_.Set(value);
      if (arena != NULL) {
        arena->Own(value);
      }
    } else {
      ptr_.Set(const_cast< ::std::string*>(default_value));
    }
  }

  // Takes a string that has lifetime equal to the arena's lifetime. The arena
  // must be non-null. It is safe only to pass this method a value returned by
  // UnsafeArenaRelease() on another field of a message in the same arena. Used
  // to implement unsafe_arena_set_allocated_<field> in generated classes.
  inline void UnsafeArenaSetAllocated(const ::std::string* default_value,
                                      ::std::string* value,
                                      Arena* /* arena */) {
    if (value != NULL) {
      ptr_.Set(value);
    } else {
      ptr_.Set(const_cast< ::std::string*>(default_value));
    }
  }

  // Swaps internal pointers. Arena-safety semantics: this is guarded by the
  // logic in Swap()/UnsafeArenaSwap() at the message level, so this method is
  // 'unsafe' if called directly.
  PROTOBUF_ALWAYS_INLINE void Swap(ArenaStringPtr* other) {
    std::swap(ptr_, other->ptr_);
  }
  PROTOBUF_ALWAYS_INLINE void Swap(ArenaStringPtr* other,
                                   const ::std::string* default_value,
                                   Arena* arena) {
#ifndef NDEBUG
    // For debug builds, we swap the contents of the string, rather than the
    // string instances themselves.  This invalidates previously taken const
    // references that are (per our documentation) invalidated by calling Swap()
    // on the message.
    //
    // If both strings are the default_value, swapping is uninteresting.
    // Otherwise, we use ArenaStringPtr::Mutable() to access the string, to
    // ensure that we do not try to mutate default_value itself.
    if (IsDefault(default_value) && other->IsDefault(default_value)) {
      return;
    }

    ::std::string* this_ptr = Mutable(default_value, arena);
    ::std::string* other_ptr = other->Mutable(default_value, arena);

    this_ptr->swap(*other_ptr);
#else
    std::swap(ptr_, other->ptr_);
    (void)default_value;
    (void)arena;
#endif
  }

  // Frees storage (if not on an arena).
  inline void Destroy(const ::std::string* default_value, Arena* arena) {
    if (arena == NULL) {
      if (!IsDefault(default_value)) {
        delete ptr_.As<std::string>();
      }
    } /* else if (ptr_.Is<std::string>()) {
      std::string* s = ptr_.As<std::string>();
      s->~string();
      } */
  }

  // Clears content, but keeps allocated string if arena != NULL, to avoid the
  // overhead of heap operations. After this returns, the content (as seen by
  // the user) will always be the empty string. Assumes that |default_value|
  // is an empty string.
  inline void ClearToEmpty(const ::std::string* default_value,
                           Arena* /* arena */) {
    if (IsDefault(default_value)) {
      // Already set to default (which is empty) -- do nothing.
    } else {
      ptr_.Get()->clear();
    }
  }

  // Clears content, assuming that the current value is not the empty string
  // default.
  inline void ClearNonDefaultToEmpty() { ptr_.Get()->clear(); }
  inline void ClearNonDefaultToEmptyNoArena() { ptr_.Get()->clear(); }

  // Clears content, but keeps allocated string if arena != NULL, to avoid the
  // overhead of heap operations. After this returns, the content (as seen by
  // the user) will always be equal to |default_value|.
  inline void ClearToDefault(const ::std::string* default_value,
                             Arena* /* arena */) {
    if (IsDefault(default_value)) {
      // Already set to default -- do nothing.
    } else {
      // Have another allocated string -- rather than throwing this away and
      // resetting ptr_ to the canonical default string instance, we just reuse
      // this instance.
      *ptr_.Get() = *default_value;
    }
  }

  // Called from generated code / reflection runtime only. Resets value to point
  // to a default string pointer, with the semantics that this ArenaStringPtr
  // does not own the pointed-to memory. Disregards initial value of ptr_ (so
  // this is the *ONLY* safe method to call after construction or when
  // reinitializing after becoming the active field in a oneof union).
  inline void UnsafeSetDefault(const ::std::string* default_value) {
    // Casting away 'const' is safe here: accessors ensure that ptr_ is only
    // returned as a const if it is equal to default_value.
    ptr_.Set(const_cast< ::std::string*>(default_value));
  }

  // The 'NoArena' variants of methods below assume arena == NULL and are
  // optimized to provide very little overhead relative to a raw string pointer
  // (while still being in-memory compatible with other code that assumes
  // ArenaStringPtr). Note the invariant that a class instance that has only
  // ever been mutated by NoArena methods must *only* be in the String state
  // (i.e., tag bits are not used), *NEVER* ArenaString. This allows all
  // tagged-pointer manipulations to be avoided.
  inline void SetNoArena(const ::std::string* default_value,
                         const ::std::string& value) {
    if (IsDefault(default_value)) {
      CreateInstanceNoArena(&value);
    } else {
      *ptr_.Get() = value;
    }
  }

  void SetNoArena(const ::std::string* default_value, ::std::string&& value) {
    if (IsDefault(default_value)) {
      ptr_.Set(new ::std::string(std::move(value)));
    } else {
      ptr_.As<std::string>()->assign(std::move(value));
    }
  }

  void AssignWithDefault(const ::std::string* default_value,
                         ArenaStringPtr value);

  inline const ::std::string& GetNoArena() const { return *ptr_.As<std::string>(); }

  inline ::std::string* MutableNoArena(const ::std::string* default_value) {
    if (IsDefault(default_value)) {
      CreateInstanceNoArena(default_value);
    }
    return ptr_.As<std::string>();
  }

  inline ::std::string* ReleaseNoArena(const ::std::string* default_value) {
    if (IsDefault(default_value)) {
      return NULL;
    } else {
      return ReleaseNonDefaultNoArena(default_value);
    }
  }

  inline ::std::string* ReleaseNonDefaultNoArena(
      const ::std::string* default_value) {
    GOOGLE_DCHECK(!IsDefault(default_value));
    ::std::string* released = ptr_.As<std::string>();
    ptr_.Set(const_cast< ::std::string*>(default_value));
    return released;
  }

  inline void SetAllocatedNoArena(const ::std::string* default_value,
                                  ::std::string* value) {
    if (!IsDefault(default_value)) {
      delete ptr_.As<std::string>();
    }
    if (value != NULL) {
      ptr_.Set(value);
    } else {
      ptr_.Set(const_cast< ::std::string*>(default_value));
    }
  }

  inline void DestroyNoArena(const ::std::string* default_value) {
    if (!IsDefault(default_value)) {
      delete ptr_.As<std::string>();
    }
  }

  inline void ClearToEmptyNoArena(const ::std::string* default_value) {
    if (IsDefault(default_value)) {
      // Nothing: already equal to default (which is the empty string).
    } else {
      ptr_.As<std::string>()->clear();
    }
  }

  inline void ClearToDefaultNoArena(const ::std::string* default_value) {
    if (IsDefault(default_value)) {
      // Nothing: already set to default.
    } else {
      // Reuse existing allocated instance.
      *ptr_.As<std::string>() = *default_value;
    }
  }

  inline bool IsDefault(const ::std::string* default_value) const {
    return ptr_.Is<std::string>() && ptr_.As<std::string>() == default_value;
  }

  // Internal accessors!!!!
  void UnsafeSetTaggedPointer(TaggedPtr< ::std::string> value) {
    ptr_ = value;
  }
  // Generated code only! An optimization, in certain cases the generated
  // code is certain we can obtain a string with no default checks and
  // tag tests.
  ::std::string* UnsafeMutablePointer() { return ptr_.As<std::string>(); }

 private:
  TaggedPtr<std::string> ptr_;

  template<bool FORCE_STD_STRING=false>
  PROTOBUF_NOINLINE
  void CreateInstance(Arena* arena, const ::std::string* initial_value) {
    GOOGLE_DCHECK(initial_value != NULL);
    if (arena && !FORCE_STD_STRING) {
      ptr_.Set(Arena::CreateMessage<ArenaString>(arena, *initial_value));
    } else {
      ptr_.Set(Arena::Create<std::string>(arena, *initial_value));
    }
  }
  PROTOBUF_NOINLINE
  void CreateInstanceNoArena(const ::std::string* initial_value) {
    GOOGLE_DCHECK(initial_value != NULL);
    ptr_.Set(new ::std::string(*initial_value));
  }
};

}  // namespace internal
}  // namespace protobuf

namespace protobuf {
namespace internal {

inline void ArenaStringPtr::AssignWithDefault(
    const ::std::string* default_value, ArenaStringPtr value) {
  const ::std::string* me = &Get();
  const ::std::string* other = &value.Get();
  // If the pointers are the same then do nothing.
  if (me != other) {
    SetNoArena(default_value, value.GetNoArena());
  }
}

}  // namespace internal
}  // namespace protobuf
}  // namespace google


#include <google/protobuf/port_undef.inc>

#endif  // GOOGLE_PROTOBUF_ARENASTRING_H__
