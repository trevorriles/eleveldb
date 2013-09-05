// -------------------------------------------------------------------
//
// eleveldb: Erlang Wrapper for LevelDB (http://code.google.com/p/leveldb/)
//
// Copyright (c) 2011-2013 Basho Technologies, Inc. All Rights Reserved.
//
// This file is provided to you under the Apache License,
// Version 2.0 (the "License"); you may not use this file
// except in compliance with the License.  You may obtain
// a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// -------------------------------------------------------------------

#include <new>
#include <set>
#include <stack>
#include <deque>
#include <sstream>
#include <utility>
#include <stdexcept>
#include <algorithm>
#include <vector>

#include "eleveldb.h"

#include "leveldb/db.h"
#include "leveldb/comparator.h"
#include "leveldb/write_batch.h"
#include "leveldb/cache.h"
#include "leveldb/filter_policy.h"
#include "leveldb/perf_count.h"

#ifndef INCL_THREADING_H
    #include "threading.h"
#endif

#ifndef INCL_WORKITEMS_H
    #include "workitems.h"
#endif

#ifndef ATOMS_H
    #include "atoms.h"
#endif

#include "work_result.hpp"

#include "detail.hpp"

static ErlNifFunc nif_funcs[] =
{
    {"close", 1, eleveldb_close},
    {"iterator_close", 1, eleveldb_iterator_close},
    {"status", 2, eleveldb_status},
    {"destroy", 2, eleveldb_destroy},
    {"repair", 2, eleveldb_repair},
    {"is_empty", 1, eleveldb_is_empty},

    {"async_open", 3, eleveldb::async_open},
    {"async_write", 4, eleveldb::async_write},
    {"async_get", 4, eleveldb::async_get},

    {"async_iterator", 3, eleveldb::async_iterator},
    {"async_iterator", 4, eleveldb::async_iterator},

    {"async_iterator_move", 3, eleveldb::async_iterator_move}
};


namespace eleveldb {

// Atoms (initialized in on_load)
ERL_NIF_TERM ATOM_TRUE;
ERL_NIF_TERM ATOM_FALSE;
ERL_NIF_TERM ATOM_OK;
ERL_NIF_TERM ATOM_ERROR;
ERL_NIF_TERM ATOM_EINVAL;
ERL_NIF_TERM ATOM_BADARG;
ERL_NIF_TERM ATOM_CREATE_IF_MISSING;
ERL_NIF_TERM ATOM_ERROR_IF_EXISTS;
ERL_NIF_TERM ATOM_WRITE_BUFFER_SIZE;
ERL_NIF_TERM ATOM_MAX_OPEN_FILES;
ERL_NIF_TERM ATOM_BLOCK_SIZE;                    /* DEPRECATED */
ERL_NIF_TERM ATOM_SST_BLOCK_SIZE;
ERL_NIF_TERM ATOM_BLOCK_RESTART_INTERVAL;
ERL_NIF_TERM ATOM_ERROR_DB_OPEN;
ERL_NIF_TERM ATOM_ERROR_DB_PUT;
ERL_NIF_TERM ATOM_NOT_FOUND;
ERL_NIF_TERM ATOM_VERIFY_CHECKSUMS;
ERL_NIF_TERM ATOM_FILL_CACHE;
ERL_NIF_TERM ATOM_SYNC;
ERL_NIF_TERM ATOM_ERROR_DB_DELETE;
ERL_NIF_TERM ATOM_CLEAR;
ERL_NIF_TERM ATOM_PUT;
ERL_NIF_TERM ATOM_DELETE;
ERL_NIF_TERM ATOM_ERROR_DB_WRITE;
ERL_NIF_TERM ATOM_BAD_WRITE_ACTION;
ERL_NIF_TERM ATOM_KEEP_RESOURCE_FAILED;
ERL_NIF_TERM ATOM_ITERATOR_CLOSED;
ERL_NIF_TERM ATOM_FIRST;
ERL_NIF_TERM ATOM_LAST;
ERL_NIF_TERM ATOM_NEXT;
ERL_NIF_TERM ATOM_PREV;
ERL_NIF_TERM ATOM_PREFETCH;
ERL_NIF_TERM ATOM_INVALID_ITERATOR;
ERL_NIF_TERM ATOM_CACHE_SIZE;
ERL_NIF_TERM ATOM_PARANOID_CHECKS;
ERL_NIF_TERM ATOM_VERIFY_COMPACTIONS;
ERL_NIF_TERM ATOM_ERROR_DB_DESTROY;
ERL_NIF_TERM ATOM_KEYS_ONLY;
ERL_NIF_TERM ATOM_COMPRESSION;
ERL_NIF_TERM ATOM_ERROR_DB_REPAIR;
ERL_NIF_TERM ATOM_USE_BLOOMFILTER;

}   // namespace eleveldb





