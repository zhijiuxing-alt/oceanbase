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

#include <cstdio>
#include <gtest/gtest.h>
#include <signal.h>
#include "share/scn.h"
#define private public
#include "env/ob_simple_log_cluster_env.h"
#undef private

const std::string TEST_NAME = "access_mode";

using namespace oceanbase::common;
using namespace oceanbase;
namespace oceanbase
{
using namespace logservice;
using namespace palf;
namespace unittest
{

class TestObSimpleLogClusterAccessMode : public ObSimpleLogClusterTestEnv
{
public:
  TestObSimpleLogClusterAccessMode() :  ObSimpleLogClusterTestEnv()
  {}
};

int64_t ObSimpleLogClusterTestBase::member_cnt_ = 3;
int64_t ObSimpleLogClusterTestBase::node_cnt_ = 4;
std::string ObSimpleLogClusterTestBase::test_name_ = TEST_NAME;

// 1. basic change_access_mode
// 2. change_access_mode(block a follower), then switch this follower to leader
// 3. change_access_mode when leader's net is blocked
// 4. change_access_mode, then add_member
// 5. prev logs have slid, and pid of prev logs is different from current pid

TEST_F(TestObSimpleLogClusterAccessMode, basic_change_access_mode)
{
  SET_CASE_LOG_FILE(TEST_NAME, "basic_change_access_mode");
  int ret = OB_SUCCESS;
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  PALF_LOG(INFO, "begin test basic_change_access_mode", K(id));
	int64_t leader_idx = 0;
  int64_t ref_ts_ns = common::ObTimeUtility::current_time_ns() + 1800 * 1000L * 1000L * 1000L;
  share::SCN ref_scn;
  ref_scn.convert_for_logservice(ref_ts_ns);
  PalfHandleGuard leader;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000 * 1000L; // 10s
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 200, id));
  AccessMode curr_access_mode;
  int64_t mode_version, curr_proposal_id;
  ObRole unused_role;
  EXPECT_EQ(OB_SUCCESS, leader.get_access_mode(mode_version, curr_access_mode));
  EXPECT_EQ(AccessMode::APPEND, curr_access_mode);
  // test can not submit log in MODE_PREPARE
  EXPECT_EQ(OB_EAGAIN, leader.palf_handle_.palf_handle_impl_->mode_mgr_.change_access_mode(mode_version, palf::AccessMode::RAW_WRITE, share::SCN::min_scn()));
  // cannot submit_log in prepare state
  EXPECT_EQ(OB_NOT_MASTER, submit_log(leader, 1, id));
  int tmp_ret = OB_EAGAIN;
  while(OB_EAGAIN == tmp_ret) {
    tmp_ret = leader.palf_handle_.palf_handle_impl_->mode_mgr_.change_access_mode(mode_version, palf::AccessMode::RAW_WRITE, share::SCN::min_scn());
    usleep(10 * 1000);
  }
  EXPECT_EQ(OB_SUCCESS, leader.get_access_mode(mode_version, curr_access_mode));
  EXPECT_EQ(AccessMode::RAW_WRITE, curr_access_mode);

