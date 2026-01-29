// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <unordered_set>

#include "faster.h"
#include "faster-c.h"
#include "device/file_system_disk.h"
#include "device/null_disk.h"

namespace {
thread_local std::unordered_set<faster_t*> active_sessions;

static inline uint8_t handle_exception(const char* op, read_callback cb, void* target) {
  try {
    throw;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fckup faster ffi exception op=%s what=%s\n", op, e.what());
  } catch (...) {
    std::fprintf(stderr, "fckup faster ffi exception op=%s unknown\n", op);
  }
  if (cb != nullptr) {
    cb(target, NULL, 0, Aborted);
  }
  return static_cast<uint8_t>(Aborted);
}
}  // namespace

extern "C" {

  using namespace FASTER::core;
  void deallocate_vec(uint8_t*, uint64_t);
  uint8_t* faster_alloc_vec(uint64_t);
  faster_checkpoint_result* faster_alloc_checkpoint_result();
  faster_recover_result* faster_alloc_recover_result();

  class Key {
    public:
      Key(const uint8_t* key, const uint64_t key_length)
        : temp_buffer_{ key }
        , key_length_{ key_length } {
      }

      Key(const Key& other) {
        key_length_ = other.key_length_;
        temp_buffer_ = NULL;
        if (other.temp_buffer_ == NULL) {
          memcpy(buffer(), other.buffer(), key_length_);
        } else {
          memcpy(buffer(), other.temp_buffer_, key_length_);
        }
      }

      ~Key() {
        if (this->temp_buffer_ != NULL) {
          deallocate_vec((uint8_t*) this->temp_buffer_, this->key_length_);
        }
      }

      /// Methods and operators required by the (implicit) interface:
      inline uint32_t size() const {
        return static_cast<uint32_t>(sizeof(Key) + key_length_);
      }
      inline KeyHash GetHash() const {
        if (this->temp_buffer_ != NULL) {
          return KeyHash(Utility::FasterHash::compute(temp_buffer_, key_length_));
        }
        return KeyHash(Utility::FasterHash::compute(buffer(), key_length_));
      }

      /// Comparison operators.
      inline bool operator==(const Key& other) const {
        if (this->key_length_ != other.key_length_) return false;
        const uint8_t* self_buffer = this->temp_buffer_ == NULL ? buffer() : this->temp_buffer_;
        const uint8_t* other_buffer = other.temp_buffer_ == NULL ? other.buffer() : other.temp_buffer_;
        return memcmp(self_buffer, other_buffer, key_length_) == 0;
      }
      inline bool operator!=(const Key& other) const {
        if (this->key_length_ != other.key_length_) return true;
        const uint8_t* self_buffer = this->temp_buffer_ == NULL ? buffer() : this->temp_buffer_;
        const uint8_t* other_buffer = other.temp_buffer_ == NULL ? other.buffer() : other.temp_buffer_;
        return memcmp(self_buffer, other_buffer, key_length_) != 0;
      }

    private:
      uint64_t key_length_;
      const uint8_t* temp_buffer_;

      inline const uint8_t* buffer() const {
        return reinterpret_cast<const uint8_t*>(this + 1);
      }
      inline uint8_t* buffer() {
        return reinterpret_cast<uint8_t*>(this + 1);
      }
  };

  class ReadContext;
  class UpsertContext;
  class RmwContext;

  class GenLock {
  public:
    GenLock()
      : control_{ 0 } {
    }
    GenLock(uint64_t control)
      : control_{ control } {
    }
    inline GenLock& operator=(const GenLock& other) {
      control_ = other.control_;
      return *this;
    }

    union {
      struct {
        uint64_t gen_number : 62;
        uint64_t locked : 1;
        uint64_t replaced : 1;
      };
      uint64_t control_;
    };
  };

  class AtomicGenLock {
  public:
    AtomicGenLock()
      : control_{ 0 } {
    }
    AtomicGenLock(uint64_t control)
      : control_{ control } {
    }

    inline GenLock load() const {
      return GenLock{ control_.load() };
    }
    inline void store(GenLock desired) {
      control_.store(desired.control_);
    }

    inline bool try_lock(bool& replaced) {
      replaced = false;
      GenLock expected{ control_.load() };
      expected.locked = 0;
      expected.replaced = 0;
      GenLock desired{ expected.control_ };
      desired.locked = 1;

      if(control_.compare_exchange_strong(expected.control_, desired.control_)) {
        return true;
      }
      if(expected.replaced) {
        replaced = true;
      }
      return false;
    }
    inline void unlock(bool replaced) {
      if(!replaced) {
        // Just turn off "locked" bit and increase gen number.
        uint64_t sub_delta = ((uint64_t)1 << 62) - 1;
        control_.fetch_sub(sub_delta);
      } else {
        // Turn off "locked" bit, turn on "replaced" bit, and increase gen number
        uint64_t add_delta = ((uint64_t)1 << 63) - ((uint64_t)1 << 62) + 1;
        control_.fetch_add(add_delta);
      }
    }

  private:
    std::atomic<uint64_t> control_;
  };

  class Value {
  public:
    Value()
      : gen_lock_{ 0 }
      , size_{ 0 }
      , length_{ 0 } {
    }

    inline uint32_t size() const {
      return size_;
    }

    friend class ReadContext;
    friend class UpsertContext;
    friend class RmwContext;

  private:
    AtomicGenLock gen_lock_;
    uint64_t size_;
    uint64_t length_;

    inline const uint8_t* buffer() const {
      return reinterpret_cast<const uint8_t*>(this + 1);
    }
    inline uint8_t* buffer() {
      return reinterpret_cast<uint8_t*>(this + 1);
    }
  };

  class ReadContext : public IAsyncContext {
  public:
    typedef Key key_t;
    typedef Value value_t;

    ReadContext(const uint8_t* key, uint64_t key_length, read_callback cb, void* target)
      : cb_ { cb }
      , target_ { target }
      , key_{ key, key_length }  {
    }

    /// Copy (and deep-copy) constructor.
    ReadContext(const ReadContext& other)
      : cb_ { other.cb_ }
      , target_ { other.target_ }
      , key_{ other.key_ }  {
    }

    /// The implicit and explicit interfaces require a key() accessor.
    inline const Key& key() const {
      return key_;
    }

    inline void Get(const Value& value) {
      cb_(target_, value.buffer(), value.length_, Ok);
    }
    inline void GetAtomic(const Value& value) {
      GenLock before, after;
      uint8_t* buffer = NULL;
      uint64_t length = 0;
      do {
        before = value.gen_lock_.load();
        buffer = (uint8_t*) realloc(buffer, value.length_);
        memcpy(buffer, value.buffer(), value.length_);
        length = value.length_;
        after = value.gen_lock_.load();
      } while(before.gen_number != after.gen_number);
      cb_(target_, buffer, length, Ok);
      free(buffer);
    }

    /// For async reads returning not found
    inline void ReturnNotFound() {
      cb_(target_, NULL, 0, NotFound);
    }

  protected:
    /// The explicit interface requires a DeepCopy_Internal() implementation.
    Status DeepCopy_Internal(IAsyncContext*& context_copy) {
      context_copy = nullptr;
      size_t extra = key_.size() - sizeof(Key);
      auto ctxt = alloc_context<ReadContext>(sizeof(ReadContext) + extra);
      if(!ctxt.get()) return Status::OutOfMemory;
      new(ctxt.get()) ReadContext{ *this };
      context_copy = ctxt.release();
      return Status::Ok;
    }

  private:
    read_callback cb_;
    void* target_;
    Key key_;
  };

  class UpsertContext : public IAsyncContext {
  public:
    typedef Key key_t;
    typedef Value value_t;

    UpsertContext(const uint8_t* key, uint64_t key_length, uint8_t* input, uint64_t length)
      : input_{ input }
      , length_{ length }
      , key_{ key, key_length } {
    }

    /// Copy (and deep-copy) constructor.
    UpsertContext(UpsertContext& other)
      : input_{ other.input_ }
      , length_{ other.length_ }
      , key_{ other.key_ } {
      other.input_ = NULL;
    }

    ~UpsertContext() {
      if (input_ != NULL) {
        deallocate_vec(input_, length_);
      }
    }

    /// The implicit and explicit interfaces require a key() accessor.
    inline const Key& key() const {
      return key_;
    }
    inline uint32_t value_size() const {
      return sizeof(Value) + length_;
    }
    /// Non-atomic and atomic Put() methods.
    inline void Put(Value& value) {
      value.gen_lock_.store(0);
      value.size_ = sizeof(Value) + length_;
      value.length_ = length_;
      std::memcpy(value.buffer(), input_, length_);
    }
    inline bool PutAtomic(Value& value) {
      bool replaced;
      while(!value.gen_lock_.try_lock(replaced) && !replaced) {
        std::this_thread::yield();
      }
      if(replaced) {
        // Some other thread replaced this record.
        return false;
      }
      if(value.size_ < sizeof(Value) + length_) {
        // Current value is too small for in-place update.
        value.gen_lock_.unlock(true);
        return false;
      }
      // In-place update overwrites length and buffer, but not size.
      value.length_ = length_;
      std::memcpy(value.buffer(), input_, length_);
      value.gen_lock_.unlock(false);
      return true;
    }

  protected:
    /// The explicit interface requires a DeepCopy_Internal() implementation.
    Status DeepCopy_Internal(IAsyncContext*& context_copy) {
      context_copy = nullptr;
      size_t extra = key_.size() - sizeof(Key);
      auto ctxt = alloc_context<UpsertContext>(sizeof(UpsertContext) + extra);
      if(!ctxt.get()) return Status::OutOfMemory;
      new(ctxt.get()) UpsertContext{ *this };
      context_copy = ctxt.release();
      return Status::Ok;
    }

  private:
    uint8_t* input_;
    uint64_t length_;
    key_t key_;
  };

  class RmwContext : public IAsyncContext {
  public:
    typedef Key key_t;
    typedef Value value_t;

    RmwContext(const uint8_t* key, uint64_t key_length, uint8_t* modification, uint64_t length, rmw_callback cb)
      : modification_{ modification }
      , length_{ length }
      , cb_{ cb }
      , new_length_{ 0 }
      , key_{ key, key_length }{
    }

    /// Copy (and deep-copy) constructor.
    RmwContext(RmwContext& other)
      : modification_{ other.modification_ }
      , length_{ other.length_ }
      , cb_{ other.cb_ }
      , new_length_{ other.new_length_ }
      , key_{ other.key_ }{
      other.modification_ = NULL;
    }

    ~RmwContext() {
      if (modification_ != NULL) {
        deallocate_vec(modification_, length_);
      }
    }

    /// The implicit and explicit interfaces require a key() accessor.
    inline const Key& key() const {
      return key_;
    }
    inline uint32_t value_size() const {
      return sizeof(Value) + length_;
    }
    inline uint32_t value_size(const Value& old_value) const {
      if (new_length_ == 0) {
        new_length_ = cb_(old_value.buffer(), old_value.length_, modification_, length_, NULL);
      }
      return sizeof(Value) + new_length_;
    }

    inline void RmwInitial(Value& value) {
      value.gen_lock_.store(0);
      value.size_ = sizeof(Value) + length_;
      value.length_ = length_;
      std::memcpy(value.buffer(), modification_, length_);
    }
    inline void RmwCopy(const Value& old_value, Value& value) {
      value.gen_lock_.store(0);
      value.length_ = cb_(old_value.buffer(), old_value.length_, modification_, length_, value.buffer());
      value.size_ = sizeof(Value) + value.length_;
    }
    inline bool RmwAtomic(Value& value) {
      bool replaced;
      while(!value.gen_lock_.try_lock(replaced) && !replaced) {
        std::this_thread::yield();
      }
      if(replaced) {
        // Some other thread replaced this record.
        return false;
      }
      if (new_length_ == 0) {
        new_length_ = cb_(value.buffer(), value.length_, modification_, length_, NULL);
      }
      if(value.size_ < sizeof(Value) + new_length_) {
        // Current value is too small for in-place update.
        value.gen_lock_.unlock(true);
        return false;
      }
      // In-place update overwrites length and buffer, but not size.
      cb_(value.buffer(), value.length_, modification_, length_, value.buffer());
      value.length_ = new_length_;
      value.gen_lock_.unlock(false);
      return true;
    }

  protected:
    /// The explicit interface requires a DeepCopy_Internal() implementation.
    Status DeepCopy_Internal(IAsyncContext*& context_copy) {
      context_copy = nullptr;
      size_t extra = key_.size() - sizeof(Key);
      auto ctxt = alloc_context<RmwContext>(sizeof(RmwContext) + extra);
      if(!ctxt.get()) return Status::OutOfMemory;
      new(ctxt.get()) RmwContext{ *this };
      context_copy = ctxt.release();
      return Status::Ok;
    }

  private:
    uint8_t* modification_;
    uint64_t length_;
    rmw_callback cb_;
    mutable uint64_t new_length_;
    Key key_;
  };

  class DeleteContext : public IAsyncContext {
  public:
      typedef Key key_t;
      typedef Value value_t;

      DeleteContext(const uint8_t* key, uint64_t key_length)
      : key_{ key, key_length } {

      }

      /// Copy (and deep-copy) constructor.
      DeleteContext(DeleteContext& other)
      : key_ { other.key_ } {
      }

      /// The implicit and explicit interfaces require a key() accessor.
      inline const Key& key() const {
        return key_;
      }
      inline uint32_t value_size() const {
        return sizeof(value_t);
      }

  protected:
      /// The explicit interface requires a DeepCopy_Internal() implementation.
      Status DeepCopy_Internal(IAsyncContext*& context_copy) {
        context_copy = nullptr;
        size_t extra = key_.size() - sizeof(Key);
        auto ctxt = alloc_context<DeleteContext>(sizeof(DeleteContext) + extra);
        if(!ctxt.get()) return Status::OutOfMemory;
        new(ctxt.get()) DeleteContext{ *this };
        context_copy = ctxt.release();
        return Status::Ok;
      }

  private:
      key_t key_;
  };

  enum store_type {
      NULL_DISK,
      FILESYSTEM_DISK,
  };
  typedef enum store_type store_type;

  typedef FASTER::environment::QueueIoHandler handler_t;
  typedef FASTER::device::FileSystemDisk<handler_t, 1073741824L> disk_t;
  typedef FASTER::device::NullDisk  disk_null_t;
  using store_t = FasterKv<Key, Value, disk_t>;
  using null_store_t = FasterKv<Key, Value, disk_null_t>;
  struct faster_t {
      union {
          store_t* store;
          null_store_t* null_store;
      } obj;
      store_type type;
  };

  static bool start_session_if_needed(faster_t* faster_t) {
    if (faster_t == NULL) {
      return false;
    }
    if (active_sessions.find(faster_t) != active_sessions.end()) {
      return false;
    }

    switch (faster_t->type) {
      case NULL_DISK:
        faster_t->obj.null_store->StartSession();
        break;
      case FILESYSTEM_DISK:
        faster_t->obj.store->StartSession();
        break;
    }

    active_sessions.insert(faster_t);
    return true;
  }

  static void stop_session_if_started(faster_t* faster_t, bool started) {
    if (!started || faster_t == NULL) {
      return;
    }

    switch (faster_t->type) {
      case NULL_DISK:
        faster_t->obj.null_store->StopSession();
        break;
      case FILESYSTEM_DISK:
        faster_t->obj.store->StopSession();
        break;
    }

    active_sessions.erase(faster_t);
  }

  faster_t* faster_open(const uint64_t table_size, const uint64_t log_size, bool pre_allocate_log = false) {
    try {
      faster_t* res = new faster_t();
      res->obj.null_store = new null_store_t{
        null_store_t::IndexConfig{ table_size },
        log_size,
        "",
        1.0,
        DEFAULT_READ_CACHE_CONFIG,
        DEFAULT_HLOG_COMPACTION_CONFIG,
        pre_allocate_log,
      };
      res->type = NULL_DISK;
      return res;
    } catch (...) {
      handle_exception("faster_open", nullptr, nullptr);
      return NULL;
    }
  }

  faster_t* faster_open_with_disk(const uint64_t table_size, const uint64_t log_size,
                                  const char* storage, double log_mutable_fraction = 0.9, bool pre_allocate_log = false) {
    try {
      faster_t* res = new faster_t();
      std::filesystem::create_directories(storage);
      res->obj.store = new store_t{
        store_t::IndexConfig{ table_size },
        log_size,
        storage,
        log_mutable_fraction,
        DEFAULT_READ_CACHE_CONFIG,
        DEFAULT_HLOG_COMPACTION_CONFIG,
        pre_allocate_log,
      };
      res->type = FILESYSTEM_DISK;
      return res;
    } catch (...) {
      handle_exception("faster_open_with_disk", nullptr, nullptr);
      return NULL;
    }
  }

  uint8_t faster_upsert(faster_t* faster_t, const uint8_t* key, const uint64_t key_length,
                        uint8_t* value, uint64_t value_length, const uint64_t monotonic_serial_number) {
    try {
      auto callback = [](IAsyncContext* ctxt, Status result) {
        assert(result == Status::Ok);
      };

      UpsertContext context { key, key_length, value, value_length };
      Status result;
      switch (faster_t->type) {
        case NULL_DISK:
          result = faster_t->obj.null_store->Upsert(context, callback, monotonic_serial_number);
          break;
        case FILESYSTEM_DISK:
          result = faster_t->obj.store->Upsert(context, callback, monotonic_serial_number);
          break;
      }
      return static_cast<uint8_t>(result);
    } catch (...) {
      return handle_exception("faster_upsert", nullptr, nullptr);
    }
  }

  uint8_t faster_rmw(faster_t* faster_t, const uint8_t* key, const uint64_t key_length, uint8_t* modification,
                     const uint64_t length, const uint64_t monotonic_serial_number, rmw_callback cb) {
    try {
      auto callback = [](IAsyncContext* ctxt, Status result) {
        CallbackContext<RmwContext> context { ctxt };
      };

      RmwContext context{ key, key_length, modification, length, cb};
      Status result;
      switch (faster_t->type) {
        case NULL_DISK:
          result = faster_t->obj.null_store->Rmw(context, callback, monotonic_serial_number);
          break;
        case FILESYSTEM_DISK:
          result = faster_t->obj.store->Rmw(context, callback, monotonic_serial_number);
          break;
      }
      return static_cast<uint8_t>(result);
    } catch (...) {
      return handle_exception("faster_rmw", nullptr, nullptr);
    }
  }

  uint8_t faster_read(faster_t* faster_t, const uint8_t* key, const uint64_t key_length,
                       const uint64_t monotonic_serial_number, read_callback cb, void* target) {
    try {
      auto callback = [](IAsyncContext* ctxt, Status result) {
        CallbackContext<ReadContext> context { ctxt };
        if (result == Status::NotFound) {
          context->ReturnNotFound();
        }
      };

      ReadContext context {key, key_length, cb, target};
      Status result;
      switch (faster_t->type) {
        case NULL_DISK:
          result = faster_t->obj.null_store->Read(context, callback, monotonic_serial_number);
          break;
        case FILESYSTEM_DISK:
          result = faster_t->obj.store->Read(context, callback, monotonic_serial_number);
          break;
      }

      return static_cast<uint8_t>(result);
    } catch (...) {
      return handle_exception("faster_read", cb, target);
    }
  }

  uint8_t faster_delete(faster_t* faster_t, const uint8_t* key, const uint64_t key_length,
                        const uint64_t monotonic_serial_number) {
    try {
      auto callback = [](IAsyncContext* ctxt, Status result) {
        CallbackContext<DeleteContext> context { ctxt };
        assert(result == Status::Ok || result == Status::NotFound);
      };

      DeleteContext context {key, key_length};
      Status result;
      switch (faster_t->type) {
        case NULL_DISK:
          result = faster_t->obj.null_store->Delete(context, callback, monotonic_serial_number);
          break;
        case FILESYSTEM_DISK:
          result = faster_t->obj.store->Delete(context, callback, monotonic_serial_number);
          break;
      }
      return static_cast<uint8_t>(result);
    } catch (...) {
      return handle_exception("faster_delete", nullptr, nullptr);
    }
  }

  // It is up to the caller to dealloc faster_checkpoint_result*
  // first token, then struct
  faster_checkpoint_result* faster_checkpoint(faster_t* faster_t) {
    if (faster_t == NULL) {
      return NULL;
    }

    auto hybrid_log_persistence_callback = [](Status result, uint64_t persistent_serial_num) {
      assert(result == Status::Ok);
    };

    Guid token;
    bool checked;
    bool started = start_session_if_needed(faster_t);
    switch (faster_t->type) {
      case NULL_DISK:
        checked = faster_t->obj.null_store->Checkpoint(nullptr, hybrid_log_persistence_callback, token);
        faster_t->obj.null_store->CompletePending(true);
        break;
      case FILESYSTEM_DISK:
        checked = faster_t->obj.store->Checkpoint(nullptr, hybrid_log_persistence_callback, token);
        faster_t->obj.store->CompletePending(true);
        break;
    }
    stop_session_if_started(faster_t, started);
    faster_checkpoint_result* res = faster_alloc_checkpoint_result();
    res->checked = checked;
    res->token = (char*) faster_alloc_vec(37);
    strncpy(res->token, token.ToString().c_str(), 37);
    return res;
  }

  // It is up to the caller to dealloc faster_checkpoint_result*
  // first token, then struct
  faster_checkpoint_result* faster_checkpoint_index(faster_t* faster_t) {
    if (faster_t == NULL) {
      return NULL;
    }

    auto index_persistence_callback = [](Status result) {
        assert(result == Status::Ok);
    };

    Guid token;
    bool checked;
    bool started = start_session_if_needed(faster_t);
    switch (faster_t->type) {
      case NULL_DISK:
        checked = faster_t->obj.null_store->CheckpointIndex(index_persistence_callback, token);
        faster_t->obj.null_store->CompletePending(true);
        break;
      case FILESYSTEM_DISK:
        checked = faster_t->obj.store->CheckpointIndex(index_persistence_callback, token);
        faster_t->obj.store->CompletePending(true);
        break;
    }
    stop_session_if_started(faster_t, started);
    faster_checkpoint_result* res = faster_alloc_checkpoint_result();
    res->checked = checked;
    res->token = (char*) faster_alloc_vec(37);
    strncpy(res->token, token.ToString().c_str(), 37);
    return res;
  }

  // It is up to the caller to dealloc faster_checkpoint_result*
  // first token, then struct
  faster_checkpoint_result* faster_checkpoint_hybrid_log(faster_t* faster_t) {
    if (faster_t == NULL) {
      return NULL;
    }

    auto hybrid_log_persistence_callback = [](Status result, uint64_t persistent_serial_num) {
        assert(result == Status::Ok);
    };

    Guid token;
    bool checked;
    bool started = start_session_if_needed(faster_t);
    switch (faster_t->type) {
      case NULL_DISK:
        checked = faster_t->obj.null_store->CheckpointHybridLog(hybrid_log_persistence_callback, token);
        faster_t->obj.null_store->CompletePending(true);
        break;
      case FILESYSTEM_DISK:
        checked = faster_t->obj.store->CheckpointHybridLog(hybrid_log_persistence_callback, token);
        faster_t->obj.store->CompletePending(true);
        break;
    }
    stop_session_if_started(faster_t, started);
    faster_checkpoint_result* res = faster_alloc_checkpoint_result();
    res->checked = checked;
    res->token = (char*) faster_alloc_vec(37);
    strncpy(res->token, token.ToString().c_str(), 37);
    return res;
  }

  void faster_destroy(faster_t *faster_t) {
    if (faster_t == NULL)
      return;

    active_sessions.erase(faster_t);

    switch (faster_t->type) {
      case NULL_DISK:
        delete faster_t->obj.null_store;
        break;
      case FILESYSTEM_DISK:
        delete faster_t->obj.store;
        break;
    }
    delete faster_t;
  }

  uint64_t faster_size(faster_t* faster_t) {
    if (faster_t == NULL) {
      return -1;
    } else {
      switch (faster_t->type) {
        case NULL_DISK:
          return faster_t->obj.null_store->Size();
        case FILESYSTEM_DISK:
          return faster_t->obj.store->Size();
      }
    }
  }

  // It is up to the caller to deallocate the faster_recover_result* struct
  faster_recover_result* faster_recover(faster_t* faster_t, const char* index_token, const char* hybrid_log_token) {
    if (faster_t == NULL) {
      return NULL;
    } else {
      uint32_t ver;
      std::vector<Guid> _session_ids;

      std::string index_str(index_token);
      std::string hybrid_str(hybrid_log_token);
      //TODO: error handling
      Guid index_guid = Guid::Parse(index_str);
      Guid hybrid_guid = Guid::Parse(hybrid_str);
      Status sres;
      switch (faster_t->type) {
        case NULL_DISK:
          sres = faster_t->obj.null_store->Recover(index_guid, hybrid_guid, ver, _session_ids);
          break;
        case FILESYSTEM_DISK:
          sres = faster_t->obj.store->Recover(index_guid, hybrid_guid, ver, _session_ids);
          break;
      }

      uint8_t status_result = static_cast<uint8_t>(sres);
      faster_recover_result* res = faster_alloc_recover_result();
      res->status= status_result;
      res->version = ver;

      int ids_total = _session_ids.size();
      res->session_ids_count = ids_total;
      int session_len = 37; // 36 + 1
      res->session_ids = (char*) faster_alloc_vec(sizeof(char) * ids_total * session_len);

      int counter = 0;
      for (auto& id : _session_ids) {
        strncpy(res->session_ids + counter * session_len * sizeof(char), id.ToString().c_str(), session_len);
        counter++;
      }

      return res;
    }
  }

  void faster_complete_pending(faster_t* faster_t, bool b) {
    if (faster_t != NULL) {
      switch (faster_t->type) {
        case NULL_DISK:
          faster_t->obj.null_store->CompletePending(b);
          break;
        case FILESYSTEM_DISK:
          faster_t->obj.store->CompletePending(b);
          break;
      }
    }
  }

  // Thread-related

  const char* faster_start_session(faster_t* faster_t) {
    if (faster_t == NULL) {
      return NULL;
    } else {
      Guid guid;
      switch (faster_t->type) {
        case NULL_DISK:
          guid = faster_t->obj.null_store->StartSession();
          break;
        case FILESYSTEM_DISK:
          guid = faster_t->obj.store->StartSession();
          break;
      }
      active_sessions.insert(faster_t);
      char* str = new char[37];
      std::strcpy(str, guid.ToString().c_str());
      return str;
    }

  }

  void faster_free_session(const char* token) {
    if (token != NULL) {
      delete[] token;
    }
  }

  uint64_t faster_continue_session(faster_t* faster_t, const char* token) {
    if (faster_t == NULL) {
      return -1;
    } else {
      std::string guid_str(token);
      Guid guid = Guid::Parse(guid_str);
      uint64_t serial_num = 0;
      switch (faster_t->type) {
        case NULL_DISK:
          serial_num = faster_t->obj.null_store->ContinueSession(guid);
          break;
        case FILESYSTEM_DISK:
          serial_num = faster_t->obj.store->ContinueSession(guid);
          break;
      }
      active_sessions.insert(faster_t);
      return serial_num;
    }
  }

  void faster_stop_session(faster_t* faster_t) {
    if (faster_t != NULL) {
      switch (faster_t->type) {
        case NULL_DISK:
          faster_t->obj.null_store->StopSession();
          break;
        case FILESYSTEM_DISK:
          faster_t->obj.store->StopSession();
          break;
      }
      active_sessions.erase(faster_t);
    }
  }

  void faster_refresh_session(faster_t* faster_t) {
    if (faster_t != NULL) {
      switch (faster_t->type) {
        case NULL_DISK:
          faster_t->obj.null_store->Refresh();
          break;
        case FILESYSTEM_DISK:
          faster_t->obj.store->Refresh();
          break;
      }
    }
  }

  void faster_dump_distribution(faster_t* faster_t) {
    if (faster_t != NULL) {
      switch (faster_t->type) {
        case NULL_DISK:
          faster_t->obj.null_store->DumpDistribution();
          break;
        case FILESYSTEM_DISK:
          faster_t->obj.store->DumpDistribution();
          break;
      }
    }
  }

  bool faster_grow_index(faster_t* faster_t) {
    auto grow_index_callback = [](uint64_t new_size) {
        assert(new_size > 0);
    };
    if (faster_t != NULL) {
      switch (faster_t->type) {
        case NULL_DISK:
          return faster_t->obj.null_store->GrowIndex(grow_index_callback);
        case FILESYSTEM_DISK:
          return faster_t->obj.store->GrowIndex(grow_index_callback);
      }
    }
  }

} // extern "C"