using std::nothrow;

struct eleveldb_itr_handle;

class eleveldb_thread_pool;
class eleveldb_priv_data;



// Erlang helpers:
ERL_NIF_TERM error_einval(ErlNifEnv* env)
{
    return enif_make_tuple2(env, eleveldb::ATOM_ERROR, eleveldb::ATOM_EINVAL);
}

static ERL_NIF_TERM error_tuple(ErlNifEnv* env, ERL_NIF_TERM error, leveldb::Status& status)
{
    ERL_NIF_TERM reason = enif_make_string(env, status.ToString().c_str(),
                                           ERL_NIF_LATIN1);
    return enif_make_tuple2(env, eleveldb::ATOM_ERROR,
                            enif_make_tuple2(env, error, reason));
}

static ERL_NIF_TERM slice_to_binary(ErlNifEnv* env, leveldb::Slice s)
{
    ERL_NIF_TERM result;
    unsigned char* value = enif_make_new_binary(env, s.size(), &result);
    memcpy(value, s.data(), s.size());
    return result;
}



/* Module-level private data: */
class eleveldb_priv_data
{
 eleveldb_priv_data(const eleveldb_priv_data&);             // nocopy
 eleveldb_priv_data& operator=(const eleveldb_priv_data&);  // nocopyassign

 public:
  eleveldb::eleveldb_thread_pool thread_pool;

 eleveldb_priv_data(const size_t n_write_threads)
  : thread_pool(n_write_threads)
 {}
};


ERL_NIF_TERM parse_open_option(ErlNifEnv* env, ERL_NIF_TERM item, leveldb::Options& opts)
{
    int arity;
    const ERL_NIF_TERM* option;
    if (enif_get_tuple(env, item, &arity, &option))
    {
        if (option[0] == eleveldb::ATOM_CREATE_IF_MISSING)
            opts.create_if_missing = (option[1] == eleveldb::ATOM_TRUE);
        else if (option[0] == eleveldb::ATOM_ERROR_IF_EXISTS)
            opts.error_if_exists = (option[1] == eleveldb::ATOM_TRUE);
        else if (option[0] == eleveldb::ATOM_PARANOID_CHECKS)
            opts.paranoid_checks = (option[1] == eleveldb::ATOM_TRUE);
        else if (option[0] == eleveldb::ATOM_VERIFY_COMPACTIONS)
            opts.verify_compactions = (option[1] == eleveldb::ATOM_TRUE);
        else if (option[0] == eleveldb::ATOM_MAX_OPEN_FILES)
        {
            int max_open_files;
            if (enif_get_int(env, option[1], &max_open_files))
                opts.max_open_files = max_open_files;
        }
        else if (option[0] == eleveldb::ATOM_WRITE_BUFFER_SIZE)
        {
            unsigned long write_buffer_sz;
            if (enif_get_ulong(env, option[1], &write_buffer_sz))
                opts.write_buffer_size = write_buffer_sz;
        }
        else if (option[0] == eleveldb::ATOM_BLOCK_SIZE)
        {
            /* DEPRECATED: the old block_size atom was actually ignored. */
            unsigned long block_sz;
            enif_get_ulong(env, option[1], &block_sz); // ignore
        }
        else if (option[0] == eleveldb::ATOM_SST_BLOCK_SIZE)
        {
            unsigned long sst_block_sz(0);
            if (enif_get_ulong(env, option[1], &sst_block_sz))
             opts.block_size = sst_block_sz; // Note: We just set the "old" block_size option.
        }
        else if (option[0] == eleveldb::ATOM_BLOCK_RESTART_INTERVAL)
        {
            int block_restart_interval;
            if (enif_get_int(env, option[1], &block_restart_interval))
                opts.block_restart_interval = block_restart_interval;
        }
        else if (option[0] == eleveldb::ATOM_CACHE_SIZE)
        {
            unsigned long cache_sz;
            if (enif_get_ulong(env, option[1], &cache_sz))
                if (cache_sz != 0)
                 {
                    opts.block_cache = leveldb::NewLRUCache(cache_sz);
                 }
        }
        else if (option[0] == eleveldb::ATOM_COMPRESSION)
        {
            if (option[1] == eleveldb::ATOM_TRUE)
            {
                opts.compression = leveldb::kSnappyCompression;
            }
            else
            {
                opts.compression = leveldb::kNoCompression;
            }
        }
        else if (option[0] == eleveldb::ATOM_USE_BLOOMFILTER)
        {
            // By default, we want to use a 16-bit-per-key bloom filter on a
            // per-table basis. We only disable it if explicitly asked. Alternatively,
            // one can provide a value for # of bits-per-key.
            unsigned long bfsize = 16;
            if (option[1] == eleveldb::ATOM_TRUE || enif_get_ulong(env, option[1], &bfsize))
            {
                opts.filter_policy = leveldb::NewBloomFilterPolicy2(bfsize);
            }
        }
    }

    return eleveldb::ATOM_OK;
}

