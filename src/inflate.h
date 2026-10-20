// inflate.h —— 裸 DEFLATE 解压 (zip 中 method=8), 自包含实现, 无第三方依赖
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace k1d500 {

// 解压 in[0..inLen) 的裸 DEFLATE 流到 out。
// expectedSize 非 0 时会校验解压长度 (防损坏)。
bool InflateRaw(const uint8_t* in, size_t inLen, size_t expectedSize,
                std::vector<uint8_t>& out, std::string& err);

}  // namespace k1d500
