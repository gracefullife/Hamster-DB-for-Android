/*
 * Copyright (C) 2005-2010 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 */

/**
 * @brief the btree-backend
 *
 */

#ifndef HAM_BTREE_H__
#define HAM_BTREE_H__

#include "internal_fwd_decl.h"

#include "endian.h"

#include "backend.h"
#include "btree_cursor.h"
#include "keys.h"


#ifdef __cplusplus
extern "C" {
#endif 

/**
 * the backend structure for a b+tree 
 *
 * @remark doesn't need packing, because it's not persistent; 
 * see comment before ham_backend_t for an explanation.
 */
struct ham_btree_t;
typedef struct ham_btree_t ham_btree_t;

#include "packstart.h"

HAM_PACK_0 struct HAM_PACK_1 ham_btree_t 
{
    /**
     * the common declarations of all backends
     */
    BACKEND_DECLARATIONS(ham_btree_t);

    /**
     * address of the root-page 
     */
    ham_offset_t _rootpage;

    /**
     * maximum keys in an internal page 
     */
    ham_u16_t _maxkeys;

} HAM_PACK_2;

#include "packstop.h"

/**
 * get the address of the root node
 */
#define btree_get_rootpage(be)          (ham_db2h_offset((be)->_rootpage))

/**
 * set the address of the root node
 */
#define btree_set_rootpage(be, rp)      (be)->_rootpage=ham_h2db_offset(rp)

/** 
 * get maximum number of keys per (internal) node 
 */
#define btree_get_maxkeys(be)           (ham_db2h16((be)->_maxkeys))

/** 
 * set maximum number of keys per (internal) node 
 */
#define btree_set_maxkeys(be, s)        (be)->_maxkeys=ham_h2db16(s)

/**
 * a macro for getting the minimum number of keys
 */
#define btree_get_minkeys(maxkeys)      (maxkeys/2)

/**
 * defines the maximum number of keys per node
 */
#define MAX_KEYS_PER_NODE				0xFFFFU /* max(ham_u16_t) */


#include "packstart.h"

/**
 * A btree-node; it spans the persistent part of a ham_page_t:
 *
 * <pre>
 * btree_node_t *btp=(btree_node_t *)page->_u._pers.payload;
 * </pre>
 */
typedef HAM_PACK_0 struct HAM_PACK_1 btree_node_t
{
    /**
     * flags of this node - flags are always the first member
     * of every page - regardless of the backend.
	 *
     * Currently only used for the page type.
	 *
	 * @sa page_type_codes
     */
    ham_u16_t _flags;

    /**
     * number of used entries in the node
     */
    ham_u16_t _count;

    /**
     * address of left sibling
     */
    ham_offset_t _left;

    /**
     * address of right sibling
     */
    ham_offset_t _right;

    /**
     * address of child node whose items are smaller than all items 
     * in this node 
     */
    ham_offset_t _ptr_left;

    /**
     * the entries of this node
     */
    int_key_t _entries[1];

} HAM_PACK_2 btree_node_t;

#include "packstop.h"

/**
 * get the number of entries of a btree-node
 */
#define btree_node_get_count(btp)            (ham_db2h16(btp->_count))

/**
 * set the number of entries of a btree-node
 */
#define btree_node_set_count(btp, c)         btp->_count=ham_h2db16(c)

/**
 * get the left sibling of a btree-node
 */
#define btree_node_get_left(btp)             (ham_db2h_offset(btp->_left))

/*
 * check if a btree node is a leaf node
 */
#define btree_node_is_leaf(btp)              (!(btree_node_get_ptr_left(btp)))

/**
 * set the left sibling of a btree-node
 */
#define btree_node_set_left(btp, l)          btp->_left=ham_h2db_offset(l)

/**
 * get the right sibling of a btree-node
 */
#define btree_node_get_right(btp)            (ham_db2h_offset(btp->_right))

/**
 * set the right sibling of a btree-node
 */
#define btree_node_set_right(btp, r)         btp->_right=ham_h2db_offset(r)

/**
 * get the ptr_left of a btree-node
 */
#define btree_node_get_ptr_left(btp)         (ham_db2h_offset(btp->_ptr_left))

/**
 * set the ptr_left of a btree-node
 */
#define btree_node_set_ptr_left(btp, r)      btp->_ptr_left=ham_h2db_offset(r)

/**
 * get a btree_node_t from a ham_page_t
 */
#define ham_page_get_btree_node(p)      ((btree_node_t *)p->_pers->_s._payload)

/**
 * "constructor" - initializes a new ham_btree_t object
 *
 * @return a pointer to a new created B+-tree backend 
 *         instance in @a backend_ref
 *
 * @remark flags are from @ref ham_env_open_db() or @ref ham_env_create_db()
 */
extern ham_status_t
btree_create(ham_backend_t **backend_ref, ham_db_t *db, ham_u32_t flags);

/**
 * search the btree structures for a record
 *
 * @remark this function returns HAM_SUCCESS and returns 
 * the record ID @a rid, if the @a key was found; otherwise 
 * an error code is returned 
 *
 * @remark this function is exported through the backend structure.
 */
extern ham_status_t 
btree_find(ham_btree_t *be, ham_key_t *key,
           ham_record_t *record, ham_u32_t flags);

/**
 * same as above, but sets the cursor to the position
 */
extern ham_status_t 
btree_find_cursor(ham_btree_t *be, ham_bt_cursor_t *cursor, 
           ham_key_t *key, ham_record_t *record, ham_u32_t flags);

/**
 * insert a new tuple (key/record) in the tree
 */
extern ham_status_t
btree_insert(ham_btree_t *be, ham_key_t *key, 
        ham_record_t *record, ham_u32_t flags);

/**
 * same as above, but sets the cursor position to the new item
 */
extern ham_status_t
btree_insert_cursor(ham_btree_t *be, ham_key_t *key, 
        ham_record_t *record, ham_bt_cursor_t *cursor, ham_u32_t flags);

/**
 * erase a key from the tree
 */
extern ham_status_t
btree_erase(ham_btree_t *be, ham_key_t *key, ham_u32_t flags);

/**
 * same as above, but with a coupled cursor
 */
extern ham_status_t
btree_erase_cursor(ham_btree_t *be, ham_key_t *key, ham_bt_cursor_t *cursor, 
        ham_u32_t flags);

/**
 * enumerate all items
 */
extern ham_status_t
btree_enumerate(ham_btree_t *be, ham_enumerate_cb_t cb,
        void *context);

/**                                                                 
 * verify the whole tree                                            
 *                                                                  
 * @remark this function is only available when						
 * hamsterdb is compiled with HAM_ENABLE_INTERNAL turned on.        

 @note This is a B+-tree 'backend' method.
 */                                                                 
extern ham_status_t
btree_check_integrity(ham_btree_t *be);

/**
 * find the child page for a key
 *
 * @return returns the child page in @a page_ref
 *
 * @remark if @a idxptr is a valid pointer, it will store the anchor index of the 
 *      loaded page
 */
extern ham_status_t
btree_traverse_tree(ham_page_t **page_ref, ham_s32_t *idxptr, 
					ham_db_t *db, ham_page_t *page, ham_key_t *key);

/**
 * search a leaf node for a key
 *
 * !!!
 * only works with leaf nodes!!
 *
 * @return returns the index of the key, or -1 if the key was not found, or 
 *         another negative @ref ham_status_codes value when an 
 *         unexpected error occurred.
 */
extern ham_s32_t 
btree_node_search_by_key(ham_db_t *db, ham_page_t *page, ham_key_t *key, 
                ham_u32_t flags);

/**
 * get entry @a i of a btree node
 */
#define btree_node_get_key(db, node, i)                             \
    ((int_key_t *)&((const char *)(node)->_entries)                 \
            [(db_get_keysize(db)+db_get_int_key_header_size())*(i)])

/**
 * get offset of entry @a i - add this to page_get_self(page) for
 * the absolute offset of the key in the file
 */
#define btree_node_get_key_offset(page, i)                          \
     (page_get_self(page)+page_get_persistent_header_size()+        \
     OFFSETOF(btree_node_t, _entries)                               \
     /* ^^^ sizeof(int_key_t) WITHOUT THE -1 !!! */ +               \
     (db_get_int_key_header_size()+db_get_keysize(page_get_owner(page)))*(i))

/*
 * get the slot of an element in the page
 */
extern ham_status_t 
btree_get_slot(ham_db_t *db, ham_page_t *page, 
        ham_key_t *key, ham_s32_t *slot, int *cmp);

extern ham_size_t
btree_calc_maxkeys(ham_size_t pagesize, ham_u16_t keysize);

extern ham_status_t 
btree_close_cursors(ham_db_t *db, ham_u32_t flags);


#ifdef __cplusplus
} // extern "C"
#endif 

#endif /* HAM_BTREE_H__ */
