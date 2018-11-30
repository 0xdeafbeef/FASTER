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

    ReadContext(uint64_t key, uint64_t* result)
      : key_{ key } 
      , result_ { result }  {
    }

    /// Copy (and deep-copy) constructor.
    ReadContext(const ReadContext& other)
      : key_{ other.key_ } 
      , result_ { other.result_ }  {
    }

    /// The implicit and explicit interfaces require a key() accessor.
    inline const Key& key() const {
      return key_;
    }

    inline void Get(const value_t& value) { 
      //*result_ = value.value_;
    }
    inline void GetAtomic(const value_t& value) {
      //*result_ = value.atomic_value_.load();
    }

    uint64_t val() const {
      return 1;
    }

   protected:
    /// The explicit interface requires a DeepCopy_Internal() implementation.
    Status DeepCopy_Internal(IAsyncContext*& context_copy) {
      return IAsyncContext::DeepCopy_Internal(*this, context_copy);
    }

   private:
    Key key_;
    uint64_t* result_;
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

  class RmwContext : public IAsyncContext {
   public:
    typedef Key key_t;
    typedef Value value_t;

    RmwContext(uint64_t key, uint64_t incr)
      : key_{ key }
      , incr_{ incr } {
    }

    /// Copy (and deep-copy) constructor.
    RmwContext(const RmwContext& other)
      : key_{ other.key_ }
      , incr_{ other.incr_ } {
    }

    /// The implicit and explicit interfaces require a key() accessor.
    const Key& key() const {
      return key_;
    }
    inline static constexpr uint32_t value_size() {
      return sizeof(value_t);
    }

    /// Initial, non-atomic, and atomic RMW methods.
    inline void RmwInitial(value_t& value) {
      value.value_ = incr_;
    }
    inline void RmwCopy(const value_t& old_value, value_t& value) {
      value.value_ = old_value.value_ + incr_;
    }
    inline bool RmwAtomic(value_t& value) {
      value.atomic_value_.fetch_add(incr_);
      return true;
    }

   protected:
    /// The explicit interface requires a DeepCopy_Internal() implementation.
    Status DeepCopy_Internal(IAsyncContext*& context_copy) {
      return IAsyncContext::DeepCopy_Internal(*this, context_copy);
    }

   private:
    Key key_;
    uint64_t incr_;
  };

  typedef FASTER::environment::QueueIoHandler handler_t;
  typedef FASTER::device::FileSystemDisk<handler_t, 1073741824L> disk_t;
  typedef FASTER::device::NullDisk  disk_null_t;
  using store_t = FasterKv<Key, Value, disk_t>;
  struct faster_t { store_t* obj; };
  
  faster_t* faster_open_with_disk(const uint64_t table_size, const uint64_t log_size, const char* storage) {
    faster_t* res = new faster_t();
    std::experimental::filesystem::create_directory(storage);
    res->obj= new store_t { table_size, log_size, storage };
    return res;
  }

  uint8_t faster_upsert(faster_t* faster_t, const uint64_t key, const uint64_t value) {
    store_t* store = faster_t->obj;

    auto callback = [](IAsyncContext* ctxt, Status result) {
        assert(result == Status::Ok);
    };

    UpsertContext context { key, value };
    Status result = store->Upsert(context, callback, 1);
    return static_cast<uint8_t>(result);
  }

  uint8_t faster_rmw(faster_t* faster_t, const uint64_t key, const uint64_t value) {
    store_t* store = faster_t->obj;

    auto callback = [](IAsyncContext* ctxt, Status result) {
      CallbackContext<RmwContext> context { ctxt };
    };

    RmwContext context{ key, value};
    Status result = store->Rmw(context, callback, 1);
    return static_cast<uint8_t>(result);
  }

  uint8_t faster_read(faster_t* faster_t, const uint64_t key) {
    store_t* store = faster_t->obj;

    auto callback = [](IAsyncContext* ctxt, Status result) {
      CallbackContext<ReadContext> context { ctxt };
    };

    ReadContext context {key, NULL};
    Status result = store->Read(context, callback, 1);
    return static_cast<uint8_t>(result);
  }

  // It is up to the caller to dealloc faster_checkpoint_result*
  // first token, then struct
  faster_checkpoint_result* faster_checkpoint(faster_t* faster_t) {
    store_t* store = faster_t->obj;
    auto hybrid_log_persistence_callback = [](Status result, uint64_t persistent_serial_num) {
      assert(result == Status::Ok);
    };

    Guid token;
    bool checked = store->Checkpoint(nullptr, hybrid_log_persistence_callback, token);
    //faster_checkpoint_result* res = new faster_checkpoint_result();
    faster_checkpoint_result* res = (faster_checkpoint_result*) malloc(sizeof(faster_checkpoint_result));
    res->checked = checked;
    res->token = (char*) malloc(37 * sizeof(char));
    strncpy(res->token, token.ToString().c_str(), 37);
    return res;
  }

  void faster_destroy(faster_t *faster_t) {
    if (faster_t == NULL)
      return;

    delete faster_t->obj;
    delete faster_t;
  }

  uint64_t faster_size(faster_t* faster_t) {
    if (faster_t == NULL) {
      return -1;
    } else {
      store_t* store = faster_t->obj;
      return store->Size();
    }
  }

  // It is up to the caller to deallocate the faster_recover_result* struct
  faster_recover_result* faster_recover(faster_t* faster_t, const char* index_token, const char* hybrid_log_token) {
    if (faster_t == NULL) {
      return NULL;
    } else {
      store_t* store = faster_t->obj;
      uint32_t ver;
      std::vector<Guid> _session_ids;

      std::string index_str(index_token);
      std::string hybrid_str(hybrid_log_token);
      //TODO: error handling
      Guid index_guid = Guid::Parse(index_str);
      Guid hybrid_guid = Guid::Parse(hybrid_str);
      Status sres = store->Recover(index_guid, hybrid_guid, ver, _session_ids);

      uint8_t status_result = static_cast<uint8_t>(sres);
      faster_recover_result* res = (faster_recover_result*) malloc(sizeof(faster_recover_result));
      res->status= status_result;
      res->version = ver;
    
      int ids_total = _session_ids.size();
      res->session_ids_count = ids_total;
      int session_len = 37; // 36 + 1
      res->session_ids = (char**) malloc(sizeof(char*));

      for (int i = 0; i < ids_total; i++) {
          res->session_ids[i] = (char*) malloc(session_len);
      }

      int counter = 0;
      for (auto& id : _session_ids) {
        strncpy(res->session_ids[counter], id.ToString().c_str(), session_len);
        counter++;
      }

      return res;
    }
  }

  void faster_complete_pending(faster_t* faster_t, bool b) {
    if (faster_t != NULL) {
      store_t* store = faster_t->obj;
      store->CompletePending(b);
    }
  }

  // Thread-related
  
  const char* faster_start_session(faster_t* faster_t) {
    if (faster_t == NULL) {
      return NULL;
    } else {
      store_t* store = faster_t->obj;
      Guid guid = store->StartSession();
      return guid.ToString().c_str();
    }

  }

  uint64_t faster_continue_session(faster_t* faster_t, const char* token) {
    if (faster_t == NULL) {
      return -1;
    } else {
      store_t* store = faster_t->obj;
      std::string guid_str(token);
      Guid guid = Guid::Parse(guid_str);
      return store->ContinueSession(guid);
    }
  }

  void faster_stop_session(faster_t* faster_t) {
    if (faster_t != NULL) {
      store_t* store = faster_t->obj;
      store->StopSession();
    }
  }

  void faster_refresh_session(faster_t* faster_t) {
    if (faster_t != NULL) {
      store_t* store = faster_t->obj;
      store->Refresh();
    }
  }

} // extern "C"
