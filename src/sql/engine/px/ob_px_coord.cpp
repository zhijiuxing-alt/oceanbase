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

#define USING_LOG_PREFIX SQL_ENG
#include "lib/random/ob_random.h"
#include "sql/engine/px/ob_px_coord.h"
#include "share/ob_rpc_share.h"
#include "share/schema/ob_part_mgr_util.h"
#include "sql/ob_sql.h"
#include "sql/engine/px/ob_px_util.h"
#include "sql/dtl/ob_dtl_channel_group.h"
#include "sql/dtl/ob_dtl_channel_loop.h"
#include "sql/dtl/ob_dtl_msg_type.h"
#include "sql/dtl/ob_dtl.h"
#include "sql/dtl/ob_op_metric.h"
#include "sql/executor/ob_transmit.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/px/ob_px_dtl_msg.h"
#include "sql/engine/px/ob_px_dtl_proc.h"
#include "sql/engine/px/exchange/ob_px_receive.h"
#include "sql/engine/px/ob_px_interruption.h"
#include "sql/engine/px/ob_dfo_scheduler.h"
#include "share/config/ob_server_config.h"
#include "sql/engine/px/ob_px_sqc_async_proxy.h"
#include "sql/engine/px/datahub/components/ob_dh_barrier.h"
#include "sql/engine/px/datahub/components/ob_dh_winbuf.h"
#include "sql/dtl/ob_dtl_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;
using namespace oceanbase::sql;
using namespace oceanbase::sql::dtl;

ObPxCoord::ObPxCoordCtx::ObPxCoordCtx(ObExecContext& ctx)
    : ObPxReceiveCtx(ctx),
      allocator_(common::ObModIds::OB_SQL_PX),
      row_allocator_(common::ObModIds::OB_SQL_PX),
      coord_info_(allocator_, msg_loop_, interrupt_id_),
      root_dfo_(NULL),
      root_receive_ch_provider_(),
      first_row_fetched_(false),
      first_row_sent_(false),
      all_rows_finish_(false),
      qc_id_(common::OB_INVALID_ID),
      first_buffer_cache_(allocator_),
      time_recorder_(0),
      register_interrupted_(false),
      px_sequence_id_(0),
      interrupt_id_(0),
      px_dop_(1)
{}

ObPxCoord::ObPxCoordCtx::~ObPxCoordCtx()
{
  destroy();
}

int ObPxCoord::ObPxCoordCtx::init_dfc(ObExecContext& ctx, ObDfo& dfo)
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx* phy_plan_ctx = NULL;
  if (OB_ISNULL(phy_plan_ctx = GET_PHY_PLAN_CTX(ctx))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("The phy plan ctx is null", K(ret));
  } else if (OB_FAIL(dfc_.init(ctx.get_my_session()->get_effective_tenant_id(), task_ch_set_.count()))) {
    LOG_WARN("Fail to init dfc", K(ret));
  } else if (OB_INVALID_ID == dfo.get_qc_id() || OB_INVALID_ID == dfo.get_dfo_id()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected status: dfo or qc id is invalid", K(dfo), K(dfo.get_qc_id()), K(dfo.get_dfo_id()));
  } else {
    ObDtlDfoKey dfo_key;
    dfo_key.set(GCTX.server_id_, dfo.get_px_sequence_id(), dfo.get_qc_id(), dfo.get_dfo_id());
    dfc_.set_timeout_ts(phy_plan_ctx->get_timeout_timestamp());
    dfc_.set_receive();
    dfc_.set_qc_coord();
    dfc_.set_dfo_key(dfo_key);
    dfc_.set_op_metric(&metric_);
    dfc_.set_dtl_channel_watcher(&msg_loop_);
    DTL.get_dfc_server().register_dfc(dfc_);
    bool force_block = false;
#ifdef ERRSIM
    int ret = OB_SUCCESS;
    ret = E(EventTable::EN_FORCE_DFC_BLOCK) ret;
    force_block = (OB_HASH_NOT_EXIST == ret);
    LOG_TRACE("Worker init dfc", K(dfo_key), K(dfc_.is_receive()), K(force_block), K(ret));
    ret = OB_SUCCESS;
#endif
    ObDtlLocalFirstBufferCache* buf_cache = nullptr;
    if (OB_FAIL(DTL.get_dfc_server().get_buffer_cache(
            ctx.get_my_session()->get_effective_tenant_id(), dfo_key, buf_cache))) {
      LOG_WARN("failed to get buffer cache", K(dfo_key));
    } else {
      dfc_.set_first_buffer_cache(buf_cache);
    }
    LOG_TRACE("QC init dfc", K(dfo_key), K(dfc_.is_receive()), K(force_block));
  }
  return ret;
}

ObPxCoord::ObPxCoord(ObIAllocator& alloc) : ObPxReceive(alloc), px_expected_worker_count_(0)
{}

ObPxCoord::~ObPxCoord()
{}

void ObPxCoord::debug_print(ObDfo& root) const
{
  // print plan tree
  const ObPhysicalPlan* phy_plan = root.get_phy_plan();
  if (NULL != phy_plan) {
    LOG_TRACE("ObPxCoord PLAN", "plan", *phy_plan);
  }
  // print dfo tree
  debug_print_dfo_tree(0, root);
}

