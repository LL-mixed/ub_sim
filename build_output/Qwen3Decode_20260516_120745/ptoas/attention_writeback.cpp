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

__global__ AICORE void attention_writeback(__gm__ bfloat16_t* v1, __gm__ bfloat16_t* v2) {
  unsigned v3 = 0;
  const int32_t v4 = 128;
  const int32_t v5 = 256;
  const int32_t v6 = 2;
  const int32_t v7 = 8;
  const int32_t v8 = 0;
  const int32_t v9 = 16384;
  const int32_t v10 = 2048;
  const int32_t v11 = 1;
  const int64_t v12 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  for (size_t v13 = (size_t) v8; v13 < ((size_t) v7); v13 += (size_t) v11) {
    int32_t v14 = (int32_t) v13;
    Tile<TileType::Vec, bfloat16_t, 1, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v15 = Tile<TileType::Vec, bfloat16_t, 1, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v5);
    TASSIGN(v15, v12);
    Tile<TileType::Vec, bfloat16_t, 1, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Vec, bfloat16_t, 1, 256, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v11, v5);
    __ubuf__ bfloat16_t* v17 = v15.data();
    uint64_t v18 = reinterpret_cast<uint64_t>(v17);
    TASSIGN(v16, v18);
    pto::Shape<1, 1, 1, 1, 256> v19 = pto::Shape<1, 1, 1, 1, 256>();
    pto::Stride<16384, 16384, 16384, 16384, 1> v20 = pto::Stride<16384, 16384, 16384, 16384, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 1, 256>, pto::Stride<16384, 16384, 16384, 16384, 1>, pto::Layout::ND> v21 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 1, 256>, pto::Stride<16384, 16384, 16384, 16384, 1>, pto::Layout::ND>(v2 + (v3 + v3 * (unsigned) v9 + (unsigned) ((int32_t) (uint32_t) v14 * (uint32_t) v10) * (unsigned) v11), v19, v20);
    wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
    TLOAD(v16, v21);
    set_flag(PIPE_MTE2, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 1, 256> v22 = pto::Shape<1, 1, 1, 1, 256>();
    pto::Stride<2048, 2048, 2048, 2048, 1> v23 = pto::Stride<2048, 2048, 2048, 2048, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 1, 256>, pto::Stride<2048, 2048, 2048, 2048, 1>, pto::Layout::ND> v24 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 1, 256>, pto::Stride<2048, 2048, 2048, 2048, 1>, pto::Layout::ND>(v1 + (v3 + v3 * (unsigned) v10 + (unsigned) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v14 * (uint32_t) v6) * (uint32_t) v4) * (unsigned) v11), v22, v23);
    wait_flag(PIPE_MTE2, PIPE_MTE3, EVENT_ID0);
    TSTORE(v24, v16);
    set_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  }
  wait_flag(PIPE_MTE3, PIPE_MTE2, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
