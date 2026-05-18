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

__global__ AICORE void online_softmax_init(__gm__ float* v1, __gm__ float* v2, __gm__ float* v3) {
  unsigned v4 = 0;
  const float v5 = 0.0f;
  const int32_t v6 = 1;
  const int32_t v7 = 128;
  const int32_t v8 = 8;
  const int64_t v9 = 4128;
  const int64_t v10 = 4096;
  const int64_t v11 = 0;
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  Tile<TileType::Vec, float, 8, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v12 = Tile<TileType::Vec, float, 8, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v7);
  TASSIGN(v12, v11);
  Tile<TileType::Vec, float, 8, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v13 = Tile<TileType::Vec, float, 8, 128, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v7);
  __ubuf__ float* v14 = v12.data();
  uint64_t v15 = reinterpret_cast<uint64_t>(v14);
  TASSIGN(v13, v15);
  TEXPANDS(v13, v5);
  set_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
  Tile<TileType::Vec, float, 1, 8, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v16 = Tile<TileType::Vec, float, 1, 8, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v6, v8);
  TASSIGN(v16, v10);
  Tile<TileType::Vec, float, 1, 8, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v17 = Tile<TileType::Vec, float, 1, 8, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v6, v8);
  __ubuf__ float* v18 = v16.data();
  uint64_t v19 = reinterpret_cast<uint64_t>(v18);
  TASSIGN(v17, v19);
  TEXPANDS(v17, v5);
  set_flag(PIPE_V, PIPE_MTE3, EVENT_ID1);
  Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v20 = Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v6);
  TASSIGN(v20, v10);
  Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v21 = Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v6);
  __ubuf__ float* v22 = v20.data();
  uint64_t v23 = reinterpret_cast<uint64_t>(v22);
  TASSIGN(v21, v23);
  Tile<TileType::Vec, float, 1, 8, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v24 = Tile<TileType::Vec, float, 1, 8, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v6, v8);
  TASSIGN(v24, v9);
  Tile<TileType::Vec, float, 1, 8, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v25 = Tile<TileType::Vec, float, 1, 8, BLayout::RowMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v6, v8);
  __ubuf__ float* v26 = v24.data();
  uint64_t v27 = reinterpret_cast<uint64_t>(v26);
  TASSIGN(v25, v27);
  TEXPANDS(v25, v5);
  set_flag(PIPE_V, PIPE_MTE3, EVENT_ID2);
  Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v28 = Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v6);
  TASSIGN(v28, v9);
  Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null> v29 = Tile<TileType::Vec, float, 8, 1, BLayout::ColMajor, -1, -1, SLayout::NoneBox, 512, PadValue::Null, CompactMode::Null>(v8, v6);
  __ubuf__ float* v30 = v28.data();
  uint64_t v31 = reinterpret_cast<uint64_t>(v30);
  TASSIGN(v29, v31);
  pto::Shape<1, 1, 1, 8, 128> v32 = pto::Shape<1, 1, 1, 8, 128>();
  pto::Stride<1024, 1024, 1024, 128, 1> v33 = pto::Stride<1024, 1024, 1024, 128, 1>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 8, 128>, pto::Stride<1024, 1024, 1024, 128, 1>, pto::Layout::ND> v34 = GlobalTensor<float, pto::Shape<1, 1, 1, 8, 128>, pto::Stride<1024, 1024, 1024, 128, 1>, pto::Layout::ND>(v1 + (v4 + v4 * (unsigned) v7 + v4 * (unsigned) v6), v32, v33);
  wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID0);
  TSTORE(v34, v13);
  pto::Shape<1, 1, 1, 8, 1> v35 = pto::Shape<1, 1, 1, 8, 1>();
  pto::Stride<8, 8, 8, 1, 8> v36 = pto::Stride<8, 8, 8, 1, 8>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 8, 1>, pto::Stride<8, 8, 8, 1, 8>, pto::Layout::DN> v37 = GlobalTensor<float, pto::Shape<1, 1, 1, 8, 1>, pto::Stride<8, 8, 8, 1, 8>, pto::Layout::DN>(v2 + (v4 + v4 * (unsigned) v6 + v4 * (unsigned) v8), v35, v36);
  wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID1);
  TSTORE(v37, v21);
  pto::Shape<1, 1, 1, 8, 1> v38 = pto::Shape<1, 1, 1, 8, 1>();
  pto::Stride<8, 8, 8, 1, 8> v39 = pto::Stride<8, 8, 8, 1, 8>();
  GlobalTensor<float, pto::Shape<1, 1, 1, 8, 1>, pto::Stride<8, 8, 8, 1, 8>, pto::Layout::DN> v40 = GlobalTensor<float, pto::Shape<1, 1, 1, 8, 1>, pto::Stride<8, 8, 8, 1, 8>, pto::Layout::DN>(v3 + (v4 + v4 * (unsigned) v6 + v4 * (unsigned) v8), v38, v39);
  wait_flag(PIPE_V, PIPE_MTE3, EVENT_ID2);
  TSTORE(v40, v29);
  #endif // __DAV_VEC__

  ptoas_auto_sync_tail(PTOAutoSyncTailMode::kBarrierAll);
  return;
}
