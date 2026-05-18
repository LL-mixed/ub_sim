#include "pto/pto-inst.hpp"
using namespace pto;

enum class PTOAutoSyncTailMode : int {
  kBarrierAll = 0,
  kSetWaitMte3ToSEvent0 = 1,
};

static AICORE inline void ptoas_auto_sync_tail(
    PTOAutoSyncTailMode mode = PTOAutoSyncTailMode::kBarrierAll) {
  switch (mode) {
  case PTOAutoSyncTailMode::kSetWaitMte3ToSEvent0:
    set_flag(PIPE_MTE3, PIPE_S, EVENT_ID0);
    wait_flag(PIPE_MTE3, PIPE_S, EVENT_ID0);
    break;
  case PTOAutoSyncTailMode::kBarrierAll:
  default:
    pipe_barrier(PIPE_ALL);
    break;
  }
}

__global__ AICORE void prefill_copy_out(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, int32_t v3, int32_t v4, int32_t v5, int32_t v6, int32_t v7) {
  unsigned v8 = 0;
  const int32_t v9 = 2621440;
  const int32_t v10 = 128;
  const int32_t v11 = 64;
  const int32_t v12 = 1;
  const int32_t v13 = 5120;
  const int64_t v14 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v15 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  TASSIGN(v15, v14);
  Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Vec, bfloat16_t, 64, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v10);
  __ubuf__ bfloat16_t* v17 = v15.data();
  uint64_t v18 = reinterpret_cast<uint64_t>(v17);
  TASSIGN(v16, v18);
  pto::Shape<1, 1, 1, 64, 128> v19 = pto::Shape<1, 1, 1, 64, 128>();
  pto::Stride<2621440, 2621440, 2621440, 5120, 1> v20 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v21 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v1 + ((v8 + (unsigned) v3 * (unsigned) v9) + (unsigned) v4 * (unsigned) v13 + (unsigned) v5 * (unsigned) v12), v19, v20);
  TLOAD(v16, v21);
  set_flag(PIPE_MTE2, PIPE_MTE3, EVENT_ID0);
  pto::Shape<1, 1, 1, 64, 128> v22 = pto::Shape<1, 1, 1, 64, 128>();
  pto::Stride<2621440, 2621440, 2621440, 5120, 1> v23 = pto::Stride<2621440, 2621440, 2621440, 5120, 1>();
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND> v24 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 64, 128>, pto::Stride<2621440, 2621440, 2621440, 5120, 1>, pto::Layout::ND>(v2 + ((v8 + (unsigned) v3 * (unsigned) v9) + (unsigned) v4 * (unsigned) v13 + (unsigned) v5 * (unsigned) v12), v22, v23);
  wait_flag(PIPE_MTE2, PIPE_MTE3, EVENT_ID0);
  TSTORE(v24, v16);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
