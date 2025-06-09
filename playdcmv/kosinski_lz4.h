/**
 * @file kosinski_lz4.h
 * @brief SH-4 Optimized LZ4 HC Decompressor for Dreamcast
 * @author Troy Davis (GPF) (DeepSeek - Vibe Coding)
 * @date 2025-06-07
 * @version 1.0.0
 * 
 * @license BSD 2-Clause "Simplified" License
 * @copyright 2025 Troy Davis (DeepSeek - Vibe Coding)
 * 
 * @note Inspired by Sega Kosinski compression principles
 * @credits LZ4 HC by Yann Collet (BSD license)
 * 
 * Feature Highlights:
 * - Cycle-accurate SH-4 DMA optimizations
 * - LZ4 HC Level 12 decompression
 * - RGB565 VQ texture specialization
  * 
 * @license BSD 2-Clause License
 * 
 * Copyright (c) 2023, Dreamcast Community
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, 
 *    this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once
#include <stdint.h>

/**
 * @brief Decompression context structure
 * 
 * Contains SH-4 optimized workspace buffers. Must be initialized with 
 * LZ4_DC_init() before first use. Can be reused across multiple decompressions.
 */
typedef struct {
    uint32_t workspace[256]; ///< Cache-aligned workspace (1KB)
} LZ4_DC_Stream;

/**
 * @brief Initialize decompression context
 * @param ctx Pointer to context structure
 * 
 * Prepares the context for first use. Safe to call multiple times.
 * Zero-initializes internal buffers and primes SH-4 cache.
 */
void LZ4_DC_init(LZ4_DC_Stream* ctx);

/**
 * @brief Decompress LZ4 HC data
 * @param ctx Initialized context pointer
 * @param src Source buffer (compressed data)
 * @param dst Destination buffer (must be pre-allocated)
 * @param src_size Size of compressed data in bytes
 * @param dst_capacity Capacity of destination buffer
 * @return Actual decompressed size, or -1 on error
 * 
 * Features:
 * - Handles LZ4 HC compressed data (level 12 recommended)
 * - Requires 32-byte aligned buffers for SH-4 DMA
 * - Includes full bounds checking
 * - Thread-safe when using separate contexts
 * 
 * Usage Example:
 * @code
 * LZ4_DC_Stream ctx;
 * LZ4_DC_init(&ctx);
 * int size = LZ4_DC_decompressHC_safest_fast(&ctx, compressed, output, comp_size, max_size);
 * @endcode
 */
int LZ4_DC_decompressHC_safest_fast(
    LZ4_DC_Stream* ctx,
    const uint8_t* ip,
    uint8_t* dst,
    int src_size,
    int dst_capacity
    );