ERL_NIF_TERM parse_read_option(ErlNifEnv* env, ERL_NIF_TERM item, leveldb::ReadOptions& opts)
{
    int arity;
    const ERL_NIF_TERM* option;
    if (enif_get_tuple(env, item, &arity, &option))
    {
        if (option[0] == eleveldb::ATOM_VERIFY_CHECKSUMS)
            opts.verify_checksums = (option[1] == eleveldb::ATOM_TRUE);
        else if (option[0] == eleveldb::ATOM_FILL_CACHE)
            opts.fill_cache = (option[1] == eleveldb::ATOM_TRUE);
    }

    return eleveldb::ATOM_OK;
}

ERL_NIF_TERM parse_write_option(ErlNifEnv* env, ERL_NIF_TERM item, leveldb::WriteOptions& opts)
{
    int arity;
    const ERL_NIF_TERM* option;
    if (enif_get_tuple(env, item, &arity, &option))
    {
        if (option[0] == eleveldb::ATOM_SYNC)
            opts.sync = (option[1] == eleveldb::ATOM_TRUE);
    }

    return eleveldb::ATOM_OK;
}

ERL_NIF_TERM write_batch_item(ErlNifEnv* env, ERL_NIF_TERM item, leveldb::WriteBatch& batch)
{
    int arity;
    const ERL_NIF_TERM* action;
    if (enif_get_tuple(env, item, &arity, &action) ||
        enif_is_atom(env, item))
    {
        if (item == eleveldb::ATOM_CLEAR)
        {
            batch.Clear();
            return eleveldb::ATOM_OK;
        }

        ErlNifBinary key, value;

        if (action[0] == eleveldb::ATOM_PUT && arity == 3 &&
            enif_inspect_binary(env, action[1], &key) &&
            enif_inspect_binary(env, action[2], &value))
        {
            leveldb::Slice key_slice((const char*)key.data, key.size);
            leveldb::Slice value_slice((const char*)value.data, value.size);
            batch.Put(key_slice, value_slice);
            return eleveldb::ATOM_OK;
        }

        if (action[0] == eleveldb::ATOM_DELETE && arity == 2 &&
            enif_inspect_binary(env, action[1], &key))
        {
            leveldb::Slice key_slice((const char*)key.data, key.size);
            batch.Delete(key_slice);
            return eleveldb::ATOM_OK;
        }
    }

    // Failed to match clear/put/delete; return the failing item
    return item;
}



