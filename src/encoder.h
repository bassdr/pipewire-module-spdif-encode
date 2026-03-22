#pragma once

#include <cstddef>
#include <cstdint>

// Encoder result from EncodeFrame
struct EncodeResult
{
    int BytesWritten;  // -1 on error
};
