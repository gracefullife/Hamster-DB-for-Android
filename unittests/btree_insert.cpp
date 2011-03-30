/**
 * Copyright (C) 2005-2008 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 */

#include "../src/config.h"

#include <stdexcept>
#include <cstring>
#include <ham/hamsterdb.h>
#include "memtracker.h"
#include "../src/db.h"
#include "../src/version.h"
#include "../src/page.h"
#include "../src/env.h"
#include "../src/btree.h"
#include "os.hpp"

#include "bfc-testsuite.hpp"
#include "hamster_fixture.hpp"

using namespace bfc;

class BtreeInsertTest : public hamsterDB_fixture
{
	define_super(hamsterDB_fixture);

public:
    BtreeInsertTest(ham_u32_t flags=0, const char *name="BtreeInsertTest")
        : hamsterDB_fixture(name), 
        m_db(0), m_flags(flags), m_alloc(0)
    {
        testrunner::get_instance()->register_fixture(this);
        BFC_REGISTER_TEST(BtreeInsertTest, defaultPivotTest);
        BFC_REGISTER_TEST(BtreeInsertTest, defaultLatePivotTest);
        BFC_REGISTER_TEST(BtreeInsertTest, sequentialInsertPivotTest);
    }

protected:
    ham_db_t *m_db;
    ham_env_t *m_env;
    ham_u32_t m_flags;
    memtracker_t *m_alloc;

public:
    virtual void setup() 
	{ 
		__super::setup();

        ham_parameter_t params[]={
            { HAM_PARAM_PAGESIZE, 1024 },
            { HAM_PARAM_KEYSIZE, 128 },
            { 0, 0 }
        };

        os::unlink(BFC_OPATH(".test"));
        BFC_ASSERT((m_alloc=memtracker_new())!=0);
        BFC_ASSERT_EQUAL(0, ham_new(&m_db));
        BFC_ASSERT_EQUAL(0, 
                ham_create_ex(m_db, BFC_OPATH(".test"), m_flags, 
                                0644, &params[0]));
        m_env=ham_get_env(m_db);
    }
    
    virtual void teardown() 
	{ 
		__super::teardown();

        BFC_ASSERT_EQUAL(0, ham_close(m_db, 0));
        ham_delete(m_db);
        BFC_ASSERT(!memtracker_get_leaks(m_alloc));
    }

    void defaultPivotTest() 
    {
        ham_key_t key;
        ham_record_t rec;
        memset(&key, 0, sizeof(key));
        memset(&rec, 0, sizeof(rec));

        for (int i=6; i>=0; i--) {
            key.data=&i;
            key.size=sizeof(i);

            BFC_ASSERT_EQUAL(0,
                    ham_insert(m_db, 0, &key, &rec, 0));
        }

        /* now verify that the index has 3 pages - root and two pages in 
         * level 1 - both are 50% full 
         *
         * the first page is the old root page, which became an index
         * page after the split
         */
        ham_page_t *page;
        btree_node_t *node;
        BFC_ASSERT_EQUAL(0,
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*1, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_INDEX, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(4, btree_node_get_count(node));

        BFC_ASSERT_EQUAL(0, 
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*2, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_INDEX, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(3, btree_node_get_count(node));

        BFC_ASSERT_EQUAL(0, 
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*3, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_ROOT, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(1, btree_node_get_count(node));
    }

    void defaultLatePivotTest() 
    {
        ham_key_t key;
        ham_record_t rec;
        memset(&key, 0, sizeof(key));
        memset(&rec, 0, sizeof(rec));

        for (int i=0; i<7; i++) {
            key.data=&i;
            key.size=sizeof(i);

            BFC_ASSERT_EQUAL(0,
                    ham_insert(m_db, 0, &key, &rec, 0));
        }

        /* now verify that the index has 3 pages - root and two pages in 
         * level 1 - both are 50% full 
         *
         * the first page is the old root page, which became an index
         * page after the split
         */
        ham_page_t *page;
        btree_node_t *node;
        BFC_ASSERT_EQUAL(0,
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*1, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_INDEX, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(4, btree_node_get_count(node));

        BFC_ASSERT_EQUAL(0, 
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*2, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_INDEX, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(3, btree_node_get_count(node));

        BFC_ASSERT_EQUAL(0, 
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*3, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_ROOT, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(1, btree_node_get_count(node));
    }

    void sequentialInsertPivotTest() 
    {
        ham_key_t key;
        ham_record_t rec;
        memset(&key, 0, sizeof(key));
        memset(&rec, 0, sizeof(rec));

        teardown();

        ham_parameter_t params[]={
            { HAM_PARAM_PAGESIZE, 1024 },
            { HAM_PARAM_KEYSIZE, 128 },
            { HAM_PARAM_DATA_ACCESS_MODE, HAM_DAM_SEQUENTIAL_INSERT },
            { 0, 0 }
        };
        BFC_ASSERT((m_alloc=memtracker_new())!=0);
        BFC_ASSERT_EQUAL(0, ham_new(&m_db));
        BFC_ASSERT_EQUAL(0, 
                ham_create_ex(m_db, BFC_OPATH(".test"), m_flags, 
                                0644, &params[0]));
        m_env=ham_get_env(m_db);

        for (int i=0; i<7; i++) {
            key.data=&i;
            key.size=sizeof(i);

            BFC_ASSERT_EQUAL(0,
                    ham_insert(m_db, 0, &key, &rec, 0));
        }

        /* now verify that the index has 3 pages - root and two pages in 
         * level 1 - both are 50% full 
         *
         * the first page is the old root page, which became an index
         * page after the split
         */
        ham_page_t *page;
        btree_node_t *node;
        BFC_ASSERT_EQUAL(0,
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*1, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_INDEX, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(4, btree_node_get_count(node));

        BFC_ASSERT_EQUAL(0, 
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*2, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_INDEX, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(3, btree_node_get_count(node));

        BFC_ASSERT_EQUAL(0, 
                db_fetch_page(&page, m_db, env_get_pagesize(m_env)*3, 0));
        BFC_ASSERT_EQUAL((unsigned)PAGE_TYPE_B_ROOT, page_get_type(page));
        node=ham_page_get_btree_node(page);
        BFC_ASSERT_EQUAL(1, btree_node_get_count(node));
    }

};

BFC_REGISTER_FIXTURE(BtreeInsertTest);