namespace eleveldb {

ERL_NIF_TERM send_reply(ErlNifEnv *env, ERL_NIF_TERM ref, ERL_NIF_TERM reply)
{
    ErlNifPid pid;
    ErlNifEnv *msg_env = enif_alloc_env();
    ERL_NIF_TERM msg = enif_make_tuple2(msg_env,
                                        enif_make_copy(msg_env, ref),
                                        enif_make_copy(msg_env, reply));
    enif_self(env, &pid);
    enif_send(env, &pid, msg_env, msg);
    enif_free_env(msg_env);
    return ATOM_OK;
}

ERL_NIF_TERM
async_open(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    char db_name[4096];

    if(!enif_get_string(env, argv[1], db_name, sizeof(db_name), ERL_NIF_LATIN1) ||
       !enif_is_list(env, argv[2]))
    {
        return enif_make_badarg(env);
    }   // if

    ERL_NIF_TERM caller_ref = argv[0];

    eleveldb_priv_data& priv = *static_cast<eleveldb_priv_data *>(enif_priv_data(env));

    leveldb::Options *opts = new leveldb::Options;
    fold(env, argv[2], parse_open_option, *opts);

    eleveldb::WorkTask *work_item = new eleveldb::OpenTask(env, caller_ref,
                                                              db_name, opts);

    if(false == priv.thread_pool.submit(work_item))
    {
        delete work_item;
        return send_reply(env, caller_ref,
                          enif_make_tuple2(env, eleveldb::ATOM_ERROR, caller_ref));
    }

    return eleveldb::ATOM_OK;

}   // async_open


ERL_NIF_TERM
async_write(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    const ERL_NIF_TERM& caller_ref = argv[0];
    const ERL_NIF_TERM& handle_ref = argv[1];
    const ERL_NIF_TERM& action_ref = argv[2];
    const ERL_NIF_TERM& opts_ref   = argv[3];

    ReferencePtr<DbObject> db_ptr;

    db_ptr.assign(DbObject::RetrieveDbObject(env, handle_ref));

    if(NULL==db_ptr.get()
       || !enif_is_list(env, action_ref)
       || !enif_is_list(env, opts_ref))
    {
        return enif_make_badarg(env);
    }

    // is this even possible?
    if(NULL == db_ptr->m_Db)
        return send_reply(env, caller_ref, error_einval(env));

    eleveldb_priv_data& priv = *static_cast<eleveldb_priv_data *>(enif_priv_data(env));

    // Construct a write batch:
    leveldb::WriteBatch* batch = new leveldb::WriteBatch;

    // Seed the batch's data:
    ERL_NIF_TERM result = fold(env, argv[2], write_batch_item, *batch);
    if(eleveldb::ATOM_OK != result)
    {
        return send_reply(env, caller_ref,
                          enif_make_tuple3(env, eleveldb::ATOM_ERROR, caller_ref,
                                           enif_make_tuple2(env, eleveldb::ATOM_BAD_WRITE_ACTION,
                                                            result)));
    }   // if

    leveldb::WriteOptions* opts = new leveldb::WriteOptions;
    fold(env, argv[3], parse_write_option, *opts);

    eleveldb::WorkTask* work_item = new eleveldb::WriteTask(env, caller_ref,
                                                            db_ptr.get(), batch, opts);

    if(false == priv.thread_pool.submit(work_item))
    {
        delete work_item;
        return send_reply(env, caller_ref,
                          enif_make_tuple2(env, eleveldb::ATOM_ERROR, caller_ref));
    }   // if

    return eleveldb::ATOM_OK;
}


ERL_NIF_TERM
async_get(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    const ERL_NIF_TERM& caller_ref = argv[0];
    const ERL_NIF_TERM& dbh_ref    = argv[1];
    const ERL_NIF_TERM& key_ref    = argv[2];
    const ERL_NIF_TERM& opts_ref   = argv[3];

    ReferencePtr<DbObject> db_ptr;

    db_ptr.assign(DbObject::RetrieveDbObject(env, dbh_ref));

    if(NULL==db_ptr.get()
       || !enif_is_list(env, opts_ref)
       || !enif_is_binary(env, key_ref))
    {
        return enif_make_badarg(env);
    }

    if(NULL == db_ptr->m_Db)
        return send_reply(env, caller_ref, error_einval(env));

    leveldb::ReadOptions *opts = new leveldb::ReadOptions();
    fold(env, opts_ref, parse_read_option, *opts);

    eleveldb::WorkTask *work_item = new eleveldb::GetTask(env, caller_ref,
                                                          db_ptr.get(), key_ref, opts);

    eleveldb_priv_data& priv = *static_cast<eleveldb_priv_data *>(enif_priv_data(env));

    if(false == priv.thread_pool.submit(work_item))
    {
        delete work_item;
        return send_reply(env, caller_ref,
                          enif_make_tuple2(env, eleveldb::ATOM_ERROR, caller_ref));
    }   // if

    return eleveldb::ATOM_OK;

}   // async_get


ERL_NIF_TERM
async_iterator(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    const ERL_NIF_TERM& caller_ref  = argv[0];
    const ERL_NIF_TERM& dbh_ref     = argv[1];
    const ERL_NIF_TERM& options_ref = argv[2];

    const bool keys_only = ((argc == 4) && (argv[3] == ATOM_KEYS_ONLY));

    ReferencePtr<DbObject> db_ptr;

    db_ptr.assign(DbObject::RetrieveDbObject(env, dbh_ref));

    if(NULL==db_ptr.get()
       || !enif_is_list(env, options_ref))
     {
        return enif_make_badarg(env);
     }

    // likely useless
    if(NULL == db_ptr->m_Db)
        return send_reply(env, caller_ref, error_einval(env));

    // Parse out the read options
    leveldb::ReadOptions *opts = new leveldb::ReadOptions;
    fold(env, options_ref, parse_read_option, *opts);

    eleveldb::WorkTask *work_item = new eleveldb::IterTask(env, caller_ref,
                                                           db_ptr.get(), keys_only, opts);

    // Now-boilerplate setup (we'll consolidate this pattern soon, I hope):
    eleveldb_priv_data& priv = *static_cast<eleveldb_priv_data *>(enif_priv_data(env));

    if(false == priv.thread_pool.submit(work_item))
    {
        delete work_item;
        return send_reply(env, caller_ref, enif_make_tuple2(env, ATOM_ERROR, caller_ref));
    }   // if

    return ATOM_OK;

}   // async_iterator


ERL_NIF_TERM
async_iterator_move(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    // const ERL_NIF_TERM& caller_ref       = argv[0];
    const ERL_NIF_TERM& itr_handle_ref   = argv[1];
    const ERL_NIF_TERM& action_or_target = argv[2];
    ERL_NIF_TERM ret_term;

    bool submit_new_request(true);

    ReferencePtr<ItrObject> itr_ptr;

    itr_ptr.assign(ItrObject::RetrieveItrObject(env, itr_handle_ref));

    if(NULL==itr_ptr.get())
        return enif_make_badarg(env);

    // Reuse ref from iterator creation
    const ERL_NIF_TERM& caller_ref = itr_ptr->m_Snapshot->itr_ref;

    /* We can be invoked with two different arities from Erlang. If our "action_atom" parameter is not
       in fact an atom, then it is actually a seek target. Let's find out which we are: */
    eleveldb::MoveTask::action_t action = eleveldb::MoveTask::SEEK;

    // If we have an atom, it's one of these (action_or_target's value is ignored):
    if(enif_is_atom(env, action_or_target))
    {
        if(ATOM_FIRST == action_or_target)  action = eleveldb::MoveTask::FIRST;
        if(ATOM_LAST == action_or_target)   action = eleveldb::MoveTask::LAST;
        if(ATOM_NEXT == action_or_target)   action = eleveldb::MoveTask::NEXT;
        if(ATOM_PREV == action_or_target)   action = eleveldb::MoveTask::PREV;
        if(ATOM_PREFETCH == action_or_target)   action = eleveldb::MoveTask::PREFETCH;
    }   // if


    //
    // Three situations:
    //  #1 not a PREFETCH next call
    //  #2 PREFETCH call and no prefetch waiting
    //  #3 PREFETCH call and prefetch is waiting

    // case #1
    if (eleveldb::MoveTask::PREFETCH != action)
    {
        // current move object could still be in later stages of
        //  worker thread completion ... race condition ...don't reuse
        itr_ptr->ReleaseReuseMove();

        submit_new_request=true;
        ret_term = enif_make_copy(env, itr_ptr->m_Snapshot->itr_ref);

        // force reply to be a message
        itr_ptr->m_Iter->m_HandoffAtomic=1;
    }   // if

    // case #2
    // before we launch a background job for "next iteration", see if there is a
    //  prefetch waiting for us
    else if (eleveldb::compare_and_swap(&itr_ptr->m_Iter->m_HandoffAtomic, 0, 1))
    {
        // nope, no prefetch ... await a message to erlang queue
        ret_term = enif_make_copy(env, itr_ptr->m_Snapshot->itr_ref);

        // is this truly a wait for prefetch ... or actually the first prefetch request
        if (!itr_ptr->m_Iter->m_PrefetchStarted)
        {
            submit_new_request=true;
            itr_ptr->m_Iter->m_PrefetchStarted=true;
            itr_ptr->ReleaseReuseMove();

            // first must return via message
            itr_ptr->m_Iter->m_HandoffAtomic=1;
        }   // if

        else
        {
            // await message that is already in the making
            submit_new_request=false;
        }   // else
    }   // else if

    // case #3
    else
    {
        // why yes there is.  copy the key/value info into a return tuple before
        //  we launch the iterator for "next" again
        if(!itr_ptr->m_Iter->Valid())
            ret_term=enif_make_tuple2(env, ATOM_ERROR, ATOM_INVALID_ITERATOR);

        else if (itr_ptr->m_Iter->m_KeysOnly)
            ret_term=enif_make_tuple2(env, ATOM_OK, slice_to_binary(env, itr_ptr->m_Iter->key()));
        else
            ret_term=enif_make_tuple3(env, ATOM_OK,
                                      slice_to_binary(env, itr_ptr->m_Iter->key()),
                                      slice_to_binary(env, itr_ptr->m_Iter->value()));

        // reset for next race
        itr_ptr->m_Iter->m_HandoffAtomic=0;

        // old MoveItem could still be active on its thread, cannot
        //  reuse ... but the current Iterator is good
        itr_ptr->ReleaseReuseMove();

        submit_new_request=true;
    }   // else


    // only build request if actually need to submit it
    if (submit_new_request)
    {
        eleveldb::MoveTask * move_item;

        move_item = new eleveldb::MoveTask(env, caller_ref,
                                           itr_ptr->m_Iter.get(), action);

        // prevent deletes during worker loop
        move_item->RefInc();
        itr_ptr->reuse_move=move_item;

        move_item->action=action;

        if (eleveldb::MoveTask::SEEK == action)
        {
            ErlNifBinary key;

            if(!enif_inspect_binary(env, action_or_target, &key))
            {
                itr_ptr->ReleaseReuseMove();
		itr_ptr->reuse_move=NULL;
                return enif_make_tuple2(env, ATOM_EINVAL, caller_ref);
            }   // if

            move_item->seek_target.assign((const char *)key.data, key.size);
        }   // else

        eleveldb_priv_data& priv = *static_cast<eleveldb_priv_data *>(enif_priv_data(env));

        if(false == priv.thread_pool.submit(move_item))
        {
            itr_ptr->ReleaseReuseMove();
	    itr_ptr->reuse_move=NULL;
            return enif_make_tuple2(env, ATOM_ERROR, caller_ref);
        }   // if
    }   // if

    return ret_term;

}   // async_iter_move


} // namespace eleveldb