  EXPECT_EQ(OB_SUCCESS, leader.get_role(unused_role, curr_proposal_id));
  EXPECT_EQ(OB_SUCCESS, leader.change_access_mode(curr_proposal_id, mode_version, palf::AccessMode::FLASHBACK, share::SCN::min_scn()));
  EXPECT_EQ(OB_NOT_MASTER, submit_log(leader, 1, id));
  // base_ts: 0.5 hour later
  EXPECT_EQ(OB_SUCCESS, leader.get_access_mode(mode_version, curr_access_mode));
  EXPECT_EQ(OB_SUCCESS, leader.get_role(unused_role, curr_proposal_id));
  EXPECT_EQ(OB_SUCCESS, leader.change_access_mode(curr_proposal_id, mode_version, AccessMode::APPEND, ref_scn));
  std::vector<LSN> lsn_array;
  std::vector<SCN> scn_arrary;
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 50, id, lsn_array, scn_arrary));
  for (auto scn : scn_arrary)
  {
    EXPECT_GT(scn, ref_scn);
  }
  // 40 minutes later
  ref_ts_ns = common::ObTimeUtility::current_time_ns() + 2400L * 1000L * 1000L * 1000L;
  ref_scn.convert_for_logservice(ref_ts_ns);
  EXPECT_EQ(OB_SUCCESS, leader.get_access_mode(mode_version, curr_access_mode));
  EXPECT_EQ(OB_SUCCESS, leader.get_role(unused_role, curr_proposal_id));
  // can not APPEND -> APPEND
  EXPECT_EQ(OB_STATE_NOT_MATCH, leader.change_access_mode(curr_proposal_id, mode_version, AccessMode::APPEND, ref_scn));
  // can not APPEND -> FLASHBACK
  EXPECT_EQ(OB_STATE_NOT_MATCH, leader.change_access_mode(curr_proposal_id, mode_version, AccessMode::FLASHBACK, ref_scn));
  EXPECT_EQ(OB_SUCCESS, leader.change_access_mode(curr_proposal_id, mode_version, AccessMode::RAW_WRITE, ref_scn));
  EXPECT_EQ(OB_SUCCESS, leader.get_access_mode(mode_version, curr_access_mode));
  EXPECT_EQ(OB_SUCCESS, leader.get_role(unused_role, curr_proposal_id));
  EXPECT_EQ(OB_SUCCESS, leader.change_access_mode(curr_proposal_id, mode_version, AccessMode::APPEND, ref_scn));
  lsn_array.clear();
  scn_arrary.clear();
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 50, id, lsn_array, scn_arrary));
  for (auto scn : scn_arrary)
  {
    EXPECT_GT(scn, ref_scn);
  }
  PALF_LOG(INFO, "end test basic_change_access_mode", K(id));
}

TEST_F(TestObSimpleLogClusterAccessMode, switch_leader1)
{
  SET_CASE_LOG_FILE(TEST_NAME, "switch_leader1");
  int ret = OB_SUCCESS;
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  PALF_LOG(INFO, "begin test switch_leader1", K(id));
	int64_t leader_idx = 0;
  PalfHandleGuard leader;
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 200, id));
  AccessMode curr_access_mode;
  int64_t mode_version, curr_proposal_id;
  ObRole unused_role;
  EXPECT_EQ(OB_SUCCESS, leader.get_access_mode(mode_version, curr_access_mode));
  EXPECT_EQ(OB_SUCCESS, leader.get_role(unused_role, curr_proposal_id));
  EXPECT_EQ(AccessMode::APPEND, curr_access_mode);
  const int64_t follower1_idx = (leader_idx+1)%3;
  const int64_t follower2_idx = (leader_idx+2)%3;
  block_net(leader_idx, follower1_idx);

  EXPECT_EQ(OB_SUCCESS, leader.change_access_mode(curr_proposal_id, mode_version, palf::AccessMode::RAW_WRITE, share::SCN::min_scn()));
  EXPECT_EQ(OB_SUCCESS, leader.get_role(unused_role, curr_proposal_id));
  EXPECT_EQ(OB_SUCCESS, leader.get_access_mode(mode_version, curr_access_mode));
  EXPECT_EQ(curr_proposal_id, mode_version);
  block_net(leader_idx, follower2_idx);
  sleep(10);
  // a follower will be leader
  while (true) {
    PalfHandleGuard new_leader;
    int64_t new_leader_idx;
    int64_t mode_version_after_reconfirm = -1;
    EXPECT_EQ(OB_SUCCESS, get_leader(id, new_leader, new_leader_idx));
    if (new_leader_idx != leader_idx) {
      EXPECT_EQ(OB_SUCCESS, new_leader.get_access_mode(mode_version_after_reconfirm, curr_access_mode));
      EXPECT_EQ(AccessMode::RAW_WRITE, curr_access_mode);
      // mode_version won't be changed after reconfirming
      EXPECT_EQ(mode_version_after_reconfirm, mode_version);
      EXPECT_EQ(OB_SUCCESS, new_leader.get_role(unused_role, curr_proposal_id));
      EXPECT_GT(curr_proposal_id, mode_version_after_reconfirm);
      break;
    } else {
      sleep(1);
    }
  }
  unblock_net(leader_idx, follower1_idx);
  unblock_net(leader_idx, follower2_idx);
  PALF_LOG(INFO, "end test switch_leader1", K(id));
}

