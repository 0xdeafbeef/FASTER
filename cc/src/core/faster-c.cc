// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.


#include "faster.h"
#include "faster-c.h"
#include "device/file_system_disk.h"
#include "device/null_disk.h"

extern "C" {

  class Key {
   public:
    Key(uint64_t key)
      : key_{ key } {
    }

    /// Methods and operators required by the (implicit) interface:
    inline static constexpr uint32_t size() {
      return static_cast<uint32_t>(sizeof(Key));
    }
    inline KeyHash GetHash() const {
      return KeyHash{ Utility::GetHashCode(key_) };
    }

    /// Comparison operators.
    inline bool operator==(const Key& other) const {
      return key_ == other.key_;
    }
    inline bool operator!=(const Key& other) const {
      return key_ != other.key_;
    }

   private:
    uint64_t key_;
  };

  class Value {
   public:
    Value()
      : value_{ 0 } {
    }

    Value(const Value& other)
      : value_{ other.value_ } {
    }

    Value(uint64_t value)
      : value_{ value } {
    }

    inline static constexpr uint32_t size() {
      return static_cast<uint32_t>(sizeof(Value));
    }

    friend class ReadContext;
    friend class UpsertContext;
    friend class RmwContext;

   private:
    union {
      uint64_t value_;
      std::atomic<uint64_t> atomic_value_;
    };
  };

  class ReadContext : public IAsyncContext {
   public:
    typedef Key key_t;
    typedef Value value_t;

    ReadContext(uint64_t key)
      : key_{ key } {
    }

    /// Copy (and deep-copy) constructor.
    ReadContext(const ReadContext& other)
      : key_{ other.key_ } {
    }

    /// The implicit and explicit interfaces require a key() accessor.
    inline const Key& key() const {
      return key_;
    }

    inline void Get(const value_t& value) { }
    inline void GetAtomic(const value_t& value) { }

   protected:
    /// The explicit interface requires a DeepCopy_Internal() implementation.
    Status DeepCopy_Internal(IAsyncContext*& context_copy) {
      return IAsyncContext::DeepCopy_Internal(*this, context_copy);
    }

   private:
    Key key_;
  };

  class UpsertContext : public IAsyncContext {
   public:
    typedef Key key_t;
    typedef Value value_t;

    UpsertContext(uint64_t key, uint64_t input)
      : key_{ key }
      , input_{ input } {
    }

    /// Copy (and deep-copy) constructor.
    UpsertContext(const UpsertContext& other)
      : key_{ other.key_ }
      , input_{ other.input_ } {
    }

    /// The implicit and explicit interfaces require a key() accessor.
    inline const Key& key() const {
      return key_;
    }
    inline static constexpr uint32_t value_size() {
      return sizeof(value_t);
    }

    /// Non-atomic and atomic Put() methods.
    inline void Put(value_t& value) {
      value.value_ = input_;
    }
    inline bool PutAtomic(value_t& value) {
      value.atomic_value_.store(input_);
      return true;
    }

   protected:
    /// The explicit interface requires a DeepCopy_Internal() implementation.
    Status DeepCopy_Internal(IAsyncContext*& context_copy) {
      return IAsyncContext::DeepCopy_Internal(*this, context_copy);
    }

   private:
    Key key_;
    uint64_t input_;
  };

  
  typedef FASTER::environment::QueueIoHandler handler_t;
  typedef FASTER::device::FileSystemDisk<handler_t, 1073741824L> disk_t;
  typedef FASTER::device::NullDisk  disk_null_t;
  using store_t = FasterKv<Key, Value, disk_t>;

  struct faster_t { store_t* obj; };

  faster_t* faster_open_with_disk(const char* storage, const size_t key_space) {
    faster_t* res = new faster_t;
    std::experimental::filesystem::create_directory(storage);
    res->obj= new store_t { key_space, 17179869184, storage };
    return res;
  }

  void faster_upsert(faster_t* faster_t) {
    store_t* store = faster_t->obj;
    uint64_t value = 42;
    uint64_t key = 10;

    store->StartSession();

    auto callback = [](IAsyncContext* ctxt, Status result) {
        assert(result == Status::Ok);
    };
    UpsertContext context{ key, value };
    store->Upsert(context, callback, 1);

    store->CompletePending(true);
    store->StopSession();
  }

  char* faster_read(faster_t* faster_t, const char* key) {
    store_t* store = faster_t->obj;

    auto callback = [](IAsyncContext* ctxt, Status result) {
      CallbackContext<ReadContext> context{ ctxt };
    };

    uint64_t key_ = 10;
    ReadContext context{ key_};
    Status result = store->Read(context, callback, 1);

    if (result == Status::Ok) {
      printf("OK\n");
    } else if (result == Status::NotFound) {
      printf("Not Found\n");
    }

    char* s = "hej";
    return s;
  }

  void faster_destroy(faster_t *faster_t) {
    if (faster_t == NULL)
      return;

    delete faster_t->obj;
    delete faster_t;
  }


} // extern "C"
