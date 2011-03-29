/*
 * Copyright (C) 2005-2010 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 *
 */

#include "config.h"

#include <string.h>

#include "db.h"
#include "device.h"
#include "env.h"
#include "error.h"
#include "log.h"
#include "mem.h"
#include "os.h"
#include "txn.h"
#include "util.h"

#define LOG_DEFAULT_THRESHOLD   64

static ham_status_t 
__undo(ham_log_t *log, log_iterator_t *iter, 
        ham_offset_t page_id, ham_u8_t **pdata)
{
    int i;
	int found=0;
    ham_status_t st=0;
    log_entry_t entry;
    ham_u8_t *data=0;
    ham_offset_t fpos[2];

    /* first, backup the current file pointers */
    for (i=0; i<2; i++) {
        st=os_tell(log_get_fd(log, i), &fpos[i]);
        if (st)
            return (st);
    }

    /*
     * walk backwards through the log and fetch either
     *  - the next before-image OR
     *  - the next after-image of a committed txn
     * and overwrite the file contents
     */
    while (1) {
        if ((st=ham_log_get_entry(log, iter, &entry, &data)))
            goto bail;
        
        if (log_entry_get_lsn(&entry)==0)
            break;

        /* a before-image */
        if (log_entry_get_type(&entry)==LOG_ENTRY_TYPE_PREWRITE
                    && log_entry_get_offset(&entry)==page_id) {
            *pdata=data;
            found=1;
            break;
        }
        /* an after-image; currently, only after-images of committed
         * transactions are written to the log */
        else if (log_entry_get_type(&entry)==LOG_ENTRY_TYPE_WRITE
                    && log_entry_get_offset(&entry)==page_id) {
            *pdata=data;
            found=1;
            break;
        }

        if (data) {
            allocator_free(log_get_allocator(log), data);
            data=0;
        }
    }

    ham_assert(found, ("failed to undo a log entry"));

bail:
    /*
     * done - restore the file pointers
     */
    for (i=0; i<2; i++)
        (void)os_seek(log_get_fd(log, i), fpos[i], HAM_OS_SEEK_SET);

    if (st && data)
        allocator_free(log_get_allocator(log), data);
    
    return (st);
}

static ham_size_t 
my_get_aligned_entry_size(ham_size_t data_size)
{
    ham_size_t s=sizeof(log_entry_t)+data_size;
    s += 8-1;
	s -= (s % 8);
    return (s);
}

static ham_status_t
my_log_clear_file(ham_log_t *log, int idx)
{
    ham_status_t st;

    st=os_truncate(log_get_fd(log, idx), sizeof(log_header_t));
    if (st)
        return (st);

    /* after truncate, the file pointer is far beyond the new end of file;
     * reset the file pointer, or the next write will resize the file to
     * the original size */
    st=os_seek(log_get_fd(log, idx), sizeof(log_header_t), HAM_OS_SEEK_SET);
    if (st)
        return (st);

    /* clear the transaction counters */
    log_set_open_txn(log, idx, 0);
    log_set_closed_txn(log, idx, 0);

    return (0);
}

static ham_status_t
my_insert_checkpoint(ham_log_t *log, ham_env_t *env)
{
    ham_status_t st;
    
    /*
     * first, flush the database file; then append the checkpoint
     *
     * for this flush, we don't need to insert LOG_ENTRY_TYPE_FLUSH_PAGE;
     * therefore, set the state of the log accordingly. the page_flush()
     * routine can then check the state and not write logfile-entries
     * for each flush
     */
    log_set_state(log, log_get_state(log)|LOG_STATE_CHECKPOINT);
    st=ham_env_flush(env, 0);
    log_set_state(log, log_get_state(log)&~LOG_STATE_CHECKPOINT);
    if (st)
        return (st);

    return (ham_log_append_checkpoint(log));
}

