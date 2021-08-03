/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifdef NDEBUG
#define __ENABLE_TRACEPOINT__ 0
#else
#define __ENABLE_TRACEPOINT__ 1
#endif

#ifndef OCEANBASE_LIB_UTILITY_OB_TRACEPOINT_
#define OCEANBASE_LIB_UTILITY_OB_TRACEPOINT_
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "lib/oblog/ob_log.h"
#include "lib/alloc/alloc_assist.h"
#include "lib/list/ob_dlist.h"
#include "lib/coro/co_var.h"

#define TP_COMMA(x) ,
#define TP_EMPTY(x)
#define TP_PAIR_ARGS() 0, 0
#define TP_THIRD_rescan(x1, x2, x3, ...) x3
#define TP_THIRD(x1, x2, x3) TP_THIRD_rescan(x1, x2, x3, 0)
#define TP_COND(test, true_expr, false_expr) TP_THIRD(TP_PAIR_ARGS test, false_expr, true_expr)

#define TP_MAPCALL(f, cur) TP_COND(cur, TP_COMMA, TP_EMPTY)(cur) TP_COND(cur, f, TP_EMPTY)(cur)
#define TP_MAP8(f, a1, a2, a3, a4, a5, a6, a7, a8, ...)                                                            \
  TP_COND(a1, f, TP_EMPTY)                                                                                         \
  (a1) TP_MAPCALL(f, a2) TP_MAPCALL(f, a3) TP_MAPCALL(f, a4) TP_MAPCALL(f, a5) TP_MAPCALL(f, a6) TP_MAPCALL(f, a7) \
      TP_MAPCALL(f, a8)
#define TP_MAP(f, ...) TP_MAP8(f, ##__VA_ARGS__, (), (), (), (), (), (), (), ())

#define TP_COMPILER_BARRIER() asm volatile("" ::: "memory")
#define TP_AL(ptr)               \
  ({                             \
    TP_COMPILER_BARRIER();       \
    (OB_ISNULL(ptr)) ? 0 : *ptr; \
  })
#define TP_AS(x, v)        \
  ({                       \
    TP_COMPILER_BARRIER(); \
    if (OB_ISNULL(x)) {    \
    } else {               \
      *(x) = v;            \
    }                      \
    __sync_synchronize();  \
  })