/***
 * HEY YOU, please convert this to an async operation
 */

ERL_NIF_TERM
eleveldb_close(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    eleveldb::DbObject * db_ptr;
    ERL_NIF_TERM ret_term;

    ret_term=eleveldb::ATOM_OK;

    db_ptr=eleveldb::DbObject::RetrieveDbObject(env, argv[0]);

    if (NULL!=db_ptr)
    {
        // set closing flag
        eleveldb::ErlRefObject::InitiateCloseRequest(db_ptr);

        db_ptr=NULL;

        ret_term=eleveldb::ATOM_OK;
    }   // if
    else
    {
        ret_term=enif_make_badarg(env);
    }   // else

    return(ret_term);

}  // eleveldb_close


ERL_NIF_TERM
eleveldb_iterator_close(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    eleveldb::ItrObject * itr_ptr;
    ERL_NIF_TERM ret_term;

    ret_term=eleveldb::ATOM_OK;

    itr_ptr=eleveldb::ItrObject::RetrieveItrObject(env, argv[0], true);

    if (NULL!=itr_ptr)
    {
        itr_ptr->ReleaseReuseMove();

        // set closing flag ... atomic likely unnecessary (but safer)
        eleveldb::ErlRefObject::InitiateCloseRequest(itr_ptr);

        itr_ptr=NULL;

        ret_term=eleveldb::ATOM_OK;
    }   // if
    else
    {
        ret_term=enif_make_badarg(env);
    }   // else

    return(ret_term);

}   // elveldb_iterator_close