ham_status_t
ham_log_create(mem_allocator_t *alloc, ham_env_t *env, const char *dbpath, 
        ham_u32_t mode, ham_u32_t flags, ham_log_t **plog)
{
    int i;
    log_header_t header;
    ham_status_t st;
    char filename[HAM_OS_MAX_PATH];
    ham_log_t *log=(ham_log_t *)allocator_alloc(alloc, sizeof(ham_log_t));
    if (!log)
        return (HAM_OUT_OF_MEMORY);
    memset(log, 0, sizeof(ham_log_t));

    *plog=0;

	ham_assert(env, (0));

    log_set_allocator(log, alloc);
	log_set_env(log, env);
    log_set_lsn(log, 1);
    log_set_flags(log, flags);
    log_set_threshold(log, LOG_DEFAULT_THRESHOLD);

    /* create the two files */
    util_snprintf(filename, sizeof(filename), "%s.log%d", dbpath, 0);
    st=os_create(filename, 0, mode, &log_get_fd(log, 0));
    if (st) {
        allocator_free(alloc, log);
        return (st);
    }

    util_snprintf(filename, sizeof(filename), "%s.log%d", dbpath, 1);
    st=os_create(filename, 0, mode, &log_get_fd(log, 1));
    if (st) {
        os_close(log_get_fd(log, 0), 0);
        allocator_free(alloc, log);
        return (st);
    }

    /* write the magic to both files */
    memset(&header, 0, sizeof(header));
    log_header_set_magic(&header, HAM_LOG_HEADER_MAGIC);

    for (i=0; i<2; i++) {
        st=os_write(log_get_fd(log, i), &header, sizeof(header));
        if (st) {
            (void)ham_log_close(log, HAM_FALSE);
            return (st);
        }
    }

    *plog=log;
    return (st);
}

ham_status_t
ham_log_open(mem_allocator_t *alloc, ham_env_t *env, const char *dbpath, 
        ham_u32_t flags, ham_log_t **plog)
{
    int i;
    log_header_t header;
    log_entry_t entry;
    ham_u64_t lsn[2];
    ham_status_t st;
    char filename[HAM_OS_MAX_PATH];
    ham_log_t *log=(ham_log_t *)allocator_alloc(alloc, sizeof(ham_log_t));
    if (!log)
        return (HAM_OUT_OF_MEMORY);
    memset(log, 0, sizeof(ham_log_t));

    *plog=0;

	ham_assert(env, (0));

    log_set_allocator(log, alloc);
	log_set_env(log, env);
    log_set_flags(log, flags);

    /* open the two files */
    util_snprintf(filename, sizeof(filename), "%s.log%d", dbpath, 0);
    st=os_open(filename, 0, &log_get_fd(log, 0));
    if (st) {
        allocator_free(alloc, log);
        return (st);
    }

    util_snprintf(filename, sizeof(filename), "%s.log%d", dbpath, 1);
    st=os_open(filename, 0, &log_get_fd(log, 1));
    if (st) {
        os_close(log_get_fd(log, 0), 0);
        allocator_free(alloc, log);
        return (st);
    }

    /* check the magic in both files */
    memset(&header, 0, sizeof(header));
    for (i=0; i<2; i++) {
        st=os_pread(log_get_fd(log, i), 0, &header, sizeof(header));
        if (st) {
            (void)ham_log_close(log, HAM_FALSE);
            return (st);
        }
        if (log_header_get_magic(&header)!=HAM_LOG_HEADER_MAGIC) {
            ham_trace(("logfile has unknown magic or is corrupt"));
            (void)ham_log_close(log, HAM_FALSE);
            return (HAM_LOG_INV_FILE_HEADER);
        }
    }

    /* now read the LSN's of both files; the file with the older
     * LSN becomes file[0] */
    for (i=0; i<2; i++) {
        /* but make sure that the file is large enough! */
        ham_offset_t size;
        st=os_get_filesize(log_get_fd(log, i), &size);
        if (st) {
            (void)ham_log_close(log, HAM_FALSE);
            return (st);
        }

        if (size>=sizeof(entry)) {
            st=os_pread(log_get_fd(log, i), size-sizeof(log_entry_t), 
                            &entry, sizeof(entry));
            if (st) {
                (void)ham_log_close(log, HAM_FALSE);
                return (st);
            }
            lsn[i]=log_entry_get_lsn(&entry);
        }
        else
            lsn[i]=0;
    }

    if (lsn[1]>lsn[0]) {
        ham_fd_t temp=log_get_fd(log, 0);
        log_set_fd(log, 0, log_get_fd(log, 1));
        log_set_fd(log, 1, temp);
    }

    *plog=log;
    return (0);
}