#define TP_BCAS(x, ov, nv) __sync_bool_compare_and_swap((x), (ov), (nv))
#define TP_RELAX() PAUSE()
#define TP_CALL_FUNC(ptr, ...)                                             \
  ({                                                                       \
    int (*func)(TP_MAP(typeof, ##__VA_ARGS__)) = (typeof(func))TP_AL(ptr); \
    (NULL != func) ? func(__VA_ARGS__) : 0;                                \
  })

#define TRACEPOINT_CALL(name, ...)                                      \
  ({                                                                    \
    static void** func_ptr = ::oceanbase::common::tracepoint_get(name); \
    (NULL != func_ptr) ? TP_CALL_FUNC(func_ptr, ##__VA_ARGS__) : 0;     \
  })

#if __ENABLE_TRACEPOINT__
#define OB_I(key, ...) \
  TRACEPOINT_CALL(::oceanbase::common::refine_tp_key(__FILE__, __FUNCTION__, #key), ##__VA_ARGS__) ?:
#else
#define OB_I(...)
#endif

bool& get_tp_switch();

#define TP_SWITCH_GUARD(v) ::oceanbase::lib::ObSwitchGuard<get_tp_switch> osg_##__COUNTER__##_(v)

namespace oceanbase {
namespace lib {
using GetSwitchFunc = bool&();

template <GetSwitchFunc fn>
class ObSwitchGuard {
public:
  ObSwitchGuard(bool newval)
  {
    oldval_ = fn();
    fn() = newval;
  }
  ~ObSwitchGuard()
  {
    fn() = oldval_;
  }

private:
  bool oldval_;
};
}  // namespace lib
}  // namespace oceanbase

#define EVENT_CALL(event_no, ...)                                           \
  ({                                                                        \
    EventItem item;                                                         \
    item = ::oceanbase::common::EventTable::instance().get_event(event_no); \
    item.call();                                                            \
  })

#define ERRSIM_POINT_DEF(name) \
  void name##name(){};         \
  static oceanbase::common::NamedEventItem name(#name, oceanbase::common::EventTable::global_item_list());
#define ERRSIM_POINT_CALL(name) name ?:

// to check if a certain tracepoint is set
#define E(event_no, ...) EVENT_CALL(event_no, ##__VA_ARGS__) ?:

// to set a particular tracepoint
#define TP_SET_EVENT(id, error_in, occur, trigger_freq)              \
  {                                                                  \
    EventItem item;                                                  \
    item.error_code_ = error_in;                                     \
    item.occur_ = occur;                                             \
    item.trigger_freq_ = trigger_freq;                               \
    ::oceanbase::common::EventTable::instance().set_event(id, item); \
  }

#define TP_SET(file_name, func_name, key, trace_func) \
  *::oceanbase::common::tracepoint_get(refine_tp_key(file_name, func_name, key)) = (void*)(trace_func)
#define TP_SET_ERROR(file_name, func_name, key, err) \
  TP_SET(file_name, func_name, key, (int (*)()) & tp_const_error<(err)>)

namespace oceanbase {
namespace common {
inline const char* tp_basename(const char* path)
{
  const char* ret = OB_ISNULL(path) ? NULL : strrchr(path, '/');
  return (NULL == ret) ? path : ++ret;
}

inline const char* refine_tp_key(const char* s1, const char* s2, const char* s3)
{
  const int32_t BUFFER_SIZE = 256;
  static RLOCAL(lib::ByteBuf<BUFFER_SIZE>, buffer);
  const char* cret = nullptr;
  if (OB_ISNULL(s1) || OB_ISNULL(s2) || OB_ISNULL(s3)) {
  } else {
    s1 = tp_basename(s1);
    snprintf(buffer, BUFFER_SIZE, "%s:%s:%s", s1, s2, s3);
    cret = buffer;
  }
  return cret;
}

template <const int err>
int tp_const_error()
{
  return err;
}

class TPSymbolTable {
public:
  TPSymbolTable()
  {}
  ~TPSymbolTable()
  {}
  void** get(const char* name)
  {
    return (NULL != name) ? do_get(name) : NULL;
  }

private:
  static uint64_t BKDRHash(const char* str);
  enum { SYMBOL_SIZE_LIMIT = 128, SYMBOL_COUNT_LIMIT = 64 * 1024 };

  struct SymbolEntry {
    enum { FREE = 0, SETTING = 1, OK = 2 };
    SymbolEntry() : lock_(FREE), value_(NULL)
    {
      name_[0] = '\0';
    }
    ~SymbolEntry()
    {}

    bool find(const char* name);

    int lock_;
    void* value_;
    char name_[SYMBOL_SIZE_LIMIT];
  };

  void** do_get(const char* name);

  SymbolEntry symbol_table_[SYMBOL_COUNT_LIMIT];
};

inline void** tracepoint_get(const char* name)
{
  static TPSymbolTable symbol_table;
  return symbol_table.get(name);
}

struct EventItem {
  int64_t occur_;         // number of occurrences
  int64_t trigger_freq_;  // trigger frequency
  int64_t error_code_;    // error code to return

  EventItem();
  int call(void) const
  {
    int ret = 0;
    if (OB_LIKELY(trigger_freq_ == 0)) {
      ret = 0;
    } else if (get_tp_switch()) {  // true means skip errsim
      ret = 0;
    } else if (trigger_freq_ == 1) {
      ret = static_cast<int>(error_code_);
      COMMON_LOG(WARN, "[ERRSIM] sim error", K(ret));
    } else {
      if (rand() % trigger_freq_ == 0) {
        ret = static_cast<int>(error_code_);
        COMMON_LOG(WARN, "[ERRSIM] sim error", K(ret), K_(error_code), K_(trigger_freq), K(lbt()));
      } else {
        ret = 0;
      }
    }
    return ret;
  }
};

struct NamedEventItem : public ObDLinkBase<NamedEventItem> {
  NamedEventItem(const char* name, ObDList<NamedEventItem>& l) : name_(name)
  {
    l.add_last(this);
  }
  operator int(void) const
  {
    return item_.call();
  }

  const char* name_;
  EventItem item_;
};

class EventTable {
  static const int SIZE_OF_EVENT_TABLE = 100000;  // max number of tracepoints supported
public:
  EventTable()
  {
    for (int64_t i = 0; i < SIZE_OF_EVENT_TABLE; ++i) {
      memset(&(event_table_[i]), 0, sizeof(EventItem));
    }
  }
  virtual ~EventTable()
  {}

  // All tracepoints should be defined here before they can be used
  enum {
    EVENT_TABLE_INVALID = 0,
    EN_1,
    EN_2,
    EN_3,
    EN_4,
    EN_5,
    EN_6,
    EN_7,
    EN_8,
    EN_9,
    EN_IS_LOG_SYNC,                // 10
    EN_POST_ADD_REPILICA_MC,       // 11
    EN_MIGRATE_FETCH_MACRO_BLOCK,  // 12
    EN_WRITE_BLOCK,                // 13
    EN_COMMIT_SLOG,                // 14
    EN_SCHEDULE_INDEX_DAG,         // 15
    EN_INDEX_LOCAL_SORT_TASK,      // 16
    EN_INDEX_MERGE_TASK,           // 17
    EN_INDEX_WRITE_BLOCK,          // 18
    EN_INDEX_COMMIT_SLOG,          // 19
    EN_CHECK_CAN_DO_MERGE,         // 20
    EN_SCHEDULE_MERGE,             // 21
    EN_MERGE_MACROBLOCK,           // 22
    EN_MERGE_CHECKSUM,             // 23
    EN_MERGE_FINISH,               // 24
    EN_IO_SETUP,                   // 25
    EN_FORCE_WRITE_SSTABLE_SECOND_INDEX = 26,
    EN_SCHEDULE_MIGRATE = 27,
    EN_TRANS_AFTER_COMMIT = 28,
    EN_CHANGE_SCHEMA_VERSION_TO_ZERO = 29,
    EN_POST_REMOVE_REPLICA_MC_MSG = 30,
    EN_POST_ADD_REPLICA_MC_MSG = 31,
    EN_CHECK_SUB_MIGRATION_TASK = 32,
    EN_POST_GET_MEMBER_LIST_MSG = 33,
    EN_WRITE_CHECKPOIRNT = 34,
    EN_MERGE_SORT_READ_MSG = 35,
    EN_IO_SUBMIT = 36,
    EN_IO_GETEVENTS = 37,
    EN_TRANS_LEADER_ACTIVE = 38,
    EN_UNIT_MANAGER = 39,
    EN_IO_CANCEL = 40,
    EN_REPLAY_ROW = 41,
    EN_BIG_ROW_REPLAY_FOR_MINORING = 42,
    EN_START_STMT_INTERFACE_ERROR = 43,
    EN_START_PARTICIPANT_INTERFACE_ERROR = 44,
    EN_END_PARTICIPANT_INTERFACE_ERROR = 45,
    EN_END_STMT_INTERFACE_ERROR = 46,
    EN_GET_GTS_LEADER = 47,
    ALLOC_LOG_ID_AND_TIMESTAMP_ERROR = 48,
    AFTER_MIGRATE_FINISH_TASK = 49,
    EN_TRANS_START_TASK_ERROR = 50,
    EN_TRANS_END_TASK_ERROR = 51,
    EN_VALID_MIGRATE_SRC = 52,
    EN_BALANCE_TASK_EXE_ERR = 53,
    EN_ADD_REBUILD_PARENT_SRC = 54,
    EN_BAD_BLOCK_ERROR = 55,
    EN_ADD_RESTORE_TASK_ERROR = 56,
    EN_CTAS_FAIL_NO_DROP_ERROR = 57,
    EN_IO_CHANNEL_QUEUE_ERROR = 58,
    EN_GET_SCHE_CTX_ERROR = 59,
    EN_CLOG_RESTORE_REPLAYED_LOG = 60,
    EN_GEN_REBUILD_TASK = 61,
    EN_IO_HANG_ERROR = 62,
    EN_CREATE_TENANT_TRANS_ONE_FAILED = 63,
    EN_CREATE_TENANT_TRANS_TWO_FAILED = 64,
    EN_DELAY_REPLAY_SOURCE_SPLIT_LOG = 65,
    EN_BLOCK_SPLIT_PROGRESS_RESPONSE = 66,
    EN_RPC_ENCODE_SEGMENT_DATA_ERR = 67,
    EN_RPC_ENCODE_RAW_DATA_ERR = 68,
    EN_RPC_DECODE_COMPRESS_DATA_ERR = 69,
    EN_RPC_DECODE_RAW_DATA_ERR = 70,
    EN_BLOCK_SHUTDOWN_PARTITION = 71,
    EN_BLOCK_SPLIT_SOURCE_PARTITION = 72,
    EN_BLOCK_SUBMIT_SPLIT_SOURCE_LOG = 73,
    EN_BLOCK_SPLIT_DEST_PARTITION = 74,
    EN_CREATE_TENANT_TRANS_THREE_FAILED = 75,
    EN_ALTER_CLUSTER_FAILED = 76,
    EN_STANDBY_REPLAY_SCHEMA_FAIL = 77,
    EN_STANDBY_REPLAY_CREATE_TABLE_FAIL = 78,
    EN_STANDBY_REPLAY_CREATE_TENANT_FAIL = 79,
    EN_STANDBY_REPLAY_CREATE_USER_FAIL = 80,
    EN_CREATE_TENANT_BEFORE_PERSIST_MEMBER_LIST = 81,
    EN_CREATE_TENANT_END_PERSIST_MEMBER_LIST = 82,
    EN_BROADCAST_CLUSTER_STATUS_FAIL = 83,
    EN_SET_FREEZE_INFO_FAILED = 84,
    EN_UPDATE_MAJOR_SCHEMA_FAIL = 85,
    EN_RENEW_SNAPSHOT_FAIL = 86,
    EN_FOLLOWER_UPDATE_FREEZE_INFO_FAIL = 87,
    EN_PARTITION_ITERATOR_FAIL = 88,
    EN_REFRESH_INCREMENT_SCHEMA_PHASE_THREE_FAILED = 89,
    EN_MIGRATE_LOGIC_TASK = 90,
    EN_REPLAY_ADD_PARTITION_TO_PG_CLOG = 91,
    EN_REPLAY_ADD_PARTITION_TO_PG_CLOG_AFTER_CREATE_SSTABLE = 92,
    EN_BEFORE_RENEW_SNAPSHOT_FAIL = 93,
    EN_BUILD_INDEX_RELEASE_SNAPSHOT_FAILED = 94,
    EN_CREATE_PG_PARTITION_FAIL = 95,
    EN_PUSH_TASK_FAILED = 96,
    EN_PUSH_REFERENCE_TABLE_FAIL = 97,
    EN_SLOG_WAIT_FLUSH_LOG = 98,
    EN_SET_MEMBER_LIST_FAIL = 99,
    EN_CREATE_TABLE_TRANS_END_FAIL = 100,
    EN_ABROT_INDEX_FAIL = 101,
    EN_DELAY_REPLAY_SOURCE_SPLIT_LOG_R_REPLICA = 102,
    EN_SKIP_GLOBAL_SSTABLE_SCHEMA_VERSION = 103,
    EN_STOP_ROOT_INSPECTION = 104,
    EN_DROP_TENANT_FAILED = 105,
    EN_SKIP_DROP_MEMTABLE = 106,
    EN_SKIP_DROP_PG_PARTITION = 107,
    EN_OBSERVER_CREATE_PARTITION_FAILED = 109,
    EN_CREATE_PARTITION_WITH_OLD_MAJOR_TS = 110,
    EN_PREPARE_SPLIT_FAILED = 111,
    EN_REPLAY_SOURCE_SPLIT_LOG_FAILED = 112,
    EN_SAVE_SPLIT_STATE_FAILED = 113,
    EN_FORCE_REFRESH_TABLE = 114,
    EN_REPLAY_SPLIT_DEST_LOG_FAILED = 116,
    EN_PROCESS_TO_PRIMARY_ERR = 117,
    EN_CREATE_PG_AFTER_CREATE_SSTBALES = 118,
    EN_CREATE_PG_AFTER_REGISTER_TRANS_SERVICE = 119,
    EN_CREATE_PG_AFTER_REGISTER_ELECTION_MGR = 120,
    EN_CREATE_PG_AFTER_ADD_PARTITIONS_TO_MGR = 121,
    EN_CREATE_PG_AFTER_ADD_PARTITIONS_TO_REPLAY_ENGINE = 122,
    EN_CREATE_PG_AFTER_BATCH_START_PARTITION_ELECTION = 123,
    EN_BACKUP_MACRO_BLOCK_SUBTASK_FAILED = 124,
    EN_BACKUP_REPORT_RESULT_FAILED = 125,
    EN_RESTORE_UPDATE_PARTITION_META_FAILED = 126,
    EN_BACKUP_FILTER_TABLE_BY_SCHEMA = 127,
    EN_FORCE_DFC_BLOCK = 128,
    EN_SERVER_PG_META_WRITE_HALF_FAILED = 129,
    EN_SERVER_TENANT_FILE_SUPER_BLOCK_WRITE_HALF_FAILED = 130,
    EN_DTL_ONE_ROW_ONE_BUFFER = 131,
    EN_LOG_ARCHIVE_PUSH_LOG_FAILED = 132,

    EN_BACKUP_DATA_VERSION_GAP_OVER_LIMIT = 133,
    EN_LOG_ARHIVE_SCHEDULER_INTERRUPT = 134,
    EN_BACKUP_IO_LIST_FILE = 135,
    EN_BACKUP_IO_IS_EXIST = 136,
    EN_BACKUP_IO_GET_FILE_LENGTH = 137,
    EN_BACKUP_IO_BEFORE_DEL_FILE = 138,
    EN_BACKUP_IO_AFTER_DEL_FILE = 139,
    EN_BACKUP_IO_BEFORE_MKDIR = 140,
    EN_BACKUP_IO_AFTER_MKDIR = 141,
    EN_BACKUP_IO_UPDATE_FILE_MODIFY_TIME = 142,
    EN_BACKUP_IO_BEFORE_WRITE_SINGLE_FILE = 143,
    EN_BACKUP_IO_AFTER_WRITE_SINGLE_FILE = 144,
    EN_BACKUP_IO_READER_OPEN = 145,
    EN_BACKUP_IO_READER_PREAD = 146,
    EN_BACKUP_IO_WRITE_OPEN = 147,
    EN_BACKUP_IO_WRITE_WRITE = 148,
    EN_BACKUP_IO_APPENDER_OPEN = 149,
    EN_BACKUP_IO_APPENDER_WRITE = 150,
    EN_ROOT_BACKUP_MAX_GENERATE_NUM = 151,
    EN_ROOT_BACKUP_NEED_SWITCH_TENANT = 152,
    EN_BACKUP_FILE_APPENDER_CLOSE = 153,
    EN_RESTORE_MACRO_CRC_ERROR = 154,
    EN_BACKUP_RECOVERY_WINDOW = 155,
    EN_BACKUP_AUTO_DELETE_INTERVAL = 156,
    EN_RESTORE_FETCH_CLOG_ERROR = 157,
    EN_BACKUP_LEASE_CAN_TAKEOVER = 158,
    EN_BACKUP_EXTERN_INFO_ERROR = 159,
    EN_INCREMENTAL_BACKUP_NUM = 160,
    EN_LOG_ARCHIVE_BEFORE_PUSH_LOG_FAILED = 161,
    EN_BACKUP_META_INDEX_BUFFER_NOT_COMPLETED = 162,
    EN_BACKUP_MACRO_INDEX_BUFFER_NOT_COMPLETED = 163,
    EN_LOG_ARCHIVE_DATA_BUFFER_NOT_COMPLETED = 164,
    EN_LOG_ARCHIVE_INDEX_BUFFER_NOT_COMPLETED = 165,
    EN_FILE_SYSTEM_RENAME_ERROR = 166,
    EN_BACKUP_OBSOLETE_INTERVAL = 167,
    EN_BACKUP_BACKUP_LOG_ARCHIVE_INTERRUPTED = 168,
    EN_BACKUP_BACKUPSET_EXTERN_INFO_ERROR = 169,
    EN_BACKUP_SCHEDULER_GET_SCHEMA_VERSION_ERROR = 170,

    EN_CHECK_STANDBY_CLUSTER_SCHEMA_CONDITION = 201,
    EN_ALLOCATE_LOB_BUF_FAILED = 202,
    EN_ALLOCATE_DESERIALIZE_LOB_BUF_FAILED = 203,
    EN_ENCRYPT_ALLOCATE_HASHMAP_FAILED = 204,
    EN_ENCRYPT_ALLOCATE_ROW_BUF_FAILED = 205,
    EN_ENCRYPT_GET_MASTER_KEY_FAILED = 206,
    EN_DECRYPT_ALLOCATE_ROW_BUF_FAILED = 207,
    EN_DECRYPT_GET_MASTER_KEY_FAILED = 208,
    EN_FAST_MIGRATE_CHANGE_MEMBER_LIST_NOT_BEGIN = 209,
    EN_FAST_MIGRATE_CHANGE_MEMBER_LIST_AFTER_REMOVE = 210,
    EN_FAST_MIGRATE_CHANGE_MEMBER_LIST_SUCCESS_BUT_TIMEOUT = 211,
    EN_SCHEDULE_DATA_MINOR_MERGE = 212,
    EN_LOG_SYNC_SLOW = 213,
    EN_WRITE_CONFIG_FILE_FAILED = 214,
    EN_INVALID_ADDR_WEAK_READ_FAILED = 215,
    EN_STACK_OVERFLOW_CHECK_EXPR_STACK_SIZE = 216,
    EN_ENABLE_PDML_ALL_FEATURE = 217,
    EN_MIGRATE_ADD_PARTITION_FAILED = 224,

    EN_PRINT_QUERY_SQL = 231,
    EN_ADD_NEW_PG_TO_PARTITION_SERVICE = 232,
    EN_DML_DISABLE_RANDOM_RESHUFFLE = 233,
    EN_RESIZE_PHYSICAL_FILE_FAILED = 234,
    EN_ALLOCATE_RESIZE_MEMORY_FAILED = 235,
    EN_WRITE_SUPER_BLOCK_FAILED = 236,
    EN_GC_FAILED_PARTICIPANTS = 237,
    EN_SSL_INVITE_NODES_FAILED = 238,
    EN_ADD_TRIGGER_SKIP_MAP = 239,
    EN_DEL_TRIGGER_SKIP_MAP = 240,
    EN_RESET_FREE_MEMORY = 241,
    EN_BKGD_TASK_REPORT_COMPLETE = 242,
    EN_BKGD_TRANSMIT_CHECK_STATUS_PER_ROW = 243,

    //
    EN_TRANS_SHARED_LOCK_CONFLICT = 250,
    EN_ENABLE_HASH_JOIN_CACHE_AWARE = 251,
    EN_SET_DISABLE_HASH_JOIN_BATCH = 252,

    // only work for remote execute
    EN_DISABLE_REMOTE_EXEC_WITH_PLAN = 255,
    EN_REMOTE_EXEC_ERR = 256,

    EN_XA_PREPARE_ERROR = 260,
    EN_XA_UPDATE_COORD_FAILED = 261,
    EN_XA_PREPARE_RESP_LOST = 262,
    EN_XA_RPC_TIMEOUT = 263,
    EN_XA_COMMIT_ABORT_RESP_LOST = 264,
    EN_XA_1PC_RESP_LOST = 265,
    EN_DISK_ERROR = 266,

    EN_CLOG_DUMP_ILOG_MEMSTORE_RENAME_FAILURE = 267,
    EN_CLOG_ILOG_MEMSTORE_ALLOC_MEMORY_FAILURE = 268,
    EN_PREVENT_SYNC_REPORT = 360,
    EN_PREVENT_ASYNC_REPORT = 361,
    EVENT_TABLE_MAX = SIZE_OF_EVENT_TABLE
  };

  /* get an event value */
  inline EventItem get_event(int64_t index)
  {
    return (index >= 0 && index < SIZE_OF_EVENT_TABLE) ? event_table_[index] : event_table_[0];
  }

  /* set an event value */
  inline void set_event(int64_t index, const EventItem& item)
  {
    if (index >= 0 && index < SIZE_OF_EVENT_TABLE) {
      event_table_[index] = item;
    }
  }

  static inline void set_event(const char* name, const EventItem& item)
  {
    DLIST_FOREACH_NORET(i, global_item_list())
    {
      if (NULL != i->name_ && NULL != name && strcmp(i->name_, name) == 0) {
        i->item_ = item;
      }
    }
  }

  static ObDList<NamedEventItem>& global_item_list()
  {
    static ObDList<NamedEventItem> g_list;
    return g_list;
  }

  static EventTable& instance()
  {
    static EventTable et;
    return et;
  }

private:
  /*
     Array of error codes for all tracepoints.
     For normal error code generation, the value should be the error code itself.
   */
  EventItem event_table_[SIZE_OF_EVENT_TABLE];
};

inline void event_access(int64_t index, /* tracepoint number */
    EventItem& item, bool is_get)       /* is a 'get' */
{
  if (is_get)
    item = EventTable::instance().get_event(index);
  else
    EventTable::instance().set_event(index, item);
}

}  // namespace common
}  // namespace oceanbase

#endif  // OCEANBASE_LIB_UTILITY_OB_TRACEPOINT_

#if __TEST_TRACEPOINT__
#include <stdio.h>

int fake_syscall()
{
  return 0;
}

int test_tracepoint(int x)
{
  int err = 0;
  printf("=== start call: %d ===\n", x);
  if (0 != (err = OB_I(a, x) fake_syscall())) {
    printf("fail at step 1: err=%d\n", err);
  } else if (0 != (err = OB_I(b) fake_syscall())) {
    printf("fail at step 2: err=%d\n", err);
  } else {
    printf("succ\n");
  }
  return err;
}

int tp_handler(int x)
{
  printf("tp_handler: x=%d\n", x);
  return 0;
}

int run_tests()
{
  test_tracepoint(0);
  TP_SET("tracepoint.h", "test_tracepoint", "a", &tp_handler);
  TP_SET_ERROR("tracepoint.h", "test_tracepoint", "b", -1);
  test_tracepoint(1);
  TP_SET_ERROR("tracepoint.h", "test_tracepoint", "b", -2);
  test_tracepoint(2);
  TP_SET("tracepoint.h", "test_tracepoint", "b", NULL);
  test_tracepoint(3);
}
#endif
