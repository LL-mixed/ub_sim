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

__global__ AICORE void decode_q_pad(__gm__ bfloat16_t* v1) {
  unsigned v2 = 0;
  RoundMode v3 = RoundMode::CAST_ROUND;
  const int32_t v4 = 5;
  const int32_t v5 = 16;
  const float v6 = 0.0f;
  const int32_t v7 = 11;
  const int32_t v8 = 0;
  const int32_t v9 = 1;
  const int32_t v10 = 128;
  const int64_t v11 = 5632;
  const int64_t v12 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  for (size_t v13 = (size_t) v8; v13 < ((size_t) v10); v13 += (size_t) v9) {
    Tile<TileType::Vec, float, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v14 = Tile<TileType::Vec, float, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v7, v10);
    TASSIGN(v14, v12);
    Tile<TileType::Vec, float, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v15 = Tile<TileType::Vec, float, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v7, v10);
    __ubuf__ float* v16 = v14.data();
    uint64_t v17 = reinterpret_cast<uint64_t>(v16);
    TASSIGN(v15, v17);
    pipe_barrier(PIPE_V);
    TEXPANDS(v15, v6);
    Tile<TileType::Vec, bfloat16_t, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v18 = Tile<TileType::Vec, bfloat16_t, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v7, v10);
    TASSIGN(v18, v11);
    Tile<TileType::Vec, bfloat16_t, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v19 = Tile<TileType::Vec, bfloat16_t, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v7, v10);
    __ubuf__ bfloat16_t* v20 = v18.data();
    uint64_t v21 = reinterpret_cast<uint64_t>(v20);
    TASSIGN(v19, v21);
    pipe_barrier(PIPE_V);
    wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
    TCVT(v19, v15, v3);
    set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    pto::Shape<1, 1, 1, 11, 128> v22 = pto::Shape<1, 1, 1, 11, 128>();
    pto::Stride<1408, 1408, 1408, 128, 1> v23 = pto::Stride<1408, 1408, 1408, 128, 1>();
    GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 11, 128>, pto::Stride<1408, 1408, 1408, 128, 1>, pto::Layout::ND> v24 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 11, 128>, pto::Stride<1408, 1408, 1408, 128, 1>, pto::Layout::ND>(v1 + (v2 + (unsigned) ((int32_t) (uint32_t) ((int32_t) (uint32_t) ((int32_t) v13) * (uint32_t) v5) + (uint32_t) v4) * (unsigned) v10 + v2 * (unsigned) v9), v22, v23);
    wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
    TSTORE(v24, v19);
    set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  }
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