ham_status_t
ham_log_is_empty(ham_log_t *log, ham_bool_t *isempty)
{
    ham_status_t st; 
    ham_offset_t size;
    int i;

    for (i=0; i<2; i++) {
        st=os_get_filesize(log_get_fd(log, i), &size);
        if (st)
            return (st);
        if (size && size!=sizeof(log_header_t)) {
            *isempty=HAM_FALSE;
            return (0);
        }
    }

    *isempty=HAM_TRUE;
    return (0);
}

ham_status_t
ham_log_append_entry(ham_log_t *log, int fdidx, log_entry_t *entry, 
        ham_size_t size)
{
    ham_status_t st;

    st=os_write(log_get_fd(log, fdidx), entry, size);
    if (st)
        return (st);

    st=os_flush(log_get_fd(log, fdidx));
    return (st);
}

ham_status_t
ham_log_flush(ham_log_t *log, int fdidx)
{
    return (os_flush(log_get_fd(log, fdidx)));
}

ham_status_t
ham_log_append_txn_begin(ham_log_t *log, struct ham_txn_t *txn)
{
    ham_status_t st;
    log_entry_t entry;
    int cur=log_get_current_fd(log);
    int other=cur ? 0 : 1;

    memset(&entry, 0, sizeof(entry));
    log_entry_set_txn_id(&entry, txn_get_id(txn));
    log_entry_set_type(&entry, LOG_ENTRY_TYPE_TXN_BEGIN);

    /* 
     * determine the log file which is used for this transaction 
     *
     * if the "current" file is not yet full, continue to write to this file
     */
    if (log_get_open_txn(log, cur)+log_get_closed_txn(log, cur)<
            log_get_threshold(log)) {
        txn_set_log_desc(txn, cur);
    }
    else if (log_get_open_txn(log, other)==0) 
	{
		/*
		 * otherwise, if the other file does no longer have open transactions,
		 * insert a checkpoint, delete the other file and use the other file
		 * as the current file
		 */

		/* checkpoint! */
        st=my_insert_checkpoint(log, txn_get_env(txn));
        if (st)
            return (st);
        /* now clear the other file */
        st=my_log_clear_file(log, other);
        if (st)
            return (st);
        /* continue writing to the other file */
        cur=other;
        log_set_current_fd(log, cur);
        txn_set_log_desc(txn, cur);
    }
    /*
     * otherwise continue writing to the current file, till the other file
     * can be deleted safely
     */
    else {
        txn_set_log_desc(txn, cur);
    }

    /*
     * now set the lsn (it might have been modified in 
     * my_insert_checkpoint())
     */
    log_entry_set_lsn(&entry, log_get_lsn(log));
    log_increment_lsn(log);

    st=ham_log_append_entry(log, cur, &entry, sizeof(entry));
    if (st)
        return (st);
    log_set_open_txn(log, cur, log_get_open_txn(log, cur)+1);

    /* store the fp-index in the log structure; it's needed so
     * log_append_checkpoint() can quickly find out which file is 
     * the newest */
    log_set_current_fd(log, cur);

    return (0);
}

