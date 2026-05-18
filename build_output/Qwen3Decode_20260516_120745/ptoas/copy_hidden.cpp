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

__global__ AICORE void copy_hidden(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2, int32_t v3, int32_t v4, int32_t v5) {
  unsigned v6 = 1024;
  unsigned v7 = 0;
  const int32_t v8 = 128;
  const int32_t v9 = 8;
  const int32_t v10 = 0;
  const int32_t v11 = 1;
  const int32_t v12 = 1024;
  const int64_t v13 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  for (size_t v14 = (size_t) v10; v14 < ((size_t) v9); v14 += (size_t) v11) {
    int32_t v15 = (int32_t) ((uint32_t) ((int32_t) v14) * (uint32_t) v8);
    Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v4, v8);
    TASSIGN(v16, v13);
    Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v17 = Tile<TileType::Vec, bfloat16_t, 16, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v4, v8);
    __ubuf__ bfloat16_t* v18 = v16.data();
    uint64_t v19 = reinterpret_cast<uint64_t>(v18);
    TASSIGN(v17, v19);
    unsigned v20 = (unsigned) v4 * v6;
    pto::Shape<1, 1, 1, -1, 128> v21 = pto::Shape<1, 1, 1, -1, 128>(v4);
    pto::Stride<-1, -1, -1, 1024, 1> v22 = pto::Stride<-1, -1, -1, 1024, 1>(v20, v20, v20);
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND> v23 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND>(v2 + (v7 + (unsigned) v3 * (unsigned) v12 + (unsigned) v15 * (unsigned) v11), v21, v22);
    wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
    TLOAD(v17, v23);
    set_flag(PIPE_MTE2, PIPE_MTE3, EVENT_ID0);
    unsigned v24 = (unsigned) v4 * v6;
    pto::Shape<1, 1, 1, -1, 128> v25 = pto::Shape<1, 1, 1, -1, 128>(v4);
    pto::Stride<-1, -1, -1, 1024, 1> v26 = pto::Stride<-1, -1, -1, 1024, 1>(v24, v24, v24);
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND> v27 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, -1, 128>, pto::Stride<-1, -1, -1, 1024, 1>, pto::Layout::ND>(v1 + (v7 + (unsigned) v3 * (unsigned) v12 + (unsigned) v15 * (unsigned) v11), v25, v26);
    wait_flag(PIPE_MTE2, PIPE_MTE3, EVENT_ID0);
    TSTORE(v27, v17);
    set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  }
  wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