ERL_NIF_TERM
eleveldb_status(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    ErlNifBinary name_bin;
    eleveldb::ReferencePtr<eleveldb::DbObject> db_ptr;

    db_ptr.assign(eleveldb::DbObject::RetrieveDbObject(env, argv[0]));

    if(NULL!=db_ptr.get()
       && enif_inspect_binary(env, argv[1], &name_bin))
    {
        if (db_ptr->m_Db == NULL)
        {
            return error_einval(env);
        }

        leveldb::Slice name((const char*)name_bin.data, name_bin.size);
        std::string value;
        if (db_ptr->m_Db->GetProperty(name, &value))
        {
            ERL_NIF_TERM result;
            unsigned char* result_buf = enif_make_new_binary(env, value.size(), &result);
            memcpy(result_buf, value.c_str(), value.size());

            return enif_make_tuple2(env, eleveldb::ATOM_OK, result);
        }
        else
        {
            return eleveldb::ATOM_ERROR;
        }
    }
    else
    {
        return enif_make_badarg(env);
    }
}   // eleveldb_status


ERL_NIF_TERM
eleveldb_repair(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    char name[4096];
    if (enif_get_string(env, argv[0], name, sizeof(name), ERL_NIF_LATIN1))
    {
        // Parse out the options
        leveldb::Options opts;

        leveldb::Status status = leveldb::RepairDB(name, opts);
        if (!status.ok())
        {
            return error_tuple(env, eleveldb::ATOM_ERROR_DB_REPAIR, status);
        }
        else
        {
            return eleveldb::ATOM_OK;
        }
    }
    else
    {
        return enif_make_badarg(env);
    }
}   // eleveldb_repair