ham_status_t
ham_log_append_txn_abort(ham_log_t *log, struct ham_txn_t *txn)
{
    int idx;
    ham_status_t st;
    ham_status_t st2;
    log_entry_t entry;

    memset(&entry, 0, sizeof(entry));
    log_entry_set_lsn(&entry, log_get_lsn(log));
    log_increment_lsn(log);
    log_entry_set_txn_id(&entry, txn_get_id(txn));
    log_entry_set_type(&entry, LOG_ENTRY_TYPE_TXN_ABORT);

    /*
     * update the transaction counters of this logfile
     */
    idx=txn_get_log_desc(txn);
    log_set_open_txn(log, idx, log_get_open_txn(log, idx)-1);
    log_set_closed_txn(log, idx, log_get_closed_txn(log, idx)+1);

    st=ham_log_append_entry(log, idx, &entry, sizeof(entry));

	/* 
	do we need to flush an ABORTED transaction to disc to make it work?

	Yes, we do. How would we know for sure the TXN is aborted, otherwise?
	*/
	st2 = ham_log_flush(log, idx);
	if (!st)
        st = st2;

	return st;
}

ham_status_t
ham_log_append_txn_commit(ham_log_t *log, struct ham_txn_t *txn)
{
    int idx;
    ham_status_t st;
    ham_status_t st2;
    log_entry_t entry;

    memset(&entry, 0, sizeof(entry));
    log_entry_set_lsn(&entry, log_get_lsn(log));
    log_increment_lsn(log);
    log_entry_set_txn_id(&entry, txn_get_id(txn));
    log_entry_set_type(&entry, LOG_ENTRY_TYPE_TXN_COMMIT);

    /*
     * update the transaction counters of this logfile
     */
    idx=txn_get_log_desc(txn);
    log_set_open_txn(log, idx, log_get_open_txn(log, idx)-1);
    log_set_closed_txn(log, idx, log_get_closed_txn(log, idx)+1);

    st=ham_log_append_entry(log, idx, &entry, sizeof(entry));

	/* 
	we MUST flush a COMMITTED transaction to disc to make it work in recovery,
	else we risk an incomplete log file while the commit came through.
	*/
	st2 = ham_log_flush(log, idx);
	if (st == 0)
        st = st2;

    return st;
}

ham_status_t
ham_log_append_checkpoint(ham_log_t *log)
{
    ham_status_t st;
    log_entry_t entry;

    memset(&entry, 0, sizeof(entry));
    log_entry_set_lsn(&entry, log_get_lsn(log));
    log_increment_lsn(log);
    log_entry_set_type(&entry, LOG_ENTRY_TYPE_CHECKPOINT);

    /* always write the checkpoint to the newer file */
    st=ham_log_append_entry(log, log_get_current_fd(log), 
            &entry, sizeof(entry));
    if (st)
        return (st);

    log_set_last_checkpoint_lsn(log, log_get_lsn(log)-1);

    return (0);
}

ham_status_t
ham_log_append_flush_page(ham_log_t *log, struct ham_page_t *page)
{
    int fdidx=log_get_current_fd(log);
    ham_status_t st;
    log_entry_t entry;

	ham_env_t *env = device_get_env(page_get_device(page));
	ham_assert(page_is_dirty(page), (0));

	/* make sure that this is never called during a checkpoint! */
    ham_assert(!(log_get_state(log)&LOG_STATE_CHECKPOINT), (0));
    
	ham_assert(page_get_device(page), (0));
	ham_assert(device_get_env(page_get_device(page)), (0));

    /* write the header */
    memset(&entry, 0, sizeof(entry));
    log_entry_set_lsn(&entry, log_get_lsn(log));
    log_increment_lsn(log);
    log_entry_set_type(&entry, LOG_ENTRY_TYPE_FLUSH_PAGE);
    log_entry_set_offset(&entry, page_get_self(page));

    if (env_get_txn(env))
        fdidx=txn_get_log_desc(env_get_txn(env)); 

    st=ham_log_append_entry(log, fdidx, &entry, sizeof(entry));
    if (st)
        return (st);

	st = ham_log_flush(log, fdidx);

    return st;
}

