#!/bin/env python
# -*- coding: utf-8 -*-
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""
/***************************************************************************
  *
  * @file test_sys_schema_change.py
  * @date 2015/02/04 15:26:21
  * @brief This file is a test file for Palo schema changing.
  * 
  **************************************************************************/
"""

import time
from data import schema_change as DATA
from lib import palo_config
from lib import palo_client
from lib import util

config = palo_config.config
LOG = palo_client.LOG
L = palo_client.L
broker_info = palo_config.broker_info


def setup_module():
    """
    setUp
    """
    global client
    client = palo_client.get_client(config.fe_host, config.fe_query_port, user=config.fe_user, 
                                    password=config.fe_password, http_port=config.fe_http_port)


def assert_return(expected_flag, expected_msg, func, *args, **kwargs):
    try:
        func(*args, **kwargs)
        assert expected_flag
    except Exception as e:
        assert expected_msg in str(e)


def test_drop_col_v1():
    """
    {
    "title": "test_sys_schema_change_drop_duplicate.test_drop_col_v1",
    "describe": "删除一个value列",
    "tag": "system,p1"
    }
    """
    """
    删除一个value列
    """
    database_name, table_name, index_name = util.gen_num_format_name_list()
    LOG.info(L('', database_name=database_name, \
        table_name=table_name, index_name=index_name)) 
    client.clean(database_name)
    client.create_database(database_name)
    distribution_info = palo_client.DistributionInfo('HASH(k1, k2)', 10) 
    client.create_table(table_name, DATA.schema_1_dup, distribution_info=distribution_info, \
            keys_desc='DUPLICATE KEY (k1, k2)')

    time.sleep(1)

    assert client.show_tables(table_name)
    assert client.get_index(table_name)

    data_desc_list = palo_client.LoadDataInfo(DATA.file_path_1, table_name)
    ret = client.batch_load(util.get_label(), data_desc_list, is_wait=True, broker=broker_info)
    assert ret

    ret = client.verify(DATA.expected_data_file_list_1, table_name)
    assert ret

    column_name_list = ['v1']

    ret = client.schema_change_drop_column(table_name, column_name_list, \
            is_wait_job=True, is_wait_delete_old_schema=True)
    assert ret

    ret = client.verify(DATA.expected_data_file_list_9, table_name)
    assert ret
    client.clean(database_name)


def test_drop_col_v1_v2():
    """
    {
    "title": "test_sys_schema_change_drop_duplicate.test_drop_col_v1_v2",
    "describe": "删除 v1 v2 列",
    "tag": "system,p1"
    }
    """
    """
    删除 v1 v2 列
    """
    database_name, table_name, index_name = util.gen_num_format_name_list()
    LOG.info(L('', database_name=database_name, \
        table_name=table_name, index_name=index_name)) 
    client.clean(database_name)
    client.create_database(database_name)
    distribution_info = palo_client.DistributionInfo('HASH(k1, k2)', 10) 
    client.create_table(table_name, DATA.schema_1_dup, distribution_info=distribution_info, \
            keys_desc='DUPLICATE KEY (k1, k2)')

    time.sleep(1)

    assert client.show_tables(table_name)
    assert client.get_index(table_name)

    data_desc_list = palo_client.LoadDataInfo(DATA.file_path_1, table_name)
    ret = client.batch_load(util.get_label(), data_desc_list, is_wait=True, broker=broker_info)
    assert ret

    ret = client.verify(DATA.expected_data_file_list_1, table_name)
    assert ret

    column_name_list = ['v1', 'v2']

    ret = client.schema_change_drop_column(table_name, column_name_list, \
            is_wait_job=True, is_wait_delete_old_schema=True)
    assert ret

    ret = client.verify(DATA.expected_data_file_list_10, table_name)
    assert ret
    client.clean(database_name)


def test_drop_col_k2():
    """
    {
    "title": "test_sys_schema_change_drop_duplicate.test_drop_col_k2",
    "describe": "删除一个key列",
    "tag": "system,p1"
    }
    """
    """
    删除一个key列
    """
    database_name, table_name, index_name = util.gen_num_format_name_list()
    LOG.info(L('', database_name=database_name, \
        table_name=table_name, index_name=index_name)) 
    client.clean(database_name)
    client.create_database(database_name)
    distribution_info = palo_client.DistributionInfo('HASH(k1)', 10) 
#    client.create_table(table_name, DATA.schema_1, distribution_info=distribution_info, \
#            keys_desc='AGGREGATE KEY (k1, k2)')
    client.create_table(table_name, DATA.schema_1_new_dup, distribution_info=distribution_info, \
                        keys_desc='DUPLICATE KEY (k1, k2, v1)')

    time.sleep(1)

    assert client.show_tables(table_name)
    assert client.get_index(table_name)

    data_desc_list = palo_client.LoadDataInfo(DATA.file_path_1, table_name)
    ret = client.batch_load(util.get_label(), data_desc_list, is_wait=True, broker=broker_info)
    assert ret

    ret = client.verify(DATA.expected_data_file_list_1, table_name)
    assert ret

    column_name_list = ['k2']

    ret = client.schema_change_drop_column(table_name, column_name_list, \
            is_wait_job=True, is_wait_delete_old_schema=True)
    assert ret

    ret = client.verify(DATA.expected_data_file_list_11, table_name)
    assert ret
    client.clean(database_name)


def test_drop_k1():
    """
    {
    "title": "test_sys_schema_change_drop_duplicate.test_drop_k1",
    "describe": "删除一个key列，剩余列key相同",
    "tag": "system,p1"
    }
    """
    """
    删除一个key列，剩余列key相同
    """
    database_name, table_name, index_name = util.gen_num_format_name_list()
    LOG.info(L('', database_name=database_name, \
        table_name=table_name, index_name=index_name)) 
    client.clean(database_name)
    client.create_database(database_name)
    client.use(database_name)

    distribution_info = palo_client.DistributionInfo('HASH(k3)', 10) 
    client.create_table(table_name, DATA.schema_2_dup, distribution_info=distribution_info, \
            keys_desc='DUPLICATE KEY (k1, k2, k3)')

    assert client.show_tables(table_name)
    assert client.get_index(table_name)

    data_desc_list = palo_client.LoadDataInfo(DATA.file_path_3, table_name)
    assert client.batch_load(util.get_label(), data_desc_list, is_wait=True, broker=broker_info)

    column_name_list = ['k1']
    assert client.schema_change_drop_column(table_name, column_name_list, \
            is_wait_job=True, is_wait_delete_old_schema=True)

    data_desc_list = palo_client.LoadDataInfo(DATA.file_path_4, table_name)
    assert client.batch_load(util.get_label(), data_desc_list, is_wait=True, broker=broker_info)

    rollup_field_list = ['k3', 'v1']
    # 删除限制：Rollup should contains all base table's duplicate keys
    assert_return(True, "",
                  client.create_rollup_table, table_name, index_name,
                  rollup_field_list, is_wait=True)
    rollup_field_list = ['k2', 'k3', 'v1']
    assert_return(True, "",
                  client.create_rollup_table, table_name, index_name,
                  rollup_field_list, is_wait=True)

    column_list = [('key_add', 'INT KEY', None, '5')]
    assert client.schema_change_add_column(table_name, column_list, \
            after_column_name='k2', is_wait_job=True)
    client.clean(database_name)


def teardown_module():
    """
    tearDown
    """
    pass


if __name__ == '__main__':
    import pdb
    pdb.set_trace()
    setup_module()