/**
 * HEY YOU ... please make async
 */
ERL_NIF_TERM
eleveldb_destroy(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    char name[4096];
    if (enif_get_string(env, argv[0], name, sizeof(name), ERL_NIF_LATIN1) &&
        enif_is_list(env, argv[1]))
    {
        // Parse out the options
        leveldb::Options opts;
        fold(env, argv[1], parse_open_option, opts);

        leveldb::Status status = leveldb::DestroyDB(name, opts);
        if (!status.ok())
        {
            return error_tuple(env, eleveldb::ATOM_ERROR_DB_DESTROY, status);
        }
        else
        {
            return eleveldb::ATOM_OK;
        }
    }
    else
    {
        return enif_make_badarg(env);
    }

}   // eleveldb_destroy


ERL_NIF_TERM
eleveldb_is_empty(
    ErlNifEnv* env,
    int argc,
    const ERL_NIF_TERM argv[])
{
    eleveldb::ReferencePtr<eleveldb::DbObject> db_ptr;

    db_ptr.assign(eleveldb::DbObject::RetrieveDbObject(env, argv[0]));

    if(NULL!=db_ptr.get())
    {
        if (db_ptr->m_Db == NULL)
        {
            return error_einval(env);
        }

        leveldb::ReadOptions opts;
        leveldb::Iterator* itr = db_ptr->m_Db->NewIterator(opts);
        itr->SeekToFirst();
        ERL_NIF_TERM result;
        if (itr->Valid())
        {
            result = eleveldb::ATOM_FALSE;
        }
        else
        {
            result = eleveldb::ATOM_TRUE;
        }
        delete itr;

        return result;
    }
    else
    {
        return enif_make_badarg(env);
    }
}   // eleveldb_is_empty


static void on_unload(ErlNifEnv *env, void *priv_data)
{
    eleveldb_priv_data *p = static_cast<eleveldb_priv_data *>(priv_data);
    delete p;
}