ham_status_t
ham_log_append_write(ham_log_t *log, ham_txn_t *txn, ham_offset_t offset,
                ham_u8_t *data, ham_size_t size)
{
    ham_status_t st;
    ham_size_t alloc_size=my_get_aligned_entry_size(size);
    log_entry_t *entry;
    ham_u8_t *alloc_buf;
    
    alloc_buf=allocator_alloc(log_get_allocator(log), alloc_size);
    if (!alloc_buf)
        return (HAM_OUT_OF_MEMORY);

    entry=(log_entry_t *)(alloc_buf+alloc_size-sizeof(log_entry_t));

    memset(entry, 0, sizeof(*entry));
    log_entry_set_lsn(entry, log_get_lsn(log));
    log_increment_lsn(log);
    if (txn)
        log_entry_set_txn_id(entry, txn_get_id(txn));
    log_entry_set_type(entry, LOG_ENTRY_TYPE_WRITE);
    log_entry_set_offset(entry, offset);
    log_entry_set_data_size(entry, size);
    memcpy(alloc_buf, data, size);

    st=ham_log_append_entry(log, 
                    txn ? txn_get_log_desc(txn) : log_get_current_fd(log), 
                    (log_entry_t *)alloc_buf, alloc_size);
    allocator_free(log_get_allocator(log), alloc_buf);
    if (st)
        return (st);

    return (0);
}

ham_status_t
ham_log_append_prewrite(ham_log_t *log, ham_txn_t *txn, ham_offset_t offset,
                ham_u8_t *data, ham_size_t size)
{
    ham_status_t st;
    ham_size_t alloc_size=my_get_aligned_entry_size(size);
    log_entry_t *entry;
    ham_u8_t *alloc_buf;
    
    alloc_buf=allocator_alloc(log_get_allocator(log), alloc_size);
    if (!alloc_buf)
        return (HAM_OUT_OF_MEMORY);

    entry=(log_entry_t *)(alloc_buf+alloc_size-sizeof(log_entry_t));

    memset(entry, 0, sizeof(*entry));
    log_entry_set_lsn(entry, log_get_lsn(log));
    log_increment_lsn(log);
    if (txn)
        log_entry_set_txn_id(entry, txn_get_id(txn));
    log_entry_set_type(entry, LOG_ENTRY_TYPE_PREWRITE);
    log_entry_set_offset(entry, offset);
    log_entry_set_data_size(entry, size);
    memcpy(alloc_buf, data, size);

    st=ham_log_append_entry(log, 
                    txn ? txn_get_log_desc(txn) : log_get_current_fd(log), 
                    (log_entry_t *)alloc_buf, alloc_size);
    allocator_free(log_get_allocator(log), alloc_buf);
    if (st)
        return (st);

    return (0);
}

ham_status_t
ham_log_clear(ham_log_t *log)
{
    ham_status_t st; 
    int i;

    for (i=0; i<2; i++) {
        if ((st=my_log_clear_file(log, i)))
            return (st);
    }

    return (0);
}