void ObPxCoord::debug_print_dfo_tree(int level, ObDfo& dfo) const
{
  int ret = OB_SUCCESS;
  const ObPhyOperator* phy_op = dfo.get_root_op();
  if (OB_ISNULL(phy_op)) {
    LOG_TRACE("DFO", K(level), K(dfo));
  } else {
    LOG_TRACE("DFO", K(level), "dfo_root_op_type", phy_op->get_type(), "dfo_root_op_id", phy_op->get_id(), K(dfo));
  }
  level++;
  int64_t cnt = dfo.get_child_count();
  for (int64_t idx = 0; OB_SUCC(ret) && idx < cnt; ++idx) {
    ObDfo* child_dfo = NULL;
    if (OB_FAIL(dfo.get_child_dfo(idx, child_dfo))) {
      LOG_WARN("fail get child_dfo", K(idx), K(cnt), K(ret));
    } else if (OB_ISNULL(child_dfo)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child_dfo is null", K(ret));
    } else {
      debug_print_dfo_tree(level, *child_dfo);
    }
  }
}

int ObPxCoord::rescan(ObExecContext& ctx) const
{
  int ret = OB_SUCCESS;
  ObPxCoordCtx* px_ctx = NULL;
  ObDfo* root_dfo = NULL;
  if (OB_ISNULL(px_ctx = GET_PHY_OPERATOR_CTX(ObPxCoordCtx, ctx, get_id()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get physical operator context failed", K(ret));
  } else if (OB_FAIL(terminate_running_dfos(ctx, px_ctx->coord_info_.dfo_mgr_))) {
    LOG_WARN("fail to release px resources in QC inner_close", K(ret));
  } else if (OB_FAIL(destroy_all_channel(*px_ctx))) {
    LOG_WARN("release dtl channel failed", K(ret));
  } else if (FALSE_IT(unregister_first_buffer_cache(ctx, *px_ctx))) {
    LOG_WARN("failed to register first buffer cache", K(ret));
  } else if (OB_FAIL(free_allocator(ctx))) {
    LOG_WARN("failed to free allocator", K(ret));
  } else if (FALSE_IT(px_ctx->reset_for_rescan())) {
    // nop
  } else if (OB_FAIL(init_dfo_mgr(ObDfoInterruptIdGen(px_ctx->interrupt_id_,
                                      (uint32_t)GCTX.server_id_,
                                      (uint32_t)px_ctx->qc_id_,
                                      px_ctx->px_sequence_id_),
                 ctx,
                 px_ctx->coord_info_.dfo_mgr_))) {
    LOG_WARN("fail parse dfo tree",
        "server_id",
        GCTX.server_id_,
        "qc_id",
        px_ctx->qc_id_,
        "execution_id",
        ctx.get_my_session()->get_current_execution_id(),
        K(ret));
  } else if (OB_ISNULL(root_dfo = px_ctx->coord_info_.dfo_mgr_.get_root_dfo())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL root dfo", K(ret));
  } else if (OB_FAIL(setup_op_input(ctx, *root_dfo))) {
    LOG_WARN("fail setup all receive/transmit op input", K(ret));
  } else if (OB_FAIL(setup_loop_proc(ctx, *px_ctx))) {
    LOG_WARN("fail setup loop proc", K(ret));
  } else if (OB_FAIL(register_first_buffer_cache(ctx, *px_ctx, root_dfo))) {
    LOG_WARN("failed to register first buffer cache", K(ret));
  }
  return ret;
}

int ObPxCoord::register_first_buffer_cache(ObExecContext& ctx, ObPxCoordCtx& px_ctx, ObDfo* root_dfo) const
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx* phy_plan_ctx = GET_PHY_PLAN_CTX(ctx);
  if (OB_ISNULL(phy_plan_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect status: physical plan context is null", K(ret));
  } else if (OB_ISNULL(phy_plan_ctx->get_phy_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpect status: physical plan is null", K(ret));
  } else {
    int64_t dop = phy_plan_ctx->get_phy_plan()->get_px_dop();
    if (OB_FAIL(px_ctx.first_buffer_cache_.init(dop, dop))) {
      LOG_WARN("failed to init first buffer cache", K(ret));
    } else {
      ObDtlDfoKey dfo_key;
      dfo_key.set(GCTX.server_id_, root_dfo->get_px_sequence_id(), root_dfo->get_qc_id(), root_dfo->get_dfo_id());
      px_ctx.first_buffer_cache_.set_first_buffer_key(dfo_key);
      px_ctx.msg_loop_.set_first_buffer_cache(&px_ctx.first_buffer_cache_);
      if (OB_FAIL(DTL.get_dfc_server().register_first_buffer_cache(
              ctx.get_my_session()->get_effective_tenant_id(), px_ctx.get_first_buffer_cache()))) {
        LOG_WARN("failed to register first buffer cache", K(ret));
      }
      LOG_TRACE("trace QC register first buffer cache", K(dfo_key), K(dop));
    }
  }
  return ret;
}

void ObPxCoord::unregister_first_buffer_cache(ObExecContext& ctx, ObPxCoordCtx& px_ctx) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(DTL.get_dfc_server().unregister_first_buffer_cache(ctx.get_my_session()->get_effective_tenant_id(),
          px_ctx.first_buffer_cache_.get_first_buffer_key(),
          &px_ctx.first_buffer_cache_))) {
    LOG_WARN("failed to register first buffer cache", K(ret));
  }
  LOG_TRACE("trace QC unregister first buffer cache", K(px_ctx.first_buffer_cache_.get_first_buffer_key()));
}

int ObPxCoord::inner_open(ObExecContext& ctx) const
{
  int ret = OB_SUCCESS;
  ObPxCoordCtx* px_ctx = NULL;
  ObDfo* root_dfo = NULL;
  int64_t px_sequence_id = 0;
  if (OB_FAIL(init_op_ctx(ctx))) {
    LOG_WARN("init operator context failed", K(ret));
  } else if (OB_ISNULL(px_ctx = GET_PHY_OPERATOR_CTX(ObPxCoordCtx, ctx, get_id()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get physical operator context failed", K(ret));
  } else if (OB_FAIL(post_init_op_ctx(ctx, *px_ctx))) {
    LOG_WARN("init operator context failed", K(ret));
  } else if (FALSE_IT(px_ctx->px_sequence_id_ = GCTX.sql_engine_->get_px_sequence_id())) {
    LOG_WARN("fail to get px sequence id", K(ret));
  } else if (OB_FAIL(register_interrupt(px_ctx))) {
    LOG_WARN("fail to register interrupt", K(ret));
  } else if (OB_FAIL(init_dfo_mgr(ObDfoInterruptIdGen(px_ctx->interrupt_id_,
                                      (uint32_t)GCTX.server_id_,
                                      (uint32_t)px_ctx->qc_id_,
                                      px_ctx->px_sequence_id_),
                 ctx,
                 px_ctx->coord_info_.dfo_mgr_))) {
    LOG_WARN("fail parse dfo tree",
        "server_id",
        GCTX.server_id_,
        "qc_id",
        px_ctx->qc_id_,
        "execution_id",
        ctx.get_my_session()->get_current_execution_id(),
        K(ret));
  } else if (OB_ISNULL(root_dfo = px_ctx->coord_info_.dfo_mgr_.get_root_dfo())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("NULL root dfo", K(ret));
  } else if (OB_FAIL(setup_op_input(ctx, *root_dfo))) {
    LOG_WARN("fail setup all receive/transmit op input", K(ret));
  } else if (OB_FAIL(register_first_buffer_cache(ctx, *px_ctx, root_dfo))) {
    LOG_WARN("failed to register first buffer cache", K(ret));
  } else {
    debug_print(*root_dfo);
  }
  return ret;
}

int ObPxCoord::setup_loop_proc(ObExecContext& ctx, ObPxCoordCtx& px_ctx) const
{
  int ret = OB_ERR_UNEXPECTED;
  UNUSED(ctx);
  UNUSED(px_ctx);
  return ret;
}

int ObPxCoord::create_operator_input(ObExecContext& ctx) const
{
  int ret = OB_SUCCESS;
  ObIPhyOperatorInput* input = NULL;
  if (OB_FAIL(CREATE_PHY_OP_INPUT(ObIPhyOperatorInput, ctx, get_id(), get_type(), input))) {
    LOG_WARN("fail to create phy op input", K(ret), K(get_id()), K(get_type()));
  }
  UNUSED(input);
  return ret;
}

int ObPxCoord::post_init_op_ctx(ObExecContext& ctx, ObPxCoordCtx& op_ctx) const
{
  int ret = OB_SUCCESS;
  ObPhyOperator* child = nullptr;
  ObTransmit* transmit = nullptr;
  UNUSED(ctx);
  if (OB_FAIL(init_obrpc_proxy(op_ctx.coord_info_.rpc_proxy_))) {
    LOG_WARN("fail init rpc proxy", K(ret));
  } else if (OB_FAIL(init_cur_row(op_ctx, true))) {
    LOG_WARN("fail init px coord cur row", K(ret));
  } else if (nullptr == (child = get_child(0))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("coord op should have a child", K(ret));
  } else if (nullptr == (transmit = dynamic_cast<ObTransmit*>(child))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail cast coord child op to transmit.", K(ret));
  } else {
    // trick: Since Receive operator not record any meta info of DFO,
    // get px id from child of ObPxCoord.
    op_ctx.qc_id_ = transmit->get_px_id();
  }

  if (OB_SUCC(ret)) {
    ObPhysicalPlanCtx* phy_plan_ctx = GET_PHY_PLAN_CTX(ctx);
    if (OB_ISNULL(phy_plan_ctx)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect status: physical plan context is null", K(ret));
    } else if (OB_ISNULL(phy_plan_ctx->get_phy_plan())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect status: physical plan is null", K(ret));
    } else {
      op_ctx.px_dop_ = phy_plan_ctx->get_phy_plan()->get_px_dop();
    }
  }
  return ret;
}

int ObPxCoord::init_dfo_mgr(const ObDfoInterruptIdGen& dfo_id_gen, ObExecContext& exec_ctx, ObDfoMgr& dfo_mgr) const
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx* phy_plan_ctx = nullptr;
  ObTaskExecutorCtx* task_exec_ctx = nullptr;
  if (OB_ISNULL(phy_plan_ctx = GET_PHY_PLAN_CTX(exec_ctx))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("phy plan ctx NULL", K(ret));
  } else if (OB_ISNULL(task_exec_ctx = GET_TASK_EXECUTOR_CTX(exec_ctx))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("task exec ctx NULL", K(ret));
  } else {
    // px_allocated_worker_count is the number of workers current PX can use.
    // Method to calculate number of workers:
    // PX expected number is a,
    // Query expected number is b and query actually got number is c. They are record in ExecContext.
    // px_allocated_worker_count = a * c / b;
    // This means when number of workers query got reduced, all PX in the query got fewer workers.

    // how many threads current px need, calculated by optimizer.
    int64_t px_expected = get_expected_worker_count();
    // how many threads query need, calculated by optimizer.
    int64_t query_expected = task_exec_ctx->get_expected_worker_cnt();
    // how many threads are allocated to query actually.
    int64_t query_allocated = task_exec_ctx->get_allocated_worker_cnt();

    int64_t px_allocated_worker_count = 0;
    if (OB_FAIL(calc_allocated_worker_count(px_expected, query_expected, query_allocated, px_allocated_worker_count))) {
      LOG_WARN("fail allocate worker count for px", K(px_expected), K(query_expected), K(query_allocated), K(ret));
    } else if (OB_FAIL(dfo_mgr.init(exec_ctx, *this, px_expected, px_allocated_worker_count, dfo_id_gen))) {
      LOG_WARN("fail init dfo mgr",
          K(px_expected),
          K(query_expected),
          K(query_allocated),
          K(px_allocated_worker_count),
          K(ret));
    }
  }
  return ret;
}

int ObPxCoord::calc_allocated_worker_count(
    int64_t px_expected, int64_t query_expected, int64_t query_allocated, int64_t& allocated_worker_count) const
{
  int ret = OB_SUCCESS;
  if (query_expected > 0 && 0 >= query_allocated) {
    ret = OB_ERR_INSUFFICIENT_PX_WORKER;
    LOG_WARN("not enough thread resource", K(px_expected), K(query_allocated), K(query_expected), K(ret));
  } else if (0 == query_expected) {
    // note: for query select single table or dop=1, fast dfo will be selected,
    // and query_expected is zero now.
    allocated_worker_count = 0;
  } else {
    allocated_worker_count =
        static_cast<int64_t>((double)px_expected * (double)query_allocated / (double)query_expected);
  }
  LOG_TRACE(
      "calc px worker count", K(px_expected), K(query_expected), K(query_allocated), K(allocated_worker_count), K(ret));
  return ret;
}

int ObPxCoord::terminate_running_dfos(ObExecContext& ctx, ObDfoMgr& dfo_mgr) const
{
  int ret = OB_SUCCESS;
  // notify all running dfo exit
  ObSEArray<ObDfo*, 32> dfos;
  if (OB_FAIL(dfo_mgr.get_running_dfos(dfos))) {
    LOG_WARN("fail find dfo", K(ret));
  } else if (OB_FAIL(ObInterruptUtil::broadcast_px(dfos, OB_GOT_SIGNAL_ABORTING))) {
    LOG_WARN("fail broadcast interrupt to all of a px", K(ret));
  } else if (!dfos.empty() && OB_FAIL(wait_all_running_dfos_exit(ctx))) {
    LOG_WARN("fail to exit dfo", K(ret));
  }
  return ret;
}

int ObPxCoord::setup_op_input(ObExecContext& ctx, ObDfo& root) const
{
  int ret = OB_SUCCESS;
  int64_t cnt = root.get_child_count();
  for (int64_t idx = 0; idx < cnt && OB_SUCC(ret); ++idx) {
    ObDfo* child_dfo = NULL;
    if (OB_FAIL(root.get_child_dfo(idx, child_dfo))) {
      LOG_WARN("fail get child_dfo", K(idx), K(cnt), K(ret));
    } else if (OB_ISNULL(child_dfo)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child_dfo is null", K(ret));
    } else if (OB_ISNULL(child_dfo->get_root_op()) || OB_ISNULL(child_dfo->get_root_op()->get_parent())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("child_dfo op is null", K(ret));
    } else {
      ObPhyOperator& receive = *child_dfo->get_root_op()->get_parent();
      if (IS_PX_RECEIVE(receive.get_type())) {
        ObPxReceiveInput* input = GET_PHY_OP_INPUT(ObPxReceiveInput, ctx, receive.get_id());
        if (OB_ISNULL(input)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("op input is null, maybe not created", "id", receive.get_id(), K(ret));
        } else {
          input->set_child_dfo_id(child_dfo->get_dfo_id());
          if (root.is_root_dfo()) {
            input->set_task_id(0);  // there is only one task in root dfo
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      // set receive input of child recursively.
      if (OB_FAIL(setup_op_input(ctx, *child_dfo))) {
        LOG_WARN("fail setup op input", K(ret));
      }
    }
  }
  return ret;
}

int ObPxCoord::inner_close(ObExecContext& ctx) const
{
  int ret = OB_SUCCESS;
  ObPxCoordCtx* px_ctx = NULL;
  if (OB_ISNULL(px_ctx = GET_PHY_OPERATOR_CTX(ObPxCoordCtx, ctx, get_id()))) {
    LOG_DEBUG("The operator has not been opened.", K(ret), K_(id), "op_type", ob_phy_operator_type_str(get_type()));
  } else {
    // ignore terminate error code of while closing.
    int terminate_ret = OB_SUCCESS;
    if (OB_SUCCESS != (terminate_ret = terminate_running_dfos(ctx, px_ctx->coord_info_.dfo_mgr_))) {
      LOG_WARN("fail to terminate running dfo, ignore ret", K(terminate_ret));
    }
    unregister_first_buffer_cache(ctx, *px_ctx);
    (void)handle_monitor_info(*px_ctx, ctx);
    int release_channel_ret = OB_SUCCESS;
    if (OB_SUCCESS != (release_channel_ret = destroy_all_channel(*px_ctx))) {
      LOG_WARN("release dtl channel failed", K(release_channel_ret));
    }
    (void)clear_interrupt(px_ctx);
  }
  LOG_TRACE("byebye. exit QC Coord");
  return ret;
}

// format: |id1#v1,v2,...+id2#v1,v2,...|, '|' is seperator for top element
//         '+' is seperator for metric
// separate time statistics and operator information with '|'
// separate information of one operator with '+'
int ObPxCoord::write_op_metric_info(
    ObPxTaskMonitorInfo& task_monitor_info, char* metric_info, int64_t len, int64_t& pos) const
{
  int ret = OB_SUCCESS;
  static const char top_element_seperate[2] = "|";
  static const char metric_seperate[2] = "+";
  ObIArray<ObOpMetric>& metrics = task_monitor_info.get_op_metrics();
  if (0 < metrics.count()) {
    for (int64_t nth = 0; nth < metrics.count() && OB_SUCC(ret); ++nth) {
      ObOpMetric& metric = metrics.at(nth);
      if (metric.get_enable_audit()) {
        if (0 == nth) {
          if (OB_FAIL(databuff_printf(metric_info, len, pos, "%s", top_element_seperate))) {
            LOG_WARN("fail to print buff", K(ret));
          }
        } else {
          if (OB_FAIL(databuff_printf(metric_info, len, pos, "%s", metric_seperate))) {
            LOG_WARN("fail to print buff", K(ret));
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(databuff_printf(metric_info,
                                len,
                                pos,
                                "%ld#%d,%ld,%ld,%ld,%ld,%ld",
                                metric.get_id(),
                                metric.get_type(),
                                metric.get_first_in_ts(),
                                metric.get_first_out_ts(),
                                metric.get_last_in_ts(),
                                metric.get_last_out_ts(),
                                metric.get_counter()))) {
          LOG_WARN("fail to print buff", K(ret));
        }
      } else {
        LOG_TRACE("disable op metric", K(ret));
      }
    }
  }
  if (0 == pos) {
    ret = OB_ERR_UNEXPECTED;
    LOG_TRACE("no metric data", K(ret));
  }
  return ret;
}

int ObPxCoord::handle_monitor_info(ObPxCoordCtx& px_ctx, ObExecContext& ctx) const
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo* session = NULL;
  if (!GCONF.enable_sql_audit) {
    // do nothing
  } else if (OB_ISNULL(session = ctx.get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is null or px_ctx is null", K(ret));
  } else {
    ObSchedInfo& sched_info = ctx.get_sched_info();
    char* sched_info_array = NULL;
    if (sched_info.get_len() < 0 || sched_info.get_len() > OB_MAX_SCHED_INFO_LENGTH) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("sched_info_len is invalid", K(ret), K(sched_info.get_len()));
    } else if (OB_ISNULL(sched_info_array = static_cast<char*>(ctx.get_allocator().alloc(OB_MAX_SCHED_INFO_LENGTH)))) {
      /*fail to alloc memory, do nothing*/
    } else {
      int64_t pos = 0;
      const common::ObIArray<ObDfo*>& edges = px_ctx.coord_info_.dfo_mgr_.get_all_dfos();
      char metric_info[1024];
      int tmp_ret = OB_SUCCESS;
      ARRAY_FOREACH_X(edges, idx, cnt, OB_SUCC(ret))
      {
        ObDfo* edge = edges.at(idx);
        ObSEArray<ObPxSqcMeta*, 16> sqcs;
        if (OB_FAIL(edge->get_sqcs(sqcs))) {
          LOG_WARN("fail to get sqcs", K(ret));
        } else {
          ARRAY_FOREACH_X(sqcs, sqc_idx, sqc_cnt, OB_SUCC(ret))
          {
            ObPxTaskMonitorInfoArray& task_monitor_info_array = sqcs.at(sqc_idx)->get_task_monitor_info_array();
            ARRAY_FOREACH_X(task_monitor_info_array, info_idx, info_cnt, OB_SUCC(ret))
            {
              int64_t tmp_pos = 0;
              // skip write content of dtl temporarily, since string is too long and buffer is not enough.
              // And it's more appropriate to put content of metric info in sql_plan_monitor later.
              if (false && OB_SUCCESS != (tmp_ret = write_op_metric_info(
                                              task_monitor_info_array.at(info_idx), metric_info, 1024, tmp_pos))) {
                // if failed to write metric info, ignore
                LOG_TRACE("fail to print buff for metric info", K(tmp_ret));
              }
              if (false && OB_SUCCESS == tmp_ret) {
                if (OB_FAIL(databuff_printf(sched_info_array,
                        OB_MAX_SCHED_INFO_LENGTH - sched_info.get_len(),
                        pos,
                        "J%ld%04ldT%ld%04ld:%ld,%ld,%ld%s;",
                        sqcs.at(sqc_idx)->get_qc_id(),
                        sqcs.at(sqc_idx)->get_dfo_id(),
                        sqcs.at(sqc_idx)->get_sqc_id(),
                        info_idx,
                        task_monitor_info_array.at(info_idx).get_task_start_timestamp(),
                        task_monitor_info_array.at(info_idx).get_exec_time(),
                        task_monitor_info_array.at(info_idx).get_sched_exec_time(),
                        metric_info))) {
                  LOG_WARN("fail to print buff", K(ret));
                }
              } else {
                if (OB_FAIL(databuff_printf(sched_info_array,
                        OB_MAX_SCHED_INFO_LENGTH - sched_info.get_len(),
                        pos,
                        "J%ld%04ldT%ld%04ld:%ld,%ld,%ld;",
                        sqcs.at(sqc_idx)->get_qc_id(),
                        sqcs.at(sqc_idx)->get_dfo_id(),
                        sqcs.at(sqc_idx)->get_sqc_id(),
                        info_idx,
                        task_monitor_info_array.at(info_idx).get_task_start_timestamp(),
                        task_monitor_info_array.at(info_idx).get_exec_time(),
                        task_monitor_info_array.at(info_idx).get_sched_exec_time()))) {
                  LOG_WARN("fail to print buff", K(ret));
                }
              }
            }
          }
        }
      }
      if (OB_LIKELY(pos > 0)) {
        sched_info.append(ctx.get_allocator(), sched_info_array, pos);
      }
    }
  }
  return ret;
}

int ObPxCoord::destroy_all_channel(ObPxCoordCtx& px_ctx) const
{
  int ret = OB_SUCCESS;
  // note: must unregister channel from msg_loop first. This enable unlink_channel safely.
  // Otherwise, channnel may receive data while unlink_channel,
  // and result in memory leak or something else.
  int tmp_ret = OB_SUCCESS;
  if (OB_SUCCESS != (tmp_ret = px_ctx.msg_loop_.unregister_all_channel())) {
    LOG_WARN("fail unregister channels from msg_loop. ignore", KR(tmp_ret));
  }

  const ObIArray<ObDfo*>& dfos = px_ctx.coord_info_.dfo_mgr_.get_all_dfos();
  /* even if one channel unlink failed, we still continue destroy other channel */
  ARRAY_FOREACH_X(dfos, idx, cnt, true)
  {
    const ObDfo* edge = dfos.at(idx);
    ObSEArray<const ObPxSqcMeta*, 16> sqcs;
    if (OB_FAIL(edge->get_sqcs(sqcs))) {
      LOG_WARN("fail to get sqcs", K(ret));
    } else {
      /* one channel unlink failed still continue destroy other channel */
      ARRAY_FOREACH_X(sqcs, sqc_idx, sqc_cnt, true)
      {
        const ObDtlChannelInfo& qc_ci = sqcs.at(sqc_idx)->get_qc_channel_info_const();
        const ObDtlChannelInfo& sqc_ci = sqcs.at(sqc_idx)->get_sqc_channel_info_const();
        if (OB_FAIL(ObDtlChannelGroup::unlink_channel(qc_ci))) {
          LOG_WARN("fail unlink channel", K(qc_ci), K(ret));
        }
        /*
         * actually, the qc and sqc can see the channel id of sqc.
         * sqc channel's onwer is SQC, not QC.
         * if we release there, all these channel will be release twice.
         * So, sqc channel will be release by sqc, not qc.
         *
         * */
        UNUSED(sqc_ci);
      }
    }
  }
  /*
   * release root task channel here.
   * */
  if (OB_FAIL(ObPxChannelUtil::unlink_ch_set(px_ctx.task_ch_set_, &px_ctx.dfc_))) {
    LOG_WARN("unlink channel failed", K(ret));
  }
  return ret;
}

int ObPxCoord::try_link_channel(ObExecContext& ctx) const
{
  int ret = OB_SUCCESS;
  // channel link that qc receive last msg with is in own get_next loop, need to do nothing here.

  UNUSED(ctx);
  return ret;
}

int ObPxCoord::wait_all_running_dfos_exit(ObExecContext& ctx) const
{
  int ret = OB_SUCCESS;
  ObPxCoordCtx* px_ctx_ptr = NULL;
  ObPhysicalPlanCtx* phy_plan_ctx = NULL;
  ObSEArray<ObDfo*, 32> active_dfos;
  bool all_dfo_terminate = false;
  ObIPxCoordEventListener* listener = NULL;
  int64_t timeout_us = 0;
  const ObNewRow* row = NULL;
  int64_t nth_channel = OB_INVALID_INDEX_INT64;
  bool collect_trans_result_ok = false;

  if (OB_ISNULL(px_ctx_ptr = GET_PHY_OPERATOR_CTX(ObPxCoordCtx, ctx, get_id()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail get op ctx", K(get_id()), K(ret));
  } else if (OB_FAIL(px_ctx_ptr->coord_info_.dfo_mgr_.get_running_dfos(active_dfos))) {
    LOG_WARN("fail find dfo", K(ret));
  } else if (OB_ISNULL(phy_plan_ctx = GET_PHY_PLAN_CTX(ctx))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("phy plan ctx NULL", K(ret));
  } else if (OB_UNLIKELY(!px_ctx_ptr->first_row_fetched_)) {
    // no dfo sent, do nothing.
    collect_trans_result_ok = true;
    ret = OB_ITER_END;
  }

  if (OB_SUCC(ret)) {
    ObPxCoordCtx& px_ctx = *px_ctx_ptr;
    // wait for msg proc from all active sqc.
    ObDtlChannelLoop& loop = px_ctx.msg_loop_;
    ObIPxCoordEventListener& listener = px_ctx_ptr->get_listenner();
    ObPxTerminateMsgProc terminate_msg_proc(px_ctx.coord_info_, listener);
    ObPxFinishSqcResultP sqc_finish_msg_proc(ctx, terminate_msg_proc);
    ObPxInitSqcResultP sqc_init_msg_proc(ctx, terminate_msg_proc);
    ObBarrierPieceMsgP barrier_piece_msg_proc(ctx, terminate_msg_proc);
    ObWinbufPieceMsgP winbuf_piece_msg_proc(ctx, terminate_msg_proc);
    ObPxQcInterruptedP interrupt_proc(ctx, terminate_msg_proc);

    // this register replaces old proc.
    (void)px_ctx.msg_loop_.clear_all_proc();
    (void)px_ctx.msg_loop_.register_processor(sqc_finish_msg_proc)
        .register_processor(sqc_init_msg_proc)
        .register_processor(barrier_piece_msg_proc)
        .register_processor(winbuf_piece_msg_proc)
        .register_processor(px_ctx.px_row_msg_proc_)
        .register_interrupt_processor(interrupt_proc);
    loop.ignore_interrupt();

    ObPxControlChannelProc control_channels;
    int64_t times_offset = 0;
    int64_t last_timestamp = 0;
    bool wait_msg = true;
    while (OB_SUCC(ret) && wait_msg) {
      ObDtlChannelLoop& loop = px_ctx.msg_loop_;
      timeout_us = phy_plan_ctx->get_timeout_timestamp() - px_ctx.get_timestamp();

      /**
       * start to get next msg.
       */
      if (OB_FAIL(check_all_sqc(active_dfos, times_offset++, all_dfo_terminate, last_timestamp))) {
        LOG_WARN("fail to check sqc");
      } else if (all_dfo_terminate) {
        wait_msg = false;
        collect_trans_result_ok = true;
        LOG_TRACE("all dfo has been terminate", K(ret));
      } else if (OB_FAIL(ctx.fast_check_status())) {
        LOG_WARN("fail check status, maybe px query timeout", K(ret));
      } else if (OB_FAIL(loop.process_one_if(&control_channels, timeout_us, nth_channel))) {
        if (OB_EAGAIN == ret) {
          LOG_DEBUG("no msessage, waiting sqc report", K(ret));
          ret = OB_SUCCESS;
        } else if (OB_ITER_END != ret) {
          LOG_WARN("fail process message", K(ret));
        }
      } else {
        ObDtlMsgType msg_type = loop.get_last_msg_type();
        /**
         * sqc finish msg has been handled in callback process.
         * discard all msg directly.
         */
        switch (msg_type) {
          case ObDtlMsgType::PX_NEW_ROW:
          case ObDtlMsgType::INIT_SQC_RESULT:
          case ObDtlMsgType::FINISH_SQC_RESULT:
          case ObDtlMsgType::DH_BARRIER_PIECE_MSG:
          case ObDtlMsgType::DH_WINBUF_PIECE_MSG:
            break;
          default:
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("Unexpected msg type", K(msg_type));
            break;
        }
      }
    }
  }
  // need to filter 4662 error code, since QC broadcast 4662 to all dfos when it wants to interrupt
  // all dfos and exist, and this error code may be reported back by sqc.
  if (OB_NOT_NULL(px_ctx_ptr) && OB_GOT_SIGNAL_ABORTING != px_ctx_ptr->coord_info_.first_error_code_ &&
      OB_ERR_SIGNALED_IN_PARALLEL_QUERY_SERVER != px_ctx_ptr->coord_info_.first_error_code_) {
    ret = px_ctx_ptr->coord_info_.first_error_code_;
  }

  if (!collect_trans_result_ok) {
    ObSQLSessionInfo* session = ctx.get_my_session();
    if (OB_ISNULL(session)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("session is null or px_ctx is null", K(ret));
    } else {
      session->get_trans_result().set_incomplete();
      LOG_WARN("collect trans_result fail",
          K(ret),
          "session_id",
          session->get_sessid(),
          "trans_result",
          session->get_trans_result());
    }
  }
  return ret;
}

int ObPxCoord::check_all_sqc(ObIArray<ObDfo *> &active_dfos,
                             int64_t times_offset,
                             bool &all_dfo_terminate, 
                             int64_t &last_timestamp) const
{
  int ret = OB_SUCCESS;
  all_dfo_terminate = true;

  for (int64_t i = 0; i < active_dfos.count() && all_dfo_terminate && OB_SUCC(ret); ++i) {
    ObArray<ObPxSqcMeta*> sqcs;
    if (OB_FAIL(active_dfos.at(i)->get_sqcs(sqcs))) {
      LOG_WARN("fail get qc-sqc channel for QC", K(ret));
    } else {
      bool sqc_threads_inited = true;
      ARRAY_FOREACH_X(sqcs, idx, cnt, OB_SUCC(ret))
      {
        ObPxSqcMeta* sqc = sqcs.at(idx);
        if (OB_ISNULL(sqc)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("NULL unexpected sqc", K(ret));
        } else if (sqc->need_report()) {
          LOG_DEBUG("wait for sqc", K(sqc));
          int64_t cur_timestamp = ObTimeUtility::current_time();
          // > 1s, increase gradually
          // In order to get the dfo to propose as soon as possible and 
          // In order to avoid the interruption that is not received, 
          // So the interruption needs to be sent repeatedly
          if (cur_timestamp - last_timestamp > (1000000 + min(times_offset, 10) * 1000000)) { 
            last_timestamp = cur_timestamp;
            ObInterruptUtil::broadcast_dfo(active_dfos.at(i), OB_GOT_SIGNAL_ABORTING);
          }
          all_dfo_terminate = false;
          break;
        }
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////

OB_SERIALIZE_MEMBER((ObPxCoord, ObPxReceive), px_expected_worker_count_);

int ObPxCoord::register_interrupt(ObPxCoordCtx* px_ctx) const
{
  int ret = OB_SUCCESS;
  ObInterruptUtil::generate_query_interrupt_id(
      (uint32_t)GCTX.server_id_, px_ctx->px_sequence_id_, px_ctx->interrupt_id_);
  if (OB_FAIL(SET_INTERRUPTABLE(px_ctx->interrupt_id_))) {
    LOG_WARN("fail to register interrupt", K(ret));
  } else {
    px_ctx->register_interrupted_ = true;
  }
  LOG_TRACE("QC register interrupt", K(ret));
  return ret;
}

void ObPxCoord::clear_interrupt(ObPxCoordCtx* px_ctx) const
{
  if (px_ctx->register_interrupted_) {
    UNSET_INTERRUPTABLE(px_ctx->interrupt_id_);
    px_ctx->register_interrupted_ = false;
  }
  LOG_TRACE("unregister interrupt");
}

int ObPxCoord::ObPxCoordCtx::receive_channel_root_dfo(
    ObExecContext& ctx, ObDfo& parent_dfo, ObPxTaskChSets& parent_ch_sets)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(task_ch_set_.assign(parent_ch_sets.at(0)))) {
    LOG_WARN("fail assign data", K(ret));
  } else if (OB_FAIL(init_dfc(ctx, parent_dfo))) {
    LOG_WARN("Failed to init dfc", K(ret));
  } else if (OB_FAIL(ObPxReceive::link_ch_sets(task_ch_set_, task_channels_, &dfc_))) {
    LOG_WARN("fail link px coord data channels with its only child dfo", K(ret));
  } else {
    if (OB_FAIL(get_listenner().on_root_data_channel_setup())) {
      LOG_WARN("fail notify listener", K(ret));
    }
    bool enable_audit = GCONF.enable_sql_audit && ctx.get_my_session()->get_local_ob_enable_sql_audit();
    metric_.init(enable_audit);
    msg_loop_.set_tenant_id(ctx.get_my_session()->get_effective_tenant_id());
    msg_loop_.set_monitor_info(&op_monitor_info_);
    // receive channel sets of root dfo are used in local machine, won't be sent by DTL,
    // so just register into msg_loop to receive data.
    int64_t cnt = task_channels_.count();
    for (int64_t idx = 0; idx < cnt && OB_SUCC(ret); ++idx) {
      dtl::ObDtlChannel* ch = task_channels_.at(idx);
      if (OB_ISNULL(ch)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("NULL unexpected", K(ch), K(ret));
      } else {
        ch->set_audit(enable_audit);
      }
      LOG_TRACE("link qc-task channel and registered to qc msg loop. ready to receive task data msg",
          K(idx),
          K(cnt),
          "ch",
          *ch,
          KP(ch->get_id()),
          K(ch->get_peer()));
    }
  }
  return ret;
}

int ObPxCoord::ObPxCoordCtx::receive_channel_root_dfo(ObExecContext& ctx, ObDfo& parent_dfo, ObDtlChTotalInfo& ch_info)
{
  int ret = OB_SUCCESS;
  ObPxTaskChSets tmp_ch_sets;
  if (OB_FAIL(ObPxChProviderUtil::inner_get_data_ch(true, tmp_ch_sets, ch_info, 0, 0, task_ch_set_, false))) {
    LOG_WARN("fail get data ch set", K(ret));
  } else if (OB_FAIL(init_dfc(ctx, parent_dfo))) {
    LOG_WARN("Failed to init dfc", K(ret));
  } else if (OB_FAIL(ObPxReceive::link_ch_sets(task_ch_set_, task_channels_, &dfc_))) {
    LOG_WARN("fail link px coord data channels with its only child dfo", K(ret));
  } else {
    if (OB_FAIL(get_listenner().on_root_data_channel_setup())) {
      LOG_WARN("fail notify listener", K(ret));
    }
    bool enable_audit = GCONF.enable_sql_audit && ctx.get_my_session()->get_local_ob_enable_sql_audit();
    metric_.init(enable_audit);
    msg_loop_.set_tenant_id(ctx.get_my_session()->get_effective_tenant_id());
    msg_loop_.set_monitor_info(&op_monitor_info_);
    // receive channel sets of root dfo are used in local machine, won't be sent by DTL,
    // so just register into msg_loop to receive data.
    int64_t cnt = task_channels_.count();
    for (int64_t idx = 0; idx < cnt && OB_SUCC(ret); ++idx) {
      dtl::ObDtlChannel* ch = task_channels_.at(idx);
      if (OB_ISNULL(ch)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("NULL unexpected", K(ch), K(ret));
      } else {
        ch->set_audit(enable_audit);
      }
      LOG_TRACE("link qc-task channel and registered to qc msg loop. ready to receive task data msg",
          K(idx),
          K(cnt),
          "ch",
          *ch,
          KP(ch->get_id()),
          K(ch->get_peer()));
    }
  }
  return ret;
}
