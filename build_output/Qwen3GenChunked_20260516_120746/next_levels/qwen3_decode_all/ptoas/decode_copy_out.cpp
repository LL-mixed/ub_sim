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

__global__ AICORE void decode_copy_out(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, int32_t v3, int32_t v4, int32_t v5, int32_t v6) {
  unsigned v7 = 1024;
  unsigned v8 = 0;
  const int32_t v9 = 128;
  const int32_t v10 = 1;
  const int32_t v11 = 1024;
  const int64_t v12 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v13 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v5, v9);
  TASSIGN(v13, v12);
  Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v14 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v5, v9);
  __ubuf__ bfloat16_t* v15 = v13.data();
  uint64_t v16 = reinterpret_cast<uint64_t>(v15);
  TASSIGN(v14, v16);
  unsigned v17 = (unsigned) v5 * v7;
  pto::Shape<1, 1, 1, -1, 128> v18 = pto::Shape<1, 1, 1, -1, 128>(v5);
  pto::Stride<-1, -1, -1, 1024, 1> v19 = pto::Stride<-1, -1, -1, 1024, 1>(v17, v17, v17);
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND> v20 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND>(v1 + (v8 + (unsigned) v3 * (unsigned) v11 + (unsigned) v4 * (unsigned) v10), v18, v19);
  TLOAD(v14, v20);
  set_flag(PIPE_MTE2, PIPE_MTE3, EVENT_ID0);
  unsigned v21 = (unsigned) v5 * v7;
  pto::Shape<1, 1, 1, -1, 128> v22 = pto::Shape<1, 1, 1, -1, 128>(v5);
  pto::Stride<-1, -1, -1, 1024, 1> v23 = pto::Stride<-1, -1, -1, 1024, 1>(v21, v21, v21);
  GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND> v24 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND>(v2 + (v8 + (unsigned) v3 * (unsigned) v11 + (unsigned) v4 * (unsigned) v10), v22, v23);
  wait_flag(PIPE_MTE2, PIPE_MTE3, EVENT_ID0);
  TSTORE(v24, v14);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