ham_status_t
ham_log_get_entry(ham_log_t *log, log_iterator_t *iter, log_entry_t *entry,
                ham_u8_t **data)
{
    ham_status_t st;

    *data=0;

    /*
     * start with the current file
     */
    if (!iter->_offset) {
        iter->_fdstart=iter->_fdidx=log_get_current_fd(log);
        st=os_get_filesize(log_get_fd(log, iter->_fdidx), &iter->_offset);
        if (st)
            return (st);
    }

    /* 
     * if the current file is empty: try to continue with the other file
     */
    if (iter->_offset<=sizeof(log_header_t)) {
        if (iter->_fdidx!=iter->_fdstart) {
            log_entry_set_lsn(entry, 0);
            return (0);
        }
        iter->_fdidx=(iter->_fdidx==0 ? 1 : 0);
        st=os_get_filesize(log_get_fd(log, iter->_fdidx), &iter->_offset);
        if (st)
            return (st);
    }

    if (iter->_offset<=sizeof(log_header_t)) {
        log_entry_set_lsn(entry, 0);
        return (0);
    }

    /*
     * now read the entry-header from the file
     */
    iter->_offset-=sizeof(log_entry_t);

    st=os_pread(log_get_fd(log, iter->_fdidx), iter->_offset, 
                    entry, sizeof(*entry));
    if (st)
        return (st);

    /*
     * now read the data
     */
    if (log_entry_get_data_size(entry)) {
        ham_offset_t pos=iter->_offset-log_entry_get_data_size(entry);
        // pos += 8-1;
		pos -= (pos % 8);

        *data=allocator_alloc(log_get_allocator(log), 
                        (ham_size_t)log_entry_get_data_size(entry));
        if (!*data)
            return (HAM_OUT_OF_MEMORY);

        st=os_pread(log_get_fd(log, iter->_fdidx), pos, *data, 
                    (ham_size_t)log_entry_get_data_size(entry));
        if (st) {
            allocator_free(log_get_allocator(log), *data);
            *data=0;
            return (st);
        }

        iter->_offset=pos;
    }
    else
        *data=0;

    return (0);
}

ham_status_t
ham_log_close(ham_log_t *log, ham_bool_t noclear)
{
    ham_status_t st = 0; 
	ham_status_t st2 = 0; 
    int i;

    if (!noclear) {
        st2 = ham_log_clear(log); /* [i_a] track status anyhow? */
#if 0 /* [i_a] failing to 'clear' will be marked through ST, but should NOT abort the close procedure: complete this anyhow */
        if (st2)
            return (st2);
#endif
    }

    for (i=0; i<2; i++) {
        if (log_get_fd(log, i)!=HAM_INVALID_FD) {
            if ((st=os_close(log_get_fd(log, i), 0)))
                return (st);
            log_set_fd(log, i, HAM_INVALID_FD);
        }
    }

    if (log_get_overwrite_data(log)) {
        allocator_free(log_get_allocator(log), log_get_overwrite_data(log));
        log_set_overwrite_data(log, 0);
        log_set_overwrite_size(log, 0);
    }
    allocator_free(log_get_allocator(log), log);
	return st2;
}

ham_status_t
ham_log_add_page_before(ham_page_t *page)
{
    ham_status_t st=0;
    ham_env_t *env=device_get_env(page_get_device(page));
    ham_log_t *log=env_get_log(env);
    ham_file_filter_t *head=env_get_file_filter(env);
    ham_u8_t *p;
    ham_size_t size=env_get_pagesize(env);

    if (!log)
        return (0);

    /*
     * only write the before-image, if it was not yet written
     * since the last checkpoint
     */
    if (page_get_before_img_lsn(page)>log_get_last_checkpoint_lsn(log))
        return (0);

    /*
     * run page through page-level filters, but not for the 
     * root-page!
     */
    if (head && page_get_self(page)!=0) {
        p=(ham_u8_t *)allocator_alloc(log_get_allocator(log), size);
        if (!p)
            return (HAM_OUT_OF_MEMORY);
        memcpy(p, page_get_raw_payload(page), size);

        while (head) {
            if (head->before_write_cb) {
                st=head->before_write_cb(env, head, p, size);
                if (st) 
                    break;
            }
            head=head->_next;
        }
    }
    else
        p=(ham_u8_t *)page_get_raw_payload(page);

    if (st==0)
        st=ham_log_append_prewrite(log, env_get_txn(env), 
                page_get_self(page), p, size);

    if (p!=page_get_raw_payload(page))
        allocator_free(log_get_allocator(log), p);

    if (st) 
        return st;

    page_set_before_img_lsn(page, log_get_lsn(log)-1);
    return (0);
}