static int on_load(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info)
try
{
    *priv_data = 0;

    // inform erlang of our two resource types
    eleveldb::DbObject::CreateDbObjectType(env);
    eleveldb::ItrObject::CreateItrObjectType(env);

    /* Gather local initialization data: */
    struct _local
    {
        int n_threads;

        _local()
         : n_threads(0)
        {}
    } local;

    /* Seed our private data with appropriate values: */
    if(!enif_is_list(env, load_info))
        throw std::invalid_argument("on_load::load_info");

    ERL_NIF_TERM load_info_head;

    while(0 != enif_get_list_cell(env, load_info, &load_info_head, &load_info))
     {
        int arity = 0;
        ERL_NIF_TERM *tuple_data;

        // Pick out "{write_threads, N}":
        if(enif_get_tuple(env, load_info_head, &arity, const_cast<const ERL_NIF_TERM **>(&tuple_data)))
         {
            if(2 != arity)
             continue;

            unsigned int atom_len;
            if(0 == enif_get_atom_length(env, tuple_data[0], &atom_len, ERL_NIF_LATIN1))
             continue;

            const unsigned int atom_max = 128;
            char atom[atom_max];
            if((atom_len + 1) != static_cast<unsigned int>(enif_get_atom(env, tuple_data[0], atom, atom_max, ERL_NIF_LATIN1)))
             continue;

            if(0 != strncmp(atom, "write_threads", atom_max))
             continue;

            // We have a setting, now peek at the parameter:
            if(0 == enif_get_int(env, tuple_data[1], &local.n_threads))
             throw std::invalid_argument("on_load::n_threads");

            if(0 >= local.n_threads || eleveldb::N_THREADS_MAX < static_cast<size_t>(local.n_threads))
             throw std::range_error("on_load::n_threads");
         }
     }

    /* Spin up the thread pool, set up all private data: */
    eleveldb_priv_data *priv = new eleveldb_priv_data(local.n_threads);

    *priv_data = priv;

    // Initialize common atoms

#define ATOM(Id, Value) { Id = enif_make_atom(env, Value); }
    ATOM(eleveldb::ATOM_OK, "ok");
    ATOM(eleveldb::ATOM_ERROR, "error");
    ATOM(eleveldb::ATOM_EINVAL, "einval");
    ATOM(eleveldb::ATOM_BADARG, "badarg");
    ATOM(eleveldb::ATOM_TRUE, "true");
    ATOM(eleveldb::ATOM_FALSE, "false");
    ATOM(eleveldb::ATOM_CREATE_IF_MISSING, "create_if_missing");
    ATOM(eleveldb::ATOM_ERROR_IF_EXISTS, "error_if_exists");
    ATOM(eleveldb::ATOM_WRITE_BUFFER_SIZE, "write_buffer_size");
    ATOM(eleveldb::ATOM_MAX_OPEN_FILES, "max_open_files");
    ATOM(eleveldb::ATOM_BLOCK_SIZE, "block_size");
    ATOM(eleveldb::ATOM_SST_BLOCK_SIZE, "sst_block_size");
    ATOM(eleveldb::ATOM_BLOCK_RESTART_INTERVAL, "block_restart_interval");
    ATOM(eleveldb::ATOM_ERROR_DB_OPEN,"db_open");
    ATOM(eleveldb::ATOM_ERROR_DB_PUT, "db_put");
    ATOM(eleveldb::ATOM_NOT_FOUND, "not_found");
    ATOM(eleveldb::ATOM_VERIFY_CHECKSUMS, "verify_checksums");
    ATOM(eleveldb::ATOM_FILL_CACHE,"fill_cache");
    ATOM(eleveldb::ATOM_SYNC, "sync");
    ATOM(eleveldb::ATOM_ERROR_DB_DELETE, "db_delete");
    ATOM(eleveldb::ATOM_CLEAR, "clear");
    ATOM(eleveldb::ATOM_PUT, "put");
    ATOM(eleveldb::ATOM_DELETE, "delete");
    ATOM(eleveldb::ATOM_ERROR_DB_WRITE, "db_write");
    ATOM(eleveldb::ATOM_BAD_WRITE_ACTION, "bad_write_action");
    ATOM(eleveldb::ATOM_KEEP_RESOURCE_FAILED, "keep_resource_failed");
    ATOM(eleveldb::ATOM_ITERATOR_CLOSED, "iterator_closed");
    ATOM(eleveldb::ATOM_FIRST, "first");
    ATOM(eleveldb::ATOM_LAST, "last");
    ATOM(eleveldb::ATOM_NEXT, "next");
    ATOM(eleveldb::ATOM_PREV, "prev");
    ATOM(eleveldb::ATOM_PREFETCH, "prefetch");
    ATOM(eleveldb::ATOM_INVALID_ITERATOR, "invalid_iterator");
    ATOM(eleveldb::ATOM_CACHE_SIZE, "cache_size");
    ATOM(eleveldb::ATOM_PARANOID_CHECKS, "paranoid_checks");
    ATOM(eleveldb::ATOM_VERIFY_COMPACTIONS, "verify_compactions");
    ATOM(eleveldb::ATOM_ERROR_DB_DESTROY, "error_db_destroy");
    ATOM(eleveldb::ATOM_ERROR_DB_REPAIR, "error_db_repair");
    ATOM(eleveldb::ATOM_KEYS_ONLY, "keys_only");
    ATOM(eleveldb::ATOM_COMPRESSION, "compression");
    ATOM(eleveldb::ATOM_USE_BLOOMFILTER, "use_bloomfilter");

#undef ATOM

    return 0;
}


catch(std::exception& e)
{
    /* Refuse to load the NIF module (I see no way right now to return a more specific exception
    or log extra information): */
    return -1;
}
catch(...)
{
    return -1;
}


extern "C" {
    ERL_NIF_INIT(eleveldb, nif_funcs, &on_load, NULL, NULL, &on_unload);
}