TEST_F(TestObSimpleLogClusterAccessMode, add_member)
{
  SET_CASE_LOG_FILE(TEST_NAME, "add_member");
  MockLocCB loc_cb;
  int ret = OB_SUCCESS;
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  PALF_LOG(INFO, "begin test add_member", K(id));
  {
	  int64_t leader_idx = 0;
    PalfHandleGuard leader;
    std::vector<PalfHandleGuard*> palf_list;
    const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000 * 1000L; // 10s
    EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
    EXPECT_EQ(OB_SUCCESS, submit_log(leader, 200, id));
    // set leader's region is different from follower, learner will
    // register itself to leader
    std::vector<ObRegion> region_list;
    LogMemberRegionMap region_map;
    common::ObRegion default_region(DEFAULT_REGION_NAME);
    EXPECT_EQ(OB_SUCCESS, region_map.init("localmap", OB_MAX_MEMBER_NUMBER));
    region_map.insert(get_cluster()[0]->get_addr(), default_region);
    region_map.insert(get_cluster()[1]->get_addr(), ObRegion("SHANGHAI"));
    region_map.insert(get_cluster()[2]->get_addr(), ObRegion("TIANJIN"));
    EXPECT_EQ(OB_SUCCESS, leader.set_paxos_member_region_map(region_map));
    EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));
    palf_list[1]->set_region(ObRegion("SHANGHAI"));
    palf_list[2]->set_region(ObRegion("TIANJIN"));

    AccessMode curr_access_mode;
    int64_t mode_version, curr_proposal_id;
    ObRole unused_role;
    EXPECT_EQ(OB_SUCCESS, leader.get_access_mode(mode_version, curr_access_mode));
    EXPECT_EQ(OB_SUCCESS, leader.get_role(unused_role, curr_proposal_id));
    EXPECT_EQ(AccessMode::APPEND, curr_access_mode);
    const int64_t follower1_idx = (leader_idx+1)%3;
    const int64_t follower2_idx = (leader_idx+2)%3;
    block_net(leader_idx, follower1_idx);
    // election membership version reaches majority
    sleep(1);

    EXPECT_EQ(OB_SUCCESS, leader.change_access_mode(curr_proposal_id, mode_version, palf::AccessMode::RAW_WRITE, share::SCN::min_scn()));
    unblock_net(leader_idx, follower1_idx);
    block_net(leader_idx, follower2_idx);

    // new_leader需要在delete_paxos_group()之前析构，否则palf_handle引用计数无法清零
    PalfHandleGuard new_leader;
    int64_t new_leader_idx;
    EXPECT_EQ(OB_SUCCESS, get_leader(id, new_leader, new_leader_idx));
    loc_cb.leader_ = get_cluster()[new_leader_idx]->get_addr();
    PALF_LOG(INFO, "set leader for loc_cb", "leader", get_cluster()[new_leader_idx]->get_addr());
    EXPECT_EQ(OB_SUCCESS, palf_list[3]->set_location_cache_cb(&loc_cb));

    EXPECT_EQ(OB_SUCCESS, leader.add_learner(ObMember(get_cluster()[3]->addr_, 1), CONFIG_CHANGE_TIMEOUT));
    sleep(2);
    EXPECT_EQ(OB_SUCCESS, leader.switch_learner_to_acceptor(ObMember(get_cluster()[3]->addr_, 1), CONFIG_CHANGE_TIMEOUT));
    unblock_net(leader_idx, follower2_idx);
    revert_cluster_palf_handle_guard(palf_list);
  }
  delete_paxos_group(id);
  PALF_LOG(INFO, "end test add_member", K(id));
}