ham_status_t
ham_log_add_page_after(ham_page_t *page)
{
    ham_status_t st=0;
    ham_env_t *env=device_get_env(page_get_device(page));
    ham_log_t *log=env_get_log(env);
    ham_file_filter_t *head=env_get_file_filter(env);
    ham_u8_t *p;
    ham_size_t size=env_get_pagesize(env);

    if (!log)
        return (0);

    /*
     * run page through page-level filters, but not for the 
     * root-page!
     */
    if (head && page_get_self(page)!=0) {
        p=(ham_u8_t *)allocator_alloc(log_get_allocator(log), 
                env_get_pagesize(env));
        if (!p)
            return (HAM_OUT_OF_MEMORY);
        memcpy(p, page_get_raw_payload(page), size);

        while (head) {
            if (head->before_write_cb) {
                st=head->before_write_cb(env, head, p, size);
                if (st) 
                    break;
            }
            head=head->_next;
        }
    }
    else
        p=(ham_u8_t *)page_get_raw_payload(page);

    if (st==0)
        st=ham_log_append_write(log, env_get_txn(env), 
                page_get_self(page), p, size);

    if (p!=page_get_raw_payload(page))
        allocator_free(log_get_allocator(log), p);

    return (st);
}

/*
 * a PAGE_FLUSH entry; each entry stores the 
 * page-ID and the lsn of the last flush of this page
 */
typedef struct
{
    ham_u64_t page_id;
    ham_u64_t lsn;
} log_flush_entry_t;

