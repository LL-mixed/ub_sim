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

__global__ AICORE void prefill_q_pad(__gm__ bfloat16_t* v1, int32_t v2) {
  unsigned v3 = 0;
  RoundMode v4 = RoundMode::CAST_ROUND;
  const int32_t v5 = 5;
  const int32_t v6 = 16;
  const float v7 = 0.0f;
  const int32_t v8 = 11;
  const int32_t v9 = 8;
  const int32_t v10 = 0;
  const int32_t v11 = 1;
  const int32_t v12 = 128;
  const int64_t v13 = 5632;
  const int64_t v14 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  for (size_t v15 = (size_t) v10; v15 < ((size_t) v9); v15 += (size_t) v11) {
    int32_t v16 = (int32_t) ((uint32_t) ((int32_t) (uint32_t) v2 * (uint32_t) v9) + (uint32_t) ((int32_t) v15));
    __gm__ bfloat16_t* v17;
    if (v16 < v9) {
      Tile<TileType::Vec, float, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v18 = Tile<TileType::Vec, float, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v12);
      TASSIGN(v18, v14);
      Tile<TileType::Vec, float, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v19 = Tile<TileType::Vec, float, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v12);
      __ubuf__ float* v20 = v18.data();
      uint64_t v21 = reinterpret_cast<uint64_t>(v20);
      TASSIGN(v19, v21);
      pipe_barrier(PIPE_V);
      TEXPANDS(v19, v7);
      Tile<TileType::Vec, bfloat16_t, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v22 = Tile<TileType::Vec, bfloat16_t, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v12);
      TASSIGN(v22, v13);
      Tile<TileType::Vec, bfloat16_t, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v23 = Tile<TileType::Vec, bfloat16_t, 11, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v12);
      __ubuf__ bfloat16_t* v24 = v22.data();
      uint64_t v25 = reinterpret_cast<uint64_t>(v24);
      TASSIGN(v23, v25);
      pipe_barrier(PIPE_V);
      wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
      TCVT(v23, v19, v4);
      set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
      pto::Shape<1, 1, 1, 11, 128> v26 = pto::Shape<1, 1, 1, 11, 128>();
      pto::Stride<1408, 1408, 1408, 128, 1> v27 = pto::Stride<1408, 1408, 1408, 128, 1>();
      GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 11, 128>, pto::Stride<1408, 1408, 1408, 128, 1>, pto::Layout::ND> v28 = GlobalTensor<bfloat16_t, pto::Shape<1, 1, 1, 11, 128>, pto::Stride<1408, 1408, 1408, 128, 1>, pto::Layout::ND>(v1 + (v3 + (unsigned) ((int32_t) (uint32_t) ((int32_t) (uint32_t) v16 * (uint32_t) v6) + (uint32_t) v5) * (unsigned) v12 + v3 * (unsigned) v11), v26, v27);
      wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
      TSTORE(v28, v23);
      set_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
      v17 = v1;
    } else {
      v17 = v1;
    };
  }
  wait_flag(PIPE_MTE3, PIPE_V, EVENT_ID0);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