TEST_F(TestObSimpleLogClusterAccessMode, prev_log_slide)
{
  SET_CASE_LOG_FILE(TEST_NAME, "prev_log_slide");
  int ret = OB_SUCCESS;
	const int64_t id = ATOMIC_AAF(&palf_id_, 1);
  PALF_LOG(INFO, "begin test prev_log_slide", K(id));
	int64_t leader_idx = 0;
  PalfHandleGuard leader;
  const int64_t CONFIG_CHANGE_TIMEOUT = 10 * 1000 * 1000 * 1000L; // 10s
  EXPECT_EQ(OB_SUCCESS, create_paxos_group(id, leader_idx, leader));
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 200, id));

  // inc current proposal_id
  int64_t curr_pid = 0;
  ObRole role;
  std::vector<PalfHandleGuard*> palf_list;
  EXPECT_EQ(OB_SUCCESS, get_cluster_palf_handle_guard(id, palf_list));
  EXPECT_EQ(OB_SUCCESS, leader.get_role(role, curr_pid));
  EXPECT_EQ(OB_EAGAIN, leader.palf_handle_.palf_handle_impl_->mode_mgr_.switch_state_(AccessMode::RAW_WRITE, share::SCN::min_scn(), false));
  // can not submit_log, config_change in prepare state
  EXPECT_FALSE(leader.palf_handle_.palf_handle_impl_->mode_mgr_.can_append());
  LogConfigChangeArgs args(ObMember(get_cluster()[3]->addr_, 1), 0, ADD_LEARNER);
  EXPECT_EQ(OB_NOT_MASTER, submit_log(leader, 1, id));
  const LogConfigMeta config_meta = leader.palf_handle_.palf_handle_impl_->config_mgr_.log_ms_meta_;
  EXPECT_EQ(OB_EAGAIN, leader.palf_handle_.palf_handle_impl_->config_mgr_.change_config(args));
  const LogConfigMeta new_config_meta = leader.palf_handle_.palf_handle_impl_->config_mgr_.log_ms_meta_;
  EXPECT_EQ(config_meta.curr_.config_version_, new_config_meta.curr_.config_version_);
  // wait prepare req reaches majority
  sleep(1);
  // switch to accept state
  EXPECT_EQ(OB_EAGAIN, leader.palf_handle_.palf_handle_impl_->mode_mgr_.switch_state_(AccessMode::RAW_WRITE, share::SCN::min_scn(), false));
  EXPECT_TRUE(leader.palf_handle_.palf_handle_impl_->mode_mgr_.can_append());
  int64_t new_pid = 0;
  EXPECT_EQ(OB_SUCCESS, leader.get_role(role, new_pid));
  EXPECT_EQ(curr_pid + 1, new_pid);
  EXPECT_EQ(curr_pid + 1, palf_list[1]->palf_handle_.palf_handle_impl_->state_mgr_.get_proposal_id());
  EXPECT_EQ(curr_pid + 1, palf_list[2]->palf_handle_.palf_handle_impl_->state_mgr_.get_proposal_id());
  //  apply config_change barrier with older log proposal_id
  EXPECT_EQ(OB_EAGAIN, leader.palf_handle_.palf_handle_impl_->config_mgr_.change_config(args));

  // submit log with new proposal_id and wait for sliding out in follower
  EXPECT_EQ(OB_SUCCESS, submit_log(leader, 100, id));
  while (true) {
    if (palf_list[1]->palf_handle_.palf_handle_impl_->sw_.last_slide_log_pid_ == new_pid &&
        palf_list[2]->palf_handle_.palf_handle_impl_->sw_.last_slide_log_pid_ == new_pid) {
      break;
    } else {
      EXPECT_EQ(OB_SUCCESS, submit_log(leader, 1, id));
      usleep(500 * 1000);
    }
  }
  sleep(1);
  revert_cluster_palf_handle_guard(palf_list);
  // continue add_learner, prev log must have slided
  while(true) {
    if (OB_SUCC(leader.palf_handle_.palf_handle_impl_->config_mgr_.change_config(args))) {
      break;
    } else {
      usleep(100 * 1000);
    }
  }
  PALF_LOG(INFO, "end test prev_log_slide", K(id));
}

} // end unittest
} // end oceanbase

int main(int argc, char **argv)
{
  RUN_SIMPLE_LOG_CLUSTER_TEST(TEST_NAME);
}