ham_status_t
ham_log_recover(ham_log_t *log, ham_device_t *device, ham_env_t *env)
{
    ham_status_t st=0;
    log_entry_t entry;
    log_iterator_t iter;
    ham_u8_t *data=0;
    ham_u64_t *txn_list=0;
    log_flush_entry_t *flush_list=0;
    ham_size_t txn_list_size=0;
	ham_size_t flush_list_size=0;
	ham_size_t i;
    ham_bool_t committed;
	ham_bool_t flushed;

    memset(&iter, 0, sizeof(iter));

    /*
     * walk backwards through the log; every action which was not
     * committed, but flushed, is undone; every action which was 
     * committed, but not flushed, is redone
     */
    while (1) {
        if ((st=ham_log_get_entry(log, &iter, &entry, &data)))
            goto bail;
        
        if (log_entry_get_lsn(&entry)==0)
            break;

        switch (log_entry_get_type(&entry)) {
            /* checkpoint: no need to continue */
            case LOG_ENTRY_TYPE_CHECKPOINT:
                goto bail;

            /* commit: store the txn-id */
            case LOG_ENTRY_TYPE_TXN_COMMIT:
                txn_list_size++;
                txn_list=(ham_u64_t *)allocator_realloc(log_get_allocator(log),
                        txn_list, txn_list_size*sizeof(ham_u64_t));
                if (!txn_list) {
                    st=HAM_OUT_OF_MEMORY;
                    goto bail;
                }
                txn_list[txn_list_size-1]=log_entry_get_txn_id(&entry);
                break;

            /* an after-image: undo if flushed but not committed, 
             * redo if committed and not flushed */
            case LOG_ENTRY_TYPE_WRITE:
                /* 
				check if this page was flushed at a later time within 
				the same log section (up to the next checkpoint): we're 
				walking BACKWARDS in time here and we must only restore
				the LATEST state.
				*/
                flushed=0;
                for (i=0; i<flush_list_size; i++) {
                    if (flush_list[i].page_id==log_entry_get_offset(&entry)
                            && flush_list[i].lsn>log_entry_get_lsn(&entry)) {
                        flushed=1;
                        break;
                    }
                }
                /* check if this txn was committed */
                committed=0;
                for (i=0; i<txn_list_size; i++) {
                    if (txn_list[i]==log_entry_get_txn_id(&entry)) {
                        committed=1;
                        break;
                    }
                }

                /* flushed and not committed: undo */
                if (flushed && !committed) {
                    ham_u8_t *udata;
                    log_iterator_t uiter=iter;
                    st=__undo(log, &uiter, 
                            log_entry_get_offset(&entry), &udata);
                    if (st)
                        goto bail;
                    st=device->write(device, log_entry_get_offset(&entry),
                            udata, env_get_pagesize(env));
                    allocator_free(log_get_allocator(log), udata);
                    if (st)
                        goto bail;
                    break;
                }
                /* not flushed and committed: redo */
                else if (!flushed && committed) {
                    st=device->write(device, log_entry_get_offset(&entry),
                            data, env_get_pagesize(env));
                    if (st)
                        goto bail;
                    /* since we just flushed the page: add page_id and lsn
                     * to the flush_list 
                     *
                     * fall through...
                     */

                }
                else
                    break;
            /* flush: store the page-id and the lsn*/
            case LOG_ENTRY_TYPE_FLUSH_PAGE:
                flush_list_size++;
                flush_list=(log_flush_entry_t *)allocator_realloc(
                        log_get_allocator(log), flush_list, 
                        flush_list_size*sizeof(log_flush_entry_t));
                if (!flush_list) {
                    st=HAM_OUT_OF_MEMORY;
                    goto bail;
                }
                flush_list[flush_list_size-1].page_id=
                    log_entry_get_offset(&entry);
                flush_list[flush_list_size-1].lsn=
                    log_entry_get_lsn(&entry);
                break;

            /* ignore everything else */
            default:
                break;
        }

        if (data) {
            allocator_free(log_get_allocator(log), data);
            data=0;
        }
    }

bail:
    if (txn_list)
        allocator_free(log_get_allocator(log), txn_list);
    if (flush_list)
        allocator_free(log_get_allocator(log), flush_list);
    if (data)
        allocator_free(log_get_allocator(log), data);

    /*
     * clear the log files and set the lsn to 1
     */
    st=ham_log_clear(log);
    if (st) {
        ham_log(("unable to clear logfiles; please manually delete the "
                "log files before re-opening the Database"));
        return (st);
    }

    log_set_lsn(log, 1);
    log_set_current_fd(log, 0);
    return (0);
}

ham_status_t
ham_log_recreate(ham_log_t *log, ham_page_t *page)
{
    ham_status_t st;
    log_iterator_t iter;
    ham_u8_t *data;
	ham_env_t *env=device_get_env(page_get_device(page));

    memset(&iter, 0, sizeof(iter));

    st=__undo(log, &iter, page_get_self(page), &data);
    if (st)
        return (st);

    memcpy(page_get_raw_payload(page), data, env_get_pagesize(env));
    allocator_free(log_get_allocator(log), data);
	/* make sure the old data will be flushed to disk, later on */
	ham_assert(page_is_dirty(page), (0));

    return (0);
}

void
ham_log_mark_db_expansion_start(ham_env_t *env)
{
	ham_log_t *log = env_get_log(env);

	if (!log)
		return;

	log_set_state(log, log_get_state(log) | LOG_STATE_DB_EXPANSION);

	return;
}

void
ham_log_mark_db_expansion_end(ham_env_t *env)
{
	ham_log_t *log = env_get_log(env);

	if (!log)
		return;

	ham_assert( 0 != (log_get_state(log) & LOG_STATE_DB_EXPANSION), (0));
	log_set_state(log, log_get_state(log) & ~LOG_STATE_DB_EXPANSION);

	return;
}

ham_bool_t
ham_log_is_db_expansion(ham_env_t *env)
{
	ham_log_t *log = env_get_log(env);

	if (!log)
		return HAM_FALSE;

	return 0 != (log_get_state(log) & LOG_STATE_DB_EXPANSION);
}
