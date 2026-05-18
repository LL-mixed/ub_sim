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

__global__ AICORE void attention_writeback(__gm__ bfloat16_t* v1, __gm__ float* v2, int32_t v3) {
  RoundMode v4 = RoundMode::CAST_ROUND;
  unsigned v5 = 0;
  const int32_t v6 = 2;
  const int32_t v7 = 0;
  const int32_t v8 = 128;
  const int32_t v9 = 2048;
  const int32_t v10 = 1;
  const int64_t v11 = 512;
  const int64_t v12 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  for (size_t v13 = (size_t) v7; v13 < ((size_t) v6); v13 += (size_t) v10) {
    int32_t v14 = (int32_t) v13;
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v15 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v8);
    TASSIGN(v15, v12);
    Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Vec, float, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v8);
    __ubuf__ float* v17 = v15.data();
    uint64_t v18 = reinterpret_cast<uint64_t>(v17);
    TASSIGN(v16, v18);
    pto::Shape<1, 1, 1, 1, 128> v19 = pto::Shape<1, 1, 1, 1, 128>();
    pto::Stride<128, 128, 128, 128, 1> v20 = pto::Stride<128, 128, 128, 128, 1>();
    GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<128, 128, 128, 128, 1>, pto::Layout::ND> v21 = GlobalTensor<float, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<128, 128, 128, 128, 1>, pto::Layout::ND>(v2 + (v5 + (unsigned) v14 * (unsigned) v8 + v5 * (unsigned) v10), v19, v20);
    wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    TLOAD(v16, v21);
    set_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    Tile<TileType::Vec, bfloat16_t, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v22 = Tile<TileType::Vec, bfloat16_t, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v8);
    TASSIGN(v22, v11);
    Tile<TileType::Vec, bfloat16_t, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v23 = Tile<TileType::Vec, bfloat16_t, 1, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v10, v8);
    __ubuf__ bfloat16_t* v24 = v22.data();
    uint64_t v25 = reinterpret_cast<uint64_t>(v24);
    TASSIGN(v23, v25);
    wait_flag(PIPE_MTE2, PIPE_V, EVENT_ID0);
    wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
    TCVT(v23, v16, v4);
    set_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 1, 128> v26 = pto::Shape<1, 1, 1, 1, 128>();
    pto::Stride<2048, 2048, 2048, 2048, 1> v27 = pto::Stride<2048, 2048, 2048, 2048, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<2048, 2048, 2048, 2048, 1>, pto::Layout::ND> v28 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 1, 128>, pto::Stride<2048, 2048, 2048, 2048, 1>, pto::Layout::ND>(v1 + (v5 + v5 * (unsigned) v9 + (unsigned) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v3 + (uint32_t) v14) * (uint32_t) v8) * (unsigned) v10), v26, v27);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v28, v23);
    set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  }
  wait_flag(PIPE_V, PIPE_MTE2, EVENT_ID0);
